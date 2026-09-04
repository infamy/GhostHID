#include "SerialConsole.h"

#include <Arduino.h>
#include <string.h>

#include "Config.h"
#include "board_config.h"
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
    Serial.println("  show                 current settings");
    Serial.println("  wifi <ssid>          station SSID (empty value disables station mode)");
    Serial.println("  wifipass <password>  station password");
    Serial.println("  appass <password>    access point password (8-63 chars)");
    Serial.println("  token <token>        pairing token (empty value disables auth)");
    Serial.println("  name <name>          device name, used for mDNS");
    Serial.println("  reset                erase all settings");
    Serial.println("  reboot               restart to apply changes");
    Serial.println();
    Serial.println("Values are taken verbatim to end of line, so spaces are fine.");
    Serial.println("Changes save immediately but take effect on reboot.");
}

void SerialConsole::printStatus() const {
    Serial.println();
    Serial.printf("  version   %s\n", GHOSTHID_VERSION);
    Serial.printf("  name      %s\n", config_.deviceName());
    Serial.printf("  wifi      %s\n",
                  config_.stationConfigured() ? config_.staSsid() : "(station mode disabled)");
    // Secrets are reported as set/unset only, matching the network API. Serial
    // access is physical, but terminals get logged and shoulder-surfed.
    Serial.printf("  wifipass  %s\n", config_.staPassword()[0] ? "(set)" : "(not set)");
    Serial.printf("  appass    (set)\n");
    Serial.printf("  token     %s\n", config_.authToken()[0] ? "(set)" : "(auth disabled)");
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
            Serial.printf("ok: wifi = %s\n", value[0] ? value : "(disabled)");
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
            Serial.printf("ok: token %s\n", value[0] ? "set" : "cleared (auth disabled)");
        } else {
            Serial.println("error: token too long (max 48)");
        }
    } else if (strcasecmp(line, "name") == 0) {
        if (config_.setDeviceName(value)) {
            Serial.printf("ok: name = %s\n", value);
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
        Serial.printf("unknown command '%s' - try 'help'\n", line);
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
