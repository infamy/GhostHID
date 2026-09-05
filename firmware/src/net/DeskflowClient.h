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
    void flushPointer();                     // emit the coalesced pointer position
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
    uint32_t lastTrafficMs_ = 0;
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
    // position matters; relative deltas sum.
    bool    haveAbs_ = false;
    int32_t absX_ = 0, absY_ = 0;
    int32_t relDx_ = 0, relDy_ = 0;

    // The 7-byte protocol name the server greeted us with, echoed back so we
    // work with Synergy, Barrier and Deskflow servers without caring which.
    char serverName_[8] = {};
};

}  // namespace ghosthid
