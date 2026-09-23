// A Deskflow / Barrier / Input Leap screen client.
//
// Those projects already solve the hard half of edge-crossing: capturing and
// suppressing input on the controller, working out when the pointer crosses a
// screen edge, multi-monitor layout, and doing it on every desktop OS. Rather
// than reimplement that, GhostHID joins an existing server as just another
// screen - one that happens to need nothing installed on the machine it drives.
//
// Wire protocol (Synergy 1.x lineage, still spoken by Deskflow, Barrier and
// Input Leap): TCP 24800, each message framed by a 4-byte big-endian length,
// then a 4-character code and big-endian integer fields.
//
// Deliberately a synchronous client driven from loop(), not AsyncTCP: we are
// the only consumer, the HID layer blocks briefly on USB anyway, and doing it
// this way keeps flash writes and USB waits out of a network callback.

#pragma once

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include "DeviceIdentity.h"
#include <stdint.h>

namespace ghosthid {

class Config;
class HidDevice;

class DeskflowClient {
public:
    DeskflowClient(HidDevice &hid, Config &config) : hid_(hid), config_(config) {}

    // Starts the client on its own task. It deliberately does NOT run from
    // loop(): a TLS handshake blocks for seconds, and doing that on the main
    // task stalls the web server, starves the WebSocket heartbeat - so a
    // browser decides it has disconnected and reloads - and breaks any OTA
    // upload in flight.
    void begin();

    bool connected() const { return state_ == State::Active; }
    // True while any memory-holding session is up (connecting, handshaking or
    // active). The OTA path waits on this going false after suspend() so it does
    // not begin an update while the ~34KB TLS session is still allocated.
    bool busy() const { return state_ != State::Idle; }
    // True while the server has handed this screen the pointer.
    bool hasFocus() const { return hasFocus_; }
    const char *statusText() const;

    // Counts of what the server actually sends. "Keyboard does nothing" and
    // "keyboard messages never arrive" look identical from the outside.
    uint32_t countMove()  const { return nMove_; }
    uint32_t countKey()   const { return nKey_; }
    uint32_t countBtn()   const { return nBtn_; }
    uint32_t countOther() const { return nOther_; }
    const char *lastUnhandled() const { return lastUnhandled_; }
    // Raw hex of the last key message. Guessing at field offsets from a format
    // string was wrong once already; this shows the actual bytes.
    const char *lastKeyRaw() const { return lastKeyRaw_; }
    const char *lastKeyDownRaw() const { return lastKeyDownRaw_; }
    // Kept separate: the key paths were overwriting the shared field, hiding
    // the very code we needed to see.
    const char *lastOtherRaw() const { return lastOtherRaw_; }

    // Whatever the server called itself in the handshake - "Synergy",
    // "Barrier", "Deskflow". Shown in the UI so the badge names the thing you
    // are actually talking to rather than guessing at the family.
    const char *serverName() const { return serverName_; }

    // This device's TLS fingerprint. The server records it on first connection
    // and matches it thereafter, so it is worth showing the user.
    const char *fingerprint() const { return identity_.fingerprint(); }

    // The device's own certificate, so it can be inspected or handed to a
    // server that wants it up front rather than on first connection.
    const char *certificatePem() const { return identity_.certificatePem(); }
    bool ensureIdentity(const char *cn) { return identity_.begin(cn); }

    // Trust on first use. When TLS is on but no server certificate is pinned,
    // the client makes one probe handshake, captures the server's certificate
    // and stops in a "needs trust" state instead of erroring. The captured
    // fingerprint is surfaced so the user can confirm it out-of-band (compare
    // it to the server's own); trusting it pins the certificate and connects
    // for real. An unconfirmed certificate is NEVER used for a live session -
    // silent auto-accept would let a first-connection MITM win.
    bool certTrustPending() const { return awaitingTrust_; }
    // SHA-256 (lower-case hex) of the captured, not-yet-trusted server cert.
    const char *pendingFingerprint() const { return pendingFp_; }
    // Confirm the captured certificate: pin it and reconnect. No-op if nothing
    // is pending. Called from another task; acted on by serviceOnce().
    void trustPendingCert() { trustReq_ = true; }

    // Compact state for the heartbeat: 0 off, 1 connecting, 2 connected,
    // 3 connected and holding the pointer.
    uint8_t stateCode() const;

    // Drops any current session so the next loop() reconnects with whatever
    // settings are now stored. Called after the server details are edited.
    void reconnect();

    // Drops the session and stays down until re-enabled. Used before a firmware
    // update: the session holds ~34KB and an update needs a large buffer, and
    // the two together are what crashed a device mid-use.
    void suspend();

#ifdef GHOSTHID_NATIVE_TEST
    // Host-test only (never compiled into device firmware): feed one raw protocol
    // message straight to the interpreter so dispatch() can be unit-tested.
    void test_dispatch(const uint8_t *m, size_t len) { dispatch(m, len); }
    void test_flushPointer() { flushPointer(); }
#endif

private:
    enum class State : uint8_t { Idle, Connecting, Handshaking, Active };

    bool readExactly(uint8_t *dst, size_t len, uint32_t timeoutMs);
    bool readMessage(uint8_t *buf, size_t cap, size_t &outLen);
    void sendMessage(const uint8_t *payload, size_t len);
    void sendCode(const char *code);
    void handshake(const uint8_t *msg, size_t len);
    void dispatch(const uint8_t *msg, size_t len);
    void sendScreenInfo();
    void disconnect(const char *why);
    void serviceOnce();                      // one pass: connect, or pump messages
    bool captureServerCert();                // one probe handshake to grab the peer cert
    bool storePending(const unsigned char *der, size_t derLen);  // DER -> pending PEM+fp
    void freePending();
    // Emit the coalesced pointer position. Non-blocking by default: if the HID
    // endpoint is busy the motion stays pending for the next pass. `block`
    // forces it out (before a click, which must land where the pointer is).
    void flushPointer(bool block = false);
    void run();                              // task body: serviceOnce forever
    static void taskEntry(void *self);

    HidDevice &hid_;
    Config    &config_;
    // Deskflow, Barrier and Synergy all enable TLS by default, with a
    // self-signed certificate the user accepts by fingerprint. Both transports
    // are kept because some deployments turn encryption off, and `sock_` points
    // at whichever is in use.
    DeviceIdentity    identity_;
    WiFiClient        plain_;
    WiFiClientSecure  tls_;
    Client           *sock_ = nullptr;

    State    state_ = State::Idle;
    bool     hasFocus_ = false;
    uint32_t lastAttemptMs_ = 0;
    uint32_t backoffMs_ = 2000;
    // Updated on every message from the server, keep-alives included. The
    // held-input watchdog uses it: if input is held and this stops advancing,
    // the server has gone silent and whatever is down must be released.
    uint32_t lastTrafficMs_ = 0;

    // reconnect()/suspend() are called from the network task; acting on them
    // there would tear down the socket and TLS state under this task mid-read.
    // Instead they set a flag that serviceOnce() acts on, on this task.
    volatile bool reconnectReq_ = false;
    volatile bool suspendReq_   = false;
    // Set from another task to confirm the captured server certificate.
    volatile bool trustReq_     = false;

    // Trust-on-first-use state. awaitingTrust_ means a certificate was captured
    // and is waiting for the user to confirm it; pendingPem_/pendingFp_ hold it
    // until then. Never fed to a live session - only pinned once trusted.
    bool  awaitingTrust_ = false;
    char *pendingPem_    = nullptr;
    char  pendingFp_[65] = {};

    // A private copy of the pinned CA PEM, taken under Config's lock at connect
    // time. mbedTLS reads the CA buffer during the handshake; using Config's own
    // buffer let a set_config on the network task free it mid-parse. Alive only
    // for the session, freed on disconnect.
    char *caCopy_ = nullptr;
    char     lastError_[128] = {};
    uint32_t nMove_ = 0, nKey_ = 0, nBtn_ = 0, nOther_ = 0;
    char     lastUnhandled_[8] = {};
    char     lastKeyRaw_[40] = {};
    char     lastKeyDownRaw_[40] = {};
    char     lastOtherRaw_[40] = {};

    // Physical button -> the HID key we pressed for it. On key-up the server
    // sends KeyID 0 and identifies the key only by its button, so without this
    // there is nothing to release and every key sticks down.
    struct HeldKey { uint16_t button; uint8_t hid; };
    static constexpr size_t kMaxHeld = 12;
    HeldKey heldByButton_[kMaxHeld] = {};
    size_t  heldByButtonCount_ = 0;

    void rememberKey(uint16_t button, uint8_t hid);
    uint8_t forgetKey(uint16_t button);

    // Pointer motion is coalesced rather than replayed. Each HID report blocks
    // until the host collects it, roughly a USB frame, while the server streams
    // positions faster than that - so sending every one means delivering a
    // backlog of stale positions, which is what stutter is. Only the newest
    // position matters; relative deltas sum. A position that could not be sent
    // (endpoint busy) stays pending here and is overwritten by newer ones.
    bool    haveAbs_ = false;
    int32_t absX_ = 0, absY_ = 0;
    int32_t relDx_ = 0, relDy_ = 0;

    // The 7-byte protocol name the server greeted us with, echoed back so we
    // work with Synergy, Barrier and Deskflow servers without caring which.
    char serverName_[8] = {};
};

}  // namespace ghosthid
