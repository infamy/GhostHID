#include "SerialConsole.h"

#include <Arduino.h>
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

}  // namespace

void SerialConsole::begin() {
    Serial.println();
    Serial.println("Type 'help' for the setup console.");
}

void SerialConsole::printHelp() const {
    Serial.println();
    Serial.println("GhostHID setup console");
    Serial.println("  show                 current settings and both addresses");
    Serial.println("  wifi <ssid>          network to JOIN (empty disables joining)");
    Serial.println("  wifipass <password>  station password");
    Serial.println("  appass <password>    password for GhostHID's OWN access point (8-63)");
    Serial.println("  ap always|fallback   keep the AP up, or drop it while joined");
    Serial.println("  kvm <host[:port]>    join a Deskflow/Barrier server as a screen");
    Serial.println("  kvmscreen <name>     this screen's name in the server layout");
    Serial.println("  kvmsize <w> <h>      target's resolution, so the pointer lands right");
    Serial.println("  kvm on|off           enable or disable the screen client");
    Serial.println("  token <token>        pairing token (empty value disables auth)");
    Serial.println("  name <name>          device name - sets the AP SSID and mDNS name");
    Serial.println("  reset                erase all settings");
    Serial.println("  reboot               restart to apply changes");
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

    if (strcasecmp(line, "help") == 0 || strcmp(line, "?") == 0) {
        printHelp();
    } else if (strcasecmp(line, "show") == 0 || strcasecmp(line, "status") == 0) {
        printStatus();
    } else if (strcasecmp(line, "wifi") == 0) {
        if (config_.setStation(value, config_.staPassword())) {
            Serial.printf("ok: wifi = %s  -- type 'reboot' to apply\r\n",
                          value[0] ? value : "(disabled)");
        } else {
            Serial.println("error: ssid too long (max 32)");
        }
    } else if (strcasecmp(line, "wifipass") == 0) {
        if (config_.setStation(config_.staSsid(), value)) {
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
        if (config_.setAuthToken(value)) {
            Serial.printf("ok: token %s  (active now, no reboot needed)\r\n",
                          value[0] ? "set" : "cleared - auth disabled");
        } else {
            Serial.println("error: token too long (max 48)");
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
    } else if (strcasecmp(line, "reset") == 0) {
        config_.factoryReset();
        Serial.println("ok: settings erased - reboot to apply build-time defaults");
    } else if (strcasecmp(line, "reboot") == 0) {
        Serial.println("rebooting...");
        processor_.requestReboot();
    } else {
        Serial.printf("unknown command '%s' - try 'help'\r\n", line);
    }
}

void SerialConsole::feed() {
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
