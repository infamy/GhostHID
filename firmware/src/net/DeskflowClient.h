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
#include <stdint.h>

namespace ghosthid {

class Config;
class HidDevice;

class DeskflowClient {
public:
    DeskflowClient(HidDevice &hid, Config &config) : hid_(hid), config_(config) {}

    // Call from loop(). Connects, handshakes and pumps messages; returns
    // promptly whether or not anything happened.
    void loop();

    bool connected() const { return state_ == State::Active; }
    // True while the server has handed this screen the pointer.
    bool hasFocus() const { return hasFocus_; }
    const char *statusText() const;

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

    HidDevice &hid_;
    Config    &config_;
    WiFiClient sock_;

    State    state_ = State::Idle;
    bool     hasFocus_ = false;
    uint32_t lastAttemptMs_ = 0;
    uint32_t backoffMs_ = 2000;
    uint32_t lastTrafficMs_ = 0;
    char     lastError_[64] = {};

    // The 7-byte protocol name the server greeted us with, echoed back so we
    // work with Synergy, Barrier and Deskflow servers without caring which.
    char serverName_[8] = {};
};

}  // namespace ghosthid
