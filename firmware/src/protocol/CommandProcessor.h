// Translates protocol messages into HID actions.
//
// Deliberately transport-agnostic: it takes a JSON string and calls into
// HidDevice. It knows nothing about WebSockets, Wi-Fi or TCP, so swapping the
// transport (BLE, ESP-NOW, a binary protocol) touches nothing in this file.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ghosthid {

class HidDevice;
class Config;

enum class CommandResult : uint8_t {
    Ok,
    Unauthenticated,   // caller must send a valid auth message first
    BadRequest,        // malformed JSON, unknown type, unknown key name
};

class CommandProcessor {
public:
    CommandProcessor(HidDevice &hid, Config &config)
        : hid_(hid), config_(config) {}

    // Called when a controller connects. Resets per-session state.
    void beginSession();

    // Called when a controller disconnects for ANY reason. Releases all held
    // input - this is the primary stuck-key defence.
    void endSession();

    // Feeds one protocol message. `outResponse` receives a JSON reply if the
    // message warrants one (it may be left empty).
    CommandResult handleMessage(const char *json, size_t len,
                                char *outResponse, size_t outSize);

    bool authenticated() const { return authenticated_; }

    // Set when a config change needs a restart to take effect. main() acts on
    // it from loop(), never from inside a network callback.
    bool rebootRequested() const { return rebootRequested_; }

    // Milliseconds since the last message from the controller.
    uint32_t millisSinceLastMessage() const;

    // Releases everything if the controller has gone quiet while holding
    // input. Call from loop(). Returns true if it actually fired.
    bool serviceWatchdog(uint32_t timeoutMs);

private:
    HidDevice &hid_;
    Config    &config_;
    bool     authenticated_ = false;
    bool     sessionActive_ = false;
    uint32_t lastMessageMs_ = 0;
    bool     rebootRequested_ = false;
};

}  // namespace ghosthid
