#include "SerialConsole.h"

#include <Arduino.h>
#include <string.h>

#include "Config.h"
#include "board_config.h"
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
    Serial.println("  token <token>        pairing token (empty value disables auth)");
    Serial.println("  name <name>          device name - sets the AP SSID and mDNS name");
    Serial.println("  reset                erase all settings");
    Serial.println("  reboot               restart to apply changes");
    Serial.println();
    Serial.println("GhostHID always hosts its own access point. Joining a network is extra,");
    Serial.println("not instead - so a wrong SSID can never lock you out.");
    Serial.println();
    Serial.println("Values are taken verbatim to end of line, so spaces are fine.");
    Serial.println("Changes save immediately but take effect on reboot.");
}

void SerialConsole::printStatus() const {
    // GhostHID runs BOTH radios at once, and reporting only the station half
    // made that genuinely confusing. Spell out both, and which one is optional.
    Serial.printf("\r\n  GhostHID %s   device name: %s\r\n",
                  GHOSTHID_VERSION, config_.deviceName());

    Serial.printf("\r\n  Own access point        ALWAYS ON\r\n");
    Serial.printf("    ssid      %s\r\n", network_.ssid());
    Serial.printf("    address   http://%s/\r\n", network_.apAddress());
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
        } else {
            Serial.printf("    address   not connected "
                          "(wrong password, out of range, or needs reboot)\r\n");
        }
    }

    Serial.printf("\r\n  Pairing token  %s\r\n",
                  config_.authToken()[0] ? "(set)" : "(none - auth disabled)");
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
            Serial.printf("ok: wifi = %s\r\n", value[0] ? value : "(disabled)");
        } else {
            Serial.println("error: ssid too long (max 32)");
        }
    } else if (strcasecmp(line, "wifipass") == 0) {
        if (config_.setStation(config_.staSsid(), value)) {
            Serial.println("ok: wifi password set");
        } else {
            Serial.println("error: password too long (max 64)");
        }
    } else if (strcasecmp(line, "appass") == 0) {
        if (config_.setApPassword(value)) {
            Serial.println("ok: ap password set");
        } else {
            Serial.println("error: WPA2 requires 8-63 characters");
        }
    } else if (strcasecmp(line, "token") == 0) {
        if (config_.setAuthToken(value)) {
            Serial.printf("ok: token %s\r\n", value[0] ? "set" : "cleared (auth disabled)");
        } else {
            Serial.println("error: token too long (max 48)");
        }
    } else if (strcasecmp(line, "name") == 0) {
        if (config_.setDeviceName(value)) {
            Serial.printf("ok: name = %s\r\n", value);
        } else {
            Serial.println("error: use letters, digits and hyphens only");
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
