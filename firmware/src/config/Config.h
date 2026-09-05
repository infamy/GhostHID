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
    static constexpr size_t kHostMax  = 63;

    // Opens NVS and loads stored values, falling back to the build-time
    // defaults for anything never set.
    void begin();

    const char *staSsid()     const { return staSsid_; }
    const char *staPassword() const { return staPass_; }
    const char *apPassword()  const { return apPass_; }
    const char *authToken()   const { return token_; }
    const char *deviceName()  const { return name_; }

    bool stationConfigured() const { return staSsid_[0] != '\0'; }

    // true (default) - the access point stays up permanently.
    // false - the AP shuts down once the station connects, and returns by
    //         itself if that connection is lost.
    //
    // Defaults to always-on because measurement showed AP+STA sharing is NOT
    // the latency cost it was assumed to be: with the AP dropped, spikes got
    // slightly worse (5.0% -> 7.5% over 60ms), and pinging the router over the
    // same link showed the same ~150ms tail. The spikes are the Wi-Fi
    // environment, not this radio. So the fallback is kept for the cases where
    // it genuinely helps - a congested band, or a deployment that wants one
    // less radio surface - rather than being the default.
    bool apAlways() const { return apAlways_; }
    bool setApAlways(bool always);

    // --- Deskflow / Barrier / Input Leap screen client ----------------------
    // The advertised width and height are the coordinate space the server
    // addresses this screen in, so they should match the target's real
    // resolution or the pointer will land in the wrong place.
    bool        deskflowEnabled() const { return dfEnabled_; }
    const char *deskflowHost()    const { return dfHost_; }
    uint16_t    deskflowPort()    const { return dfPort_; }
    const char *deskflowScreen()  const { return dfScreen_; }
    uint16_t    deskflowWidth()   const { return dfWidth_; }
    uint16_t    deskflowHeight()  const { return dfHeight_; }
    // Deskflow/Barrier/Synergy ship with encryption on, so this defaults true.
    bool        deskflowTls()     const { return dfTls_; }

    // The server's own certificate, pinned. Not optional in practice:
    // arduino-esp32 only loads a client certificate when verification is
    // enabled, so skipping verification also silently drops our identity - and
    // these servers require mutual TLS. Pinning is the protocol's own trust
    // model anyway, so this is the right thing rather than a workaround.
    const char *deskflowServerCert() const { return dfCa_ ? dfCa_ : ""; }
    bool setDeskflowServerCert(const char *pem);

    // Crash guard. The screen client's first TLS handshake happens during boot,
    // before the web server starts, because it needs an unfragmented heap. If
    // that crashes the device it would loop forever with no way in over the
    // network - so a marker is written before the attempt and cleared after.
    // Finding it still set at boot means the last attempt did not survive.
    bool deskflowAttemptPending() const { return dfPending_; }
    void markDeskflowAttempt(bool inProgress);

    bool setDeskflowServer(const char *host, uint16_t port);
    bool setDeskflowScreen(const char *name);
    bool setDeskflowScreenSize(uint16_t w, uint16_t h);
    bool setDeskflowEnabled(bool on);

    // When false, the web server is NOT STARTED at all while the screen client
    // is enabled - a memory-lean mode managed from the serial console.
    //
    // Not starting it is the only thing that works: AsyncTCP 3.5.0 never tears
    // its task down, so stopping the server later frees nothing (measured: 92
    // bytes). Skipping it keeps the largest contiguous block near its 135KB
    // boot value instead of dropping to ~47KB, which is the difference between
    // a TLS session leaving 13KB and leaving something comfortable.
    bool webWhenKvm() const { return webWhenKvm_; }
    bool setWebWhenKvm(bool on);
    bool setDeskflowTls(bool on);

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
    bool apAlways_ = false;
    bool     dfEnabled_ = false;
    char     dfHost_[64]   = {};
    uint16_t dfPort_       = 24800;
    char     dfScreen_[32] = {};
    uint16_t dfWidth_      = 1920;
    uint16_t dfHeight_     = 1080;
    bool     dfTls_        = true;
    bool     dfPending_    = false;
    bool     webWhenKvm_   = true;
    char    *dfCa_         = nullptr;   // heap: a PEM is too big for a member
    bool rebootPending_ = false;
};

}  // namespace ghosthid
