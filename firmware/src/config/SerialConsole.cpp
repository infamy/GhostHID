#include "SerialConsole.h"

#include <Arduino.h>
#include <soc/rtc_cntl_reg.h>
#include <string.h>

#include "Config.h"
#include "board_config.h"
#include "net/DeskflowClient.h"
#include "net/Network.h"
#include "protocol/CommandProcessor.h"

namespace ghosthid {
namespace {

// Splits "verb rest of the line" into verb and the remainder. The remainder is
// taken verbatim rather than tokenised, because SSIDs and passwords routinely
// contain spaces.
char *splitVerb(char *line) {
    char *sp = strchr(line, ' ');
    if (sp == nullptr) return nullptr;
    *sp = '\0';
    char *rest = sp + 1;
    while (*rest == ' ') ++rest;
    return rest;
}

// Remove whitespace from a token. It is shown on the LCD in two 4-char blocks
// ("AB2C 9XKF"), so accept it typed with or without the space. Real tokens are
// uppercase letters and digits only - never whitespace.
void stripWs(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0' && o + 1 < cap; ++i) {
        if (in[i] == ' ' || in[i] == '\t') continue;
        out[o++] = in[i];
    }
    out[o] = '\0';
}

// Constant-time compare for the unlock token (no early return).
bool ctEq(const char *a, const char *b) {
    const size_t la = strlen(a), lb = strlen(b);
    unsigned char d = static_cast<unsigned char>(la ^ lb);
    const size_t n = la > lb ? la : lb;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char ca = i < la ? static_cast<unsigned char>(a[i]) : 0;
        const unsigned char cb = i < lb ? static_cast<unsigned char>(b[i]) : 0;
        d |= static_cast<unsigned char>(ca ^ cb);
    }
    return d == 0;
}

}  // namespace

void SerialConsole::begin() {
    // While sealed the USB CDC interface is not enumerated at all (main() drops
    // it at boot), so there is nothing to greet. Stay silent.
    if (config_.sealed()) return;
    Serial.println();
    Serial.println("Type 'help' for the setup console.");
}

void SerialConsole::armUnseal() {
    if (!config_.sealed() || unsealArmed_) return;
    unsealArmed_ = true;
    // Serial was just brought back by main() on the BOOT hold. Greet so the user
    // can see the port is live and knows the two remaining steps.
    Serial.println();
    Serial.println("*** BOOT held - serial console re-enabled ***");
    Serial.println("This device is SEALED. To unseal:");
    Serial.println("  1) unlock <token>");
    Serial.println("  2) unseal");
    Serial.println("Reboot or unplug without unsealing returns to HID-only (no serial).");
    Serial.print("> ");
}

void SerialConsole::printHelp() const {
    Serial.println();
    Serial.println("GhostHID setup console");
    Serial.println("  show                 current settings and both addresses");
    Serial.println("  wifi <ssid>          network to JOIN (empty disables joining)");
    Serial.println("  wifipass <password>  password to JOIN that network (station)");
    Serial.println("  appass <password>    password for GhostHID's OWN access point (8-63)");
    Serial.println("  ap always|fallback   keep the AP up, or drop it while joined");
    Serial.println("  kvm <host[:port]>    join a Deskflow/Barrier server as a screen");
    Serial.println("  kvmscreen <name>     this screen's name in the server layout");
    Serial.println("  kvmsize <w> <h>      target's resolution, so the pointer lands right");
    Serial.println("  kvm on|off           enable or disable the screen client");
    Serial.println("  trustcert            pin the server cert captured on first connect");
    Serial.println("  web off|on           start or stop the web UI now (this boot only)");
    Serial.println("  heap                 free and largest-block memory");
    Serial.println("  token <token>        pairing token (empty disables auth; else 6-48)");
    Serial.println("  unlock <token>       same pairing token as 'token'; allows");
    Serial.println("                       wifi/token/reset changes this boot");
    Serial.println("  name <name>          device name - sets the AP SSID and mDNS name");
    Serial.println("  seal                 lock down: HID-only USB, no serial, no net");
    Serial.println("                       config/OTA, no plaintext KVM (needs unlock)");
    Serial.println("  unseal               undo seal (needs a ~5s BOOT hold, then unlock)");
    Serial.println("  reset                erase all settings");
    Serial.println("  reboot               restart to apply changes");
    Serial.println("  bootloader           reboot into USB download mode for flashing");
    Serial.println();
    Serial.println("'always' (default) keeps the AP up alongside the joined network, so a");
    Serial.println("bad config can never lock you out. 'fallback' drops it while joined and");
    Serial.println("restores it if that connection is lost - measured as no faster here, but");
    Serial.println("useful on a congested band or to reduce radio surface.");
    Serial.println();
    Serial.println("Values are taken verbatim to end of line, so spaces are fine.");
    Serial.println("Changes save immediately but take effect on reboot.");
}

void SerialConsole::printStatus() const {
    // GhostHID runs BOTH radios at once, and reporting only the station half
    // made that genuinely confusing. Spell out both, and which one is optional.
    Serial.printf("\r\n  GhostHID %s   device name: %s\r\n",
                  GHOSTHID_VERSION, config_.deviceName());

    Serial.printf("\r\n  Own access point        %s\r\n",
                  config_.apAlways() ? "ALWAYS ON"
                                     : (network_.apActive() ? "UP (no network joined)"
                                                            : "STANDBY - returns if the network drops"));
    Serial.printf("    ssid      %s\r\n", network_.ssid());
    if (network_.apActive()) {
        Serial.printf("    address   http://%s/\r\n", network_.apAddress());
    } else {
        Serial.printf("    address   -   (radio dedicated to the joined network)\r\n");
    }
    Serial.printf("    password  (set)\r\n");

    Serial.printf("\r\n  Joined network          OPTIONAL\r\n");
    if (!config_.stationConfigured()) {
        Serial.printf("    ssid      (not set - use: wifi <ssid>)\r\n");
        Serial.printf("    password  (not set - use: wifipass <password>)\r\n");
        Serial.printf("    address   -\r\n");
    } else {
        Serial.printf("    ssid      %s\r\n", config_.staSsid());
        Serial.printf("    password  %s\r\n",
                      config_.staPassword()[0] ? "(set)" : "(NOT SET)");
        if (network_.stationConnected()) {
            Serial.printf("    address   http://%s/\r\n", network_.staAddress());
        } else if (config_.rebootPending()) {
            // We know the settings changed since boot, so this is not a
            // failure - the radio simply has not been restarted yet. Saying
            // "maybe" here sent people chasing passwords that were fine.
            Serial.printf("    address   NOT APPLIED YET - type 'reboot'\r\n");
        } else {
            Serial.printf("    address   FAILED to connect "
                          "(wrong password, or out of range)\r\n");
        }
    }

    Serial.printf("\r\n  Screen client (Deskflow/Barrier)  %s\r\n",
                  config_.deskflowEnabled() ? "ENABLED" : "off");
    if (config_.deskflowEnabled()) {
        Serial.printf("    server    %s:%u\r\n",
                      config_.deskflowHost()[0] ? config_.deskflowHost() : "(not set)",
                      (unsigned)config_.deskflowPort());
        Serial.printf("    screen    %s   %ux%u\r\n", config_.deskflowScreen(),
                      (unsigned)config_.deskflowWidth(), (unsigned)config_.deskflowHeight());
        Serial.printf("    state     %s\r\n", deskflow_.statusText());
    }

    Serial.printf("\r\n  Pairing token  %s   (applies immediately, no reboot)\r\n",
                  config_.authToken()[0] ? "(set)" : "(none - auth disabled)");
    if (config_.rebootPending()) {
        Serial.printf("\r\n  *** REBOOT REQUIRED - settings changed since boot ***\r\n");
    }
    Serial.printf("  USB to target  %s\r\n\r\n",
                  "see the web UI's USB badge");
}

void SerialConsole::execute(char *line) {
    while (*line == ' ') ++line;
    if (*line == '\0') return;

    char *arg = splitVerb(line);
    const char *value = (arg != nullptr) ? arg : "";

    // The console is reachable by the target host across the USB seam. Once a
    // token is set, commands that change an already-set security setting require
    // `unlock <token>` first (M1). Bootstrap stays open: with no token set, or a
    // blank station, first-run configuration over the cable is ungated.
    const bool locked = (config_.authToken()[0] != '\0') && !consoleUnlocked_;

    if (strcasecmp(line, "help") == 0 || strcmp(line, "?") == 0) {
        printHelp();
    } else if (strcasecmp(line, "show") == 0 || strcasecmp(line, "status") == 0) {
        printStatus();
    } else if (strcasecmp(line, "unlock") == 0) {
        char tok[64]; stripWs(value, tok, sizeof(tok));
        if (config_.authToken()[0] == '\0') {
            Serial.println("console is already open (no token set)");
        } else if (ctEq(tok, config_.authToken())) {
            consoleUnlocked_ = true;
            Serial.println("ok: console unlocked for this boot");
        } else {
            Serial.println("error: wrong token");
        }
    } else if (strcasecmp(line, "wifi") == 0) {
        if (locked && config_.stationConfigured()) {
            Serial.println("locked: run 'unlock <token>' first");
        } else if (config_.setStation(value, config_.staPassword())) {
            Serial.printf("ok: wifi = %s  -- type 'reboot' to apply\r\n",
                          value[0] ? value : "(disabled)");
        } else {
            Serial.println("error: ssid too long (max 32)");
        }
    } else if (strcasecmp(line, "wifipass") == 0) {
        if (locked && config_.stationConfigured()) {
            Serial.println("locked: run 'unlock <token>' first");
        } else if (config_.setStation(config_.staSsid(), value)) {
            Serial.println("ok: wifi password set  -- type 'reboot' to apply");
        } else {
            Serial.println("error: password too long (max 64)");
        }
    } else if (strcasecmp(line, "appass") == 0) {
        if (config_.setApPassword(value)) {
            Serial.println("ok: ap password set  -- type 'reboot' to apply");
        } else {
            Serial.println("error: WPA2 requires 8-63 characters");
        }
    } else if (strcasecmp(line, "token") == 0) {
        char tok[64]; stripWs(value, tok, sizeof(tok));
        if (locked) {
            Serial.println("locked: run 'unlock <token>' first");
        } else if (config_.setAuthToken(tok)) {
            Serial.printf("ok: token %s  (active now, no reboot needed)\r\n",
                          tok[0] ? "set" : "cleared - auth disabled");
        } else {
            Serial.println("error: token must be empty (disables auth) or 6-48 chars");
        }
    } else if (strcasecmp(line, "name") == 0) {
        if (config_.setDeviceName(value)) {
            Serial.printf("ok: name = %s  -- type 'reboot' to apply\r\n", value);
        } else {
            Serial.println("error: use letters, digits and hyphens only");
        }
    } else if (strcasecmp(line, "ap") == 0) {
        if (strcasecmp(value, "always") == 0) {
            config_.setApAlways(true);
            Serial.println("ok: access point stays up permanently"
                           "  -- type 'reboot' to apply");
        } else if (strcasecmp(value, "fallback") == 0) {
            config_.setApAlways(false);
            Serial.println("ok: access point drops while the network is joined"
                           "  -- type 'reboot' to apply");
        } else {
            Serial.println("error: use 'ap always' or 'ap fallback'");
        }
    } else if (strcasecmp(line, "kvm") == 0) {
        if (strcasecmp(value, "on") == 0 || strcasecmp(value, "off") == 0) {
            const bool on = (strcasecmp(value, "on") == 0);
            if (on && config_.deskflowHost()[0] == '\0') {
                Serial.println("error: set a server first, e.g. 'kvm 192.168.1.10'");
            } else {
                config_.setDeskflowEnabled(on);
                Serial.printf("ok: screen client %s  (takes effect immediately)\r\n",
                              on ? "enabled" : "disabled");
            }
        } else if (value[0] != '\0') {
            char host[80];
            snprintf(host, sizeof(host), "%s", value);
            uint16_t port = 24800;
            char *colon = strrchr(host, ':');
            if (colon != nullptr) { *colon = '\0'; port = (uint16_t)atoi(colon + 1); }
            if (config_.setDeskflowServer(host, port)) {
                config_.setDeskflowEnabled(true);
                Serial.printf("ok: screen client -> %s:%u, enabled\r\n",
                              config_.deskflowHost(), (unsigned)config_.deskflowPort());
            } else {
                Serial.println("error: bad host or port");
            }
        } else {
            Serial.printf("  server  %s:%u\r\n",
                          config_.deskflowHost()[0] ? config_.deskflowHost() : "(not set)",
                          (unsigned)config_.deskflowPort());
            Serial.printf("  screen  %s\r\n", config_.deskflowScreen());
            Serial.printf("  size    %ux%u\r\n", (unsigned)config_.deskflowWidth(),
                          (unsigned)config_.deskflowHeight());
            Serial.printf("  state   %s\r\n", deskflow_.statusText());
            if (deskflow_.certTrustPending()) {
                Serial.printf("  cert    PENDING - run 'trustcert' to pin\r\n"
                              "          %s\r\n", deskflow_.pendingFingerprint());
            }
        }
    } else if (strcasecmp(line, "trustcert") == 0) {
        // Confirm a captured (trust-on-first-use) server certificate. Gated like
        // reset: pinning a cert is a security change, and a compromised target
        // host reaching the console over USB must not make it (M1).
        if (locked) {
            Serial.println("locked: run 'unlock <token>' first");
        } else if (!deskflow_.certTrustPending()) {
            Serial.println("no certificate is awaiting confirmation");
        } else {
            Serial.printf("ok: pinning server certificate\r\n      %s\r\n",
                          deskflow_.pendingFingerprint());
            deskflow_.trustPendingCert();
        }
    } else if (strcasecmp(line, "kvmscreen") == 0) {
        if (config_.setDeskflowScreen(value)) Serial.printf("ok: screen name = %s\r\n", value);
        else Serial.println("error: name must be 1-31 characters");
    } else if (strcasecmp(line, "kvmsize") == 0) {
        unsigned w = 0, h = 0;
        if (sscanf(value, "%u %u", &w, &h) == 2 &&
            config_.setDeskflowScreenSize((uint16_t)w, (uint16_t)h)) {
            Serial.printf("ok: screen size = %ux%u\r\n", w, h);
        } else {
            Serial.println("error: use 'kvmsize 1920 1080' (320-16384 each)");
        }
    } else if (strcasecmp(line, "heap") == 0) {
        Serial.printf("  free %u, largest block %u\r\n",
                      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    } else if (strcasecmp(line, "web") == 0) {
        if (strcasecmp(value, "off") == 0) {
            network_.stopServers();
        } else if (strcasecmp(value, "on") == 0) {
            network_.beginServers();
            Serial.printf("  free %u, largest block %u\r\n",
                          (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
        } else {
            Serial.printf("  web server is %s\r\n",
                          network_.serversRunning() ? "running" : "stopped");
        }
    } else if (strcasecmp(line, "reset") == 0) {
        if (locked) {
            Serial.println("locked: run 'unlock <token>' first");
        } else {
            config_.factoryReset();
            Serial.println("ok: settings erased - reboot to apply build-time defaults");
        }
    } else if (strcasecmp(line, "seal") == 0) {
        // Lock the device down. Token-gated like every other security change so a
        // target host on the USB seam can't seal the device against its owner.
        if (locked) {
            Serial.println("locked: run 'unlock <token>' first");
        } else if (config_.sealed()) {
            Serial.println("already sealed");
        } else {
            config_.setSealed(true);
            Serial.println("ok: sealing. The device reboots HID-only with NO serial console.");
            Serial.println("    To get back in: hold BOOT ~5s to re-enable serial, then");
            Serial.println("    'unlock <token>' and 'unseal'. Rebooting...");
            Serial.flush();
            delay(250);
            esp_restart();
        }
    } else if (strcasecmp(line, "unseal") == 0) {
        // Two factors, both required: the physical BOOT hold (unsealArmed_) that
        // re-enabled this console, and the token (unlock). Neither alone unseals.
        if (!config_.sealed()) {
            Serial.println("not sealed");
        } else if (!unsealArmed_) {
            Serial.println("locked: hold the BOOT button ~5s first to arm unseal");
        } else if (locked) {
            Serial.println("locked: run 'unlock <token>' first");
        } else {
            config_.setSealed(false);
            Serial.println("ok: unsealed - rebooting with the serial console restored.");
            Serial.flush();
            delay(250);
            esp_restart();
        }
    } else if (strcasecmp(line, "reboot") == 0) {
        Serial.println("rebooting...");
        processor_.requestReboot();
    } else if (strcasecmp(line, "bootloader") == 0 || strcasecmp(line, "download") == 0) {
        // Reboot straight into the ROM serial bootloader so a flasher can write
        // over USB without the physical BOOT+RST dance. Force-download-boot is a
        // sticky RTC flag the ROM checks on reset. Gated like reset: a target
        // host shouldn't be able to knock the device into download mode at will.
        if (locked) {
            Serial.println("locked: run 'unlock <token>' first");
        } else {
            Serial.println("entering download mode - start your flasher now "
                           "(the port will re-enumerate)");
            Serial.flush();
            delay(200);
            REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
            esp_restart();
        }
    } else {
        Serial.printf("unknown command '%s' - try 'help'\r\n", line);
    }
}

void SerialConsole::feed() {
    // Sealed and not yet armed by a physical BOOT hold: the console is closed.
    // Normally the CDC interface isn't even enumerated (main drops it), so there
    // is nothing to read; this is defence in depth for any path where the port
    // is present anyway. Drain and ignore - no echo, no execution, no prompt.
    if (config_.sealed() && !unsealArmed_) {
        while (Serial.available() > 0) Serial.read();
        return;
    }
    while (Serial.available() > 0) {
        const int c = Serial.read();
        if (c < 0) return;

        if (c == '\r' || c == '\n') {
            if (len_ > 0) {
                line_[len_] = '\0';
                Serial.println();
                execute(line_);
                len_ = 0;
            }
            Serial.print("> ");
        } else if (c == 0x08 || c == 0x7F) {           // backspace / delete
            if (len_ > 0) { --len_; Serial.print("\b \b"); }
        } else if (c >= 0x20 && c < 0x7F) {
            if (len_ < kLineMax - 1) {
                line_[len_++] = static_cast<char>(c);
                Serial.write(static_cast<char>(c));    // local echo
            }
        }
    }
}

}  // namespace ghosthid
