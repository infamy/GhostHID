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
class DeskflowClient;

enum class CommandResult : uint8_t {
    Ok,
    Unauthenticated,   // caller must send a valid auth message first
    BadRequest,        // malformed JSON, unknown type, unknown key name
};

class CommandProcessor {
public:
    CommandProcessor(HidDevice &hid, Config &config)
        : hid_(hid), config_(config) {}

    // Called when a controller connects. Registers a per-client session.
    void beginSession(uint32_t clientId);

    // Called when a controller disconnects for ANY reason. Drops that client's
    // session and, once the last controller is gone, releases all held input -
    // the primary stuck-key defence.
    void endSession(uint32_t clientId);

    // Feeds one protocol message from a specific client. Auth is tracked per
    // client, so one controller authenticating never authorises another.
    // `outResponse` receives a JSON reply if the message warrants one.
    CommandResult handleMessage(uint32_t clientId, const char *json, size_t len,
                                char *outResponse, size_t outSize);

    // Whether a specific client has authenticated this session.
    bool authenticated(uint32_t clientId) const;
    // How many controllers are currently connected (for the multi-controller
    // warning surfaced in the heartbeat).
    size_t sessionCount() const { return sessionCount_; }

    // Set when a config change needs a restart to take effect. main() acts on
    // it from loop(), never from inside a network callback.
    bool rebootRequested() const { return rebootRequested_; }
    void requestReboot() { rebootRequested_ = true; }

    // Locks out HID input for the duration of a firmware update. Injecting
    // keystrokes into the target while we are rewriting our own flash is not
    // something we want to find out the consequences of.
    void setLocked(bool locked, const char *reason);

    // Optional: lets get_config/status report the screen client's real state
    // rather than only what is configured.
    void attachDeskflow(DeskflowClient *client) { deskflow_ = client; }

    // Milliseconds since the last message from the controller.
    uint32_t millisSinceLastMessage() const;

    // Releases everything if the controller has gone quiet while holding
    // input. Call from loop(). Returns true if it actually fired.
    bool serviceWatchdog(uint32_t timeoutMs);

    // True (once) when the transport should drop the current socket: set after
    // repeated auth failures so a brute-force attempt is thrown off rather than
    // left to retry on the same connection. Reading it clears it.
    bool consumeDisconnectRequest();

    // True while the HID lock (set for an OTA) has been held longer than
    // `timeoutMs`. A backstop: if an OTA client vanishes mid-upload the
    // completion handler may never run, and without this the device would
    // refuse all input until a manual reboot. Call from loop().
    bool lockedTooLong(uint32_t timeoutMs) const;

private:
    HidDevice &hid_;
    Config    &config_;
    DeskflowClient *deskflow_ = nullptr;

    // Multiple controllers may connect at once; each authenticates on its own.
    // A shared auth flag would let one client's login authorise another, so
    // auth is tracked per client id here.
    static constexpr size_t kMaxSessions = 4;
    uint32_t sessionId_[kMaxSessions]   = {};   // connected client ids (0 = free)
    bool     sessionAuthed_[kMaxSessions] = {}; // parallel: has that client authed
    size_t   sessionCount_ = 0;
    int  findSession(uint32_t clientId) const;  // index, or -1 if not connected

    uint32_t lastMessageMs_ = 0;
    bool     rebootRequested_ = false;
    bool     locked_ = false;
    uint32_t lockedAtMs_ = 0;
    const char *lockReason_ = "";

    // Auth brute-force defence. Deliberately NOT reset by beginSession(): a
    // reconnect must not clear the failure count, or the limit is free to
    // bypass. Only a successful auth (or the cooldown elapsing) clears it.
    uint8_t  authFails_ = 0;
    uint8_t  authLockouts_ = 0;         // how many cooldowns so far (escalating backoff)
    uint32_t authCooldownUntil_ = 0;   // 0 = never tripped
    uint32_t lastAuthMs_ = 0;          // last auth attempt, for idle decay of the counters
    bool     disconnectReq_ = false;
};

}  // namespace ghosthid
