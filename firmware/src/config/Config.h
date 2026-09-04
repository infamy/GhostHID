// Runtime configuration, persisted in NVS.
//
// Credentials used to be compile-time -D flags, which meant a full rebuild and
// a reflash to change a Wi-Fi password. They now live in flash and are editable
// over the API, so the compile-time macros in board_config.h serve only as
// first-boot defaults.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ghosthid {

class Config {
public:
    // Field limits. SSID and passphrase caps come from 802.11 itself.
    static constexpr size_t kSsidMax  = 32;
    static constexpr size_t kPassMax  = 64;
    static constexpr size_t kTokenMax = 48;
    static constexpr size_t kNameMax  = 24;

    // Opens NVS and loads stored values, falling back to the build-time
    // defaults for anything never set.
    void begin();

    const char *staSsid()     const { return staSsid_; }
    const char *staPassword() const { return staPass_; }
    const char *apPassword()  const { return apPass_; }
    const char *authToken()   const { return token_; }
    const char *deviceName()  const { return name_; }

    bool stationConfigured() const { return staSsid_[0] != '\0'; }

    // Setters persist immediately. Each returns false and changes nothing if
    // the value is invalid, so a bad edit over the network cannot brick the
    // device's own AP.
    bool setStation(const char *ssid, const char *password);
    bool setApPassword(const char *password);
    bool setAuthToken(const char *token);
    bool setDeviceName(const char *name);

    // Wipes stored settings; the next boot uses build-time defaults.
    void factoryReset();

    // True if a setting has been changed since boot that the radios only read
    // at startup - station credentials, AP password, device name. Lets the UI
    // say "reboot required" as a fact rather than a guess.
    //
    // The pairing token is deliberately excluded: CommandProcessor reads it
    // live on every new session, so a token change takes effect immediately.
    bool rebootPending() const { return rebootPending_; }

    // WPA2 requires 8..63 characters. Refusing a shorter one here is what
    // stops a typo from silently bringing the AP up wide open.
    static bool validApPassword(const char *p);

private:
    char staSsid_[kSsidMax + 1]  = {};
    char staPass_[kPassMax + 1]  = {};
    char apPass_[kPassMax + 1]   = {};
    char token_[kTokenMax + 1]   = {};
    char name_[kNameMax + 1]     = {};
    bool rebootPending_ = false;
};

}  // namespace ghosthid
