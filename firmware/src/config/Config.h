// Runtime configuration, persisted in NVS.
//
// Credentials used to be compile-time -D flags, which meant a full rebuild and
// a reflash to change a Wi-Fi password. They now live in flash and are editable
// over the API, so the compile-time macros in board_config.h serve only as
// first-boot defaults.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

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

    // True when this boot generated fresh random credentials (see begin()).
    // main() uses it to surface them on the LCD/serial for first-time setup.
    bool justProvisioned() const { return justProvisioned_; }
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

    // Invert the scroll wheel (natural scrolling). Applies to both the web
    // trackpad and the screen-client wheel.
    bool scrollInvert() const { return scrollInvert_; }
    bool setScrollInvert(bool on);

    // Sealed mode. When true the device is locked down for unattended
    // deployment: the USB serial console is dropped from the descriptor
    // (HID-only enumeration), network config writes and OTA are refused, and a
    // plaintext screen-client session is refused. It is a single all-or-nothing
    // switch, persisted here. Cleared by factoryReset() along with everything
    // else - which is fine, because reaching factoryReset() at all needs the
    // same physical-BOOT + token proof that unsealing does (see SealedMode #1).
    bool sealed() const { return sealed_; }
    void setSealed(bool on);

    // One-shot "unseal window". A sealed device has no serial console, and a CDC
    // interface can only be added to the USB descriptor at boot (TinyUSB refuses
    // it once USB has started). So the ~5s BOOT-hold unseal gesture sets this flag
    // and reboots: the next boot brings the console up (see usb_serial.cpp) and
    // arms the `unseal` command. It is consumed (cleared) on that boot, so if the
    // operator doesn't unseal, the following boot returns to HID-only.
    bool unsealWindow() const { return unsealWin_; }
    void setUnsealWindow(bool on);

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
    // Direct pointer to the pinned CA PEM. Only safe to read on a task that
    // cannot race setDeskflowServerCert(); the screen client, which runs on its
    // own task while set_config runs on the network task, must use
    // copyServerCert() instead - a mid-connect free() of this buffer under
    // mbedTLS was a use-after-free.
    const char *deskflowServerCert() const { return dfCa_ ? dfCa_ : ""; }
    bool setDeskflowServerCert(const char *pem);

    // Copies the pinned CA PEM into `dst` under the CA lock, so it is safe to
    // call from another task even while setDeskflowServerCert() is replacing the
    // buffer. Returns the length copied (0 if none / truncated to fit).
    size_t copyServerCert(char *dst, size_t cap) const;

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
    bool scrollInvert_ = false;
    bool sealed_ = false;
    bool unsealWin_ = false;
    bool justProvisioned_ = false;
    bool     dfEnabled_ = false;
    char     dfHost_[64]   = {};
    uint16_t dfPort_       = 24800;
    char     dfScreen_[32] = {};
    uint16_t dfWidth_      = 1920;
    uint16_t dfHeight_     = 1080;
    bool     dfTls_        = true;
    bool     dfPending_    = false;
    char    *dfCa_         = nullptr;   // heap: a PEM is too big for a member
    // Guards dfCa_ against a free()/malloc() on the network task racing a read
    // on the screen-client task. Created lazily so a static Config is valid
    // before begin() runs.
    mutable SemaphoreHandle_t caLock_ = nullptr;
    bool rebootPending_ = false;
};

}  // namespace ghosthid
