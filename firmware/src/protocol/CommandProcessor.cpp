#include "CommandProcessor.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <stdarg.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "Keymap.h"
#include "board_config.h"
#include "config/Config.h"
#include "net/DeskflowClient.h"
#include "net/TlsArena.h"

extern uint32_t g_heapAfterBoot, g_heapAfterWifi, g_heapAfterServer;
extern bool g_bootComplete;
#include "hid/HidDevice.h"

namespace ghosthid {
namespace {

bool parseMouseButton(const char *name, MouseButton &out) {
    if (name == nullptr) return false;
    if (strcasecmp(name, "left")   == 0) { out = MouseButton::Left;   return true; }
    if (strcasecmp(name, "right")  == 0) { out = MouseButton::Right;  return true; }
    if (strcasecmp(name, "middle") == 0) { out = MouseButton::Middle; return true; }
    return false;
}

// Constant-time string compare: folds every byte into an accumulator with no
// early return, so a remote timing attack can't learn the token prefix-by-prefix
// (matters most combined with the auth rate limit).
bool ctEquals(const char *a, const char *b) {
    const size_t la = strlen(a), lb = strlen(b);
    unsigned char diff = static_cast<unsigned char>(la ^ lb);
    const size_t n = la > lb ? la : lb;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char ca = i < la ? static_cast<unsigned char>(a[i]) : 0;
        const unsigned char cb = i < lb ? static_cast<unsigned char>(b[i]) : 0;
        diff |= static_cast<unsigned char>(ca ^ cb);
    }
    return diff == 0;
}

// ASCII upper-case copy, for case-insensitive token comparison.
void upperCopy(char *dst, const char *src, size_t cap) {
    size_t i = 0;
    for (; src[i] != '\0' && i + 1 < cap; ++i) {
        char c = src[i];
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        dst[i] = c;
    }
    dst[i] = '\0';
}

void reply(char *out, size_t outSize, const char *fmt, ...) {
    if (out == nullptr || outSize == 0) return;
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(out, outSize, fmt, args);
    va_end(args);
    // M5: on overflow vsnprintf truncates, leaving JSON with no closing brace
    // that the browser fails to parse - worst exactly when there's an error to
    // report. Emit a short well-formed document instead.
    if (n < 0 || static_cast<size_t>(n) >= outSize) {
        snprintf(out, outSize, "{\"type\":\"error\",\"error\":\"response too large\"}");
    }
}

}  // namespace

int CommandProcessor::findSession(uint32_t clientId) const {
    for (size_t i = 0; i < kMaxSessions; ++i) {
        if (sessionCount_ > 0 && sessionId_[i] == clientId && clientId != 0) return (int)i;
    }
    return -1;
}

void CommandProcessor::beginSession(uint32_t clientId) {
    if (findSession(clientId) >= 0) return;             // already registered
    for (size_t i = 0; i < kMaxSessions; ++i) {
        if (sessionId_[i] == 0) {
            sessionId_[i] = clientId;
            // An empty token means auth is disabled; treat the client as already
            // authenticated so the device is usable without a pairing step.
            const char *tok = config_.authToken();
            sessionAuthed_[i] = (tok == nullptr || tok[0] == '\0');
            ++sessionCount_;
            break;
        }
    }
    lastMessageMs_ = millis();
}

void CommandProcessor::endSession(uint32_t clientId) {
    const int i = findSession(clientId);
    if (i < 0) return;                      // not a tracked controller
    sessionId_[i] = 0;
    sessionAuthed_[i] = false;
    if (sessionCount_ > 0) --sessionCount_;
    // A controller vanishing must NEVER leave a key held on the target - the
    // worst failure this device can produce. Held keys can't be attributed to a
    // specific controller (the HID state is shared), so any disconnect releases
    // everything while anything is held: a spurious release for the controllers
    // that remain is far better than a key stuck down (H8).
    if (hid_.anythingHeld()) hid_.releaseAll();
}

bool CommandProcessor::authenticated(uint32_t clientId) const {
    const int i = findSession(clientId);
    return i >= 0 && sessionAuthed_[i];
}

uint32_t CommandProcessor::millisSinceLastMessage() const {
    return millis() - lastMessageMs_;
}

bool CommandProcessor::serviceWatchdog(uint32_t timeoutMs) {
    if (sessionCount_ == 0) return false;
    if (!hid_.anythingHeld()) return false;      // nothing to protect against
    if (millisSinceLastMessage() < timeoutMs) return false;

    hid_.releaseAll();
    return true;
}

void CommandProcessor::setLocked(bool locked, const char *reason) {
    locked_ = locked;
    lockReason_ = (reason != nullptr) ? reason : "";
    if (locked_) { lockedAtMs_ = millis(); hid_.releaseAll(); }
}

bool CommandProcessor::consumeDisconnectRequest() {
    const bool d = disconnectReq_;
    disconnectReq_ = false;
    return d;
}

bool CommandProcessor::lockedTooLong(uint32_t timeoutMs) const {
    return locked_ && (millis() - lockedAtMs_) > timeoutMs;
}

CommandResult CommandProcessor::handleMessage(uint32_t clientId, const char *json, size_t len,
                                              char *outResponse, size_t outSize) {
    if (outResponse != nullptr && outSize > 0) outResponse[0] = '\0';
    lastMessageMs_ = millis();

    JsonDocument doc;
    if (deserializeJson(doc, json, len) != DeserializationError::Ok) {
        reply(outResponse, outSize, "{\"type\":\"error\",\"error\":\"bad json\"}");
        return CommandResult::BadRequest;
    }

    const char *type = doc["type"] | "";

    // --- auth ---------------------------------------------------------------
    if (strcmp(type, "auth") == 0) {
        const char *rawGiven = doc["token"] | "";
        // The token is shown on the LCD in two 4-char blocks; accept it with or
        // without the space, from any client (the web UI already strips it, but
        // a raw WebSocket client should get the same leniency). Real tokens have
        // no whitespace.
        char given[64];
        {
            size_t o = 0;
            for (size_t i = 0; rawGiven[i] != '\0' && o + 1 < sizeof(given); ++i) {
                if (rawGiven[i] == ' ' || rawGiven[i] == '\t') continue;
                given[o++] = rawGiven[i];
            }
            given[o] = '\0';
        }
        const char *tok = config_.authToken();
        const bool needTok = (tok != nullptr && tok[0] != '\0');
        const uint32_t now = millis();

        // Idle decay: a legitimate operator fumbling a hard-to-read token pauses
        // between tries; a brute-force script does not. If it has been quiet for
        // a while, forgive the accumulated strikes and any cooldown so an honest
        // user is never stuck behind a 15-minute wall they earned by mistyping.
        if (lastAuthMs_ != 0 && now - lastAuthMs_ > 120000) {
            authFails_ = 0;
            authLockouts_ = 0;
            authCooldownUntil_ = 0;
        }
        lastAuthMs_ = now;

        // Lockout window after repeated failures: refuse without even checking,
        // and tell the client how long to wait (retry_ms) so it can say "locked,
        // wait Ns" rather than mislabelling it as a bad token. Also drop the
        // socket so a script must reconnect (and wait) between guesses.
        if (needTok && authCooldownUntil_ != 0 &&
            static_cast<int32_t>(authCooldownUntil_ - now) > 0) {
            reply(outResponse, outSize,
                  "{\"type\":\"auth\",\"ok\":false,\"locked\":true,\"retry_ms\":%u}",
                  (unsigned)(authCooldownUntil_ - now));
            disconnectReq_ = true;
            return CommandResult::Unauthenticated;
        }
        // Case-insensitive: the generated token alphabet is uppercase + digits
        // with no case collisions, so folding case removes a whole class of
        // mobile-keyboard mistype at no cost for generated tokens (a small,
        // deliberate keyspace trade for user-set mixed-case tokens).
        char givU[64], tokU[64];
        upperCopy(givU, given, sizeof(givU));
        upperCopy(tokU, tok, sizeof(tokU));
        if (needTok && !ctEquals(givU, tokU)) {
            // Three strikes -> a cooldown and a forced disconnect. The counters
            // survive reconnects (see the header note), so this actually bounds
            // the guess rate instead of resetting on every new socket.
            bool justLocked = false;
            if (++authFails_ >= 3) {
                // Escalating backoff, gentler at the start (15s, 30s, 60s, ...
                // capped at 15 min) so a first fumble is a short wait, not a wall.
                const unsigned shift = authLockouts_ < 6 ? authLockouts_ : 6;
                uint32_t cd = 15000u << shift;
                if (cd > 900000u) cd = 900000u;
                authCooldownUntil_ = now + cd;
                authFails_ = 0;
                if (authLockouts_ < 250) ++authLockouts_;
                disconnectReq_ = true;
                justLocked = true;
            }
            if (justLocked) {
                reply(outResponse, outSize,
                      "{\"type\":\"auth\",\"ok\":false,\"locked\":true,\"retry_ms\":%u}",
                      (unsigned)(authCooldownUntil_ - now));
            } else {
                reply(outResponse, outSize, "{\"type\":\"auth\",\"ok\":false}");
            }
            return CommandResult::Unauthenticated;
        }
        authFails_ = 0;
        authLockouts_ = 0;
        authCooldownUntil_ = 0;
        {
            const int i = findSession(clientId);
            if (i >= 0) sessionAuthed_[i] = true;
        }
        reply(outResponse, outSize,
              "{\"type\":\"auth\",\"ok\":true,\"version\":\"%s\",\"usb\":%s}",
              GHOSTHID_VERSION, hid_.ready() ? "true" : "false");
        return CommandResult::Ok;
    }

    // Everything past this point requires a valid session. Refusing HID
    // actions - not just rejecting the connection - is what stops an
    // unauthenticated peer on the network from typing on the target.
    if (!authenticated(clientId)) {
        reply(outResponse, outSize, "{\"type\":\"error\",\"error\":\"unauthenticated\"}");
        return CommandResult::Unauthenticated;
    }

    // --- heartbeat ----------------------------------------------------------
    // The pong carries USB state so the controller learns about a target that
    // slept or was unplugged without having to poll separately.
    if (strcmp(type, "ping") == 0) {
        // The pong carries the host lock-LED state (locks) and a count of the
        // output reports the host has sent (hled). A climbing hled is proof the
        // target is actually driving our keyboard, not merely powering it - the
        // one thing nothing else on the wire can tell the controller.
        reply(outResponse, outSize,
              "{\"type\":\"pong\",\"usb\":%s,\"kvm\":%u,\"locks\":%u,\"hled\":%u,\"ctrls\":%u}",
              hid_.ready() ? "true" : "false",
              (unsigned)(deskflow_ ? deskflow_->stateCode() : 0),
              (unsigned)hid_.hostLeds(),
              (unsigned)hid_.hostLedReports(),
              (unsigned)sessionCount_);
        return CommandResult::Ok;
    }

    if (strcmp(type, "status") == 0) {
        // ArduinoJson so the device-derived last_* fields are escaped (they are
        // hex today, but this stops a future field regressing into a raw %s -
        // the last of M4). Field names/types are unchanged for API clients.
        JsonDocument out;
        out["type"]         = "status";
        out["version"]      = GHOSTHID_VERSION;
        out["usb"]          = hid_.ready();
        out["held"]         = static_cast<unsigned>(hid_.heldKeyCount());
        out["heap_free"]    = (unsigned)ESP.getFreeHeap();
        out["heap_largest"] = (unsigned)ESP.getMaxAllocHeap();
        out["heap_boot"]    = (unsigned)g_heapAfterBoot;
        out["heap_wifi"]    = (unsigned)g_heapAfterWifi;
        out["heap_server"]  = (unsigned)g_heapAfterServer;
        out["stack_main"]   = (unsigned)uxTaskGetStackHighWaterMark(nullptr);
        out["stack_kvm"]    = (unsigned)(xTaskGetHandle("deskflow")
                              ? uxTaskGetStackHighWaterMark(xTaskGetHandle("deskflow")) : 0);
        out["stack_async"]  = (unsigned)(xTaskGetHandle("async_tcp")
                              ? uxTaskGetStackHighWaterMark(xTaskGetHandle("async_tcp")) : 0);
        out["n_move"]       = (unsigned)(deskflow_ ? deskflow_->countMove()  : 0);
        out["n_key"]        = (unsigned)(deskflow_ ? deskflow_->countKey()   : 0);
        out["n_btn"]        = (unsigned)(deskflow_ ? deskflow_->countBtn()   : 0);
        out["n_other"]      = (unsigned)(deskflow_ ? deskflow_->countOther() : 0);
        out["last_unhandled"]   = deskflow_ ? deskflow_->lastUnhandled()   : "";
        out["last_key_raw"]     = deskflow_ ? deskflow_->lastKeyRaw()      : "";
        out["last_keydown_raw"] = deskflow_ ? deskflow_->lastKeyDownRaw()  : "";
        out["last_other_raw"]   = deskflow_ ? deskflow_->lastOtherRaw()    : "";
        out["hid_dropped"]     = (unsigned)hid_.droppedReports();
        out["tls_reserved"]    = (unsigned)TlsArena::reservedBytes();
        out["tls_blocks_lent"] = (unsigned)TlsArena::inUse();
        if (measureJson(out) + 1 > outSize) {
            reply(outResponse, outSize,
                  "{\"type\":\"error\",\"error\":\"response too large\"}");
        } else {
            serializeJson(out, outResponse, outSize);
        }
        return CommandResult::Ok;
    }

    // Refuse input commands when the target has not enumerated us. Silently
    // dropping them - which is what the HID layer does on its own - looks
    // identical to success from the controller's side, and that is the single
    // most confusing failure this device can present.
    const bool isInput =
        strcmp(type, "key") == 0 || strcmp(type, "text") == 0 ||
        strcmp(type, "mouse_move") == 0 || strcmp(type, "mouse_abs") == 0 ||
        strcmp(type, "mouse_button") == 0 ||
        strcmp(type, "mouse_wheel") == 0 ||
        strcmp(type, "media") == 0 || strcmp(type, "system") == 0;
    if (isInput && locked_) {
        reply(outResponse, outSize,
              "{\"type\":\"error\",\"error\":\"%s\"}", lockReason_);
        return CommandResult::BadRequest;
    }
    if (isInput && !hid_.ready()) {
        reply(outResponse, outSize,
              "{\"type\":\"error\",\"error\":\"usb not ready - target has not "
              "enumerated GhostHID (unplugged, asleep, or a charge-only port)\"}");
        return CommandResult::BadRequest;
    }

    // --- keyboard -----------------------------------------------------------
    if (strcmp(type, "key") == 0) {
        const char *name = doc["key"] | "";
        const uint8_t code = lookupKey(name);
        if (code == 0) {
            reply(outResponse, outSize,
                  "{\"type\":\"error\",\"error\":\"unknown key\"}");
            return CommandResult::BadRequest;
        }
        const bool pressed = doc["pressed"] | true;
        if (pressed) hid_.keyDown(code); else hid_.keyUp(code);
        return CommandResult::Ok;
    }

    if (strcmp(type, "text") == 0) {
        const char *text = doc["text"] | "";
        // Bound the length: each character is two blocking USB reports, and an
        // unbounded string in one frame would hold this network callback long
        // enough to trip the task watchdog. Copy the capped prefix so we never
        // walk past the limit.
        char buf[GHOSTHID_TEXT_MAX + 1];
        size_t n = 0;
        for (const char *p = text; *p && n < GHOSTHID_TEXT_MAX; ++p) buf[n++] = *p;
        buf[n] = '\0';
        hid_.typeText(buf);
        return CommandResult::Ok;
    }

    // --- mouse --------------------------------------------------------------
    if (strcmp(type, "mouse_move") == 0) {
        // Clamp at the boundary. HidDevice splits a delta across 127-px reports,
        // each blocking on USB; an unclamped int32 is a watchdog-tripping flood
        // of reports inside this callback. A real move is never this large.
        int32_t dx = doc["dx"] | 0, dy = doc["dy"] | 0;
        if (dx >  GHOSTHID_MOUSE_MAX_MOVE) dx =  GHOSTHID_MOUSE_MAX_MOVE;
        if (dx < -GHOSTHID_MOUSE_MAX_MOVE) dx = -GHOSTHID_MOUSE_MAX_MOVE;
        if (dy >  GHOSTHID_MOUSE_MAX_MOVE) dy =  GHOSTHID_MOUSE_MAX_MOVE;
        if (dy < -GHOSTHID_MOUSE_MAX_MOVE) dy = -GHOSTHID_MOUSE_MAX_MOVE;
        hid_.mouseMove(dx, dy);
        return CommandResult::Ok;
    }

    // Absolute positioning. x and y are fractions of the target's desktop
    // (0..1), not pixels: the firmware cannot learn the target's resolution,
    // and a fraction survives a resolution change.
    if (strcmp(type, "mouse_abs") == 0) {
        if (!doc["x"].is<float>() || !doc["y"].is<float>()) {
            reply(outResponse, outSize,
                  "{\"type\":\"error\",\"error\":\"mouse_abs needs numeric x and y\"}");
            return CommandResult::BadRequest;
        }
        const float x = doc["x"].as<float>(), y = doc["y"].as<float>();
        // L2: NaN slips past the 0..1 clamp (all comparisons are false) and
        // reaches a uint16_t cast (UB). Reject non-finite values here.
        if (!isfinite(x) || !isfinite(y)) {
            reply(outResponse, outSize,
                  "{\"type\":\"error\",\"error\":\"mouse_abs needs finite x and y\"}");
            return CommandResult::BadRequest;
        }
        hid_.mouseMoveAbsolute(x, y);
        return CommandResult::Ok;
    }

    if (strcmp(type, "mouse_button") == 0) {
        MouseButton button;
        if (!parseMouseButton(doc["button"] | (const char *)nullptr, button)) {
            reply(outResponse, outSize,
                  "{\"type\":\"error\",\"error\":\"unknown button\"}");
            return CommandResult::BadRequest;
        }
        const bool pressed = doc["pressed"] | true;
        if (pressed) hid_.mouseButtonDown(button); else hid_.mouseButtonUp(button);
        return CommandResult::Ok;
    }

    if (strcmp(type, "mouse_wheel") == 0) {
        int32_t d = doc["delta"] | 0;
        if (d >  GHOSTHID_WHEEL_MAX) d =  GHOSTHID_WHEEL_MAX;
        if (d < -GHOSTHID_WHEEL_MAX) d = -GHOSTHID_WHEEL_MAX;
        hid_.mouseWheel(d);
        if (doc["pan"].is<int>()) {
            int32_t p = doc["pan"].as<int>();
            if (p >  GHOSTHID_WHEEL_MAX) p =  GHOSTHID_WHEEL_MAX;
            if (p < -GHOSTHID_WHEEL_MAX) p = -GHOSTHID_WHEEL_MAX;
            hid_.mousePan(p);
        }
        return CommandResult::Ok;
    }

    // --- media / system -----------------------------------------------------
    // Consumer-control (media) keys. Sent as a tap by the HID layer.
    if (strcmp(type, "media") == 0) {
        const char *k = doc["key"] | "";
        MediaKey mk;
        bool ok = true;
        if      (strcmp(k, "vol_up")      == 0) mk = MediaKey::VolumeUp;
        else if (strcmp(k, "vol_down")    == 0) mk = MediaKey::VolumeDown;
        else if (strcmp(k, "mute")        == 0) mk = MediaKey::Mute;
        else if (strcmp(k, "play_pause")  == 0) mk = MediaKey::PlayPause;
        else if (strcmp(k, "next")        == 0) mk = MediaKey::Next;
        else if (strcmp(k, "prev")        == 0) mk = MediaKey::Previous;
        else if (strcmp(k, "stop")        == 0) mk = MediaKey::Stop;
        else if (strcmp(k, "bright_up")   == 0) mk = MediaKey::BrightnessUp;
        else if (strcmp(k, "bright_down") == 0) mk = MediaKey::BrightnessDown;
        else ok = false;
        if (!ok) {
            reply(outResponse, outSize,
                  "{\"type\":\"error\",\"error\":\"unknown media key\"}");
            return CommandResult::BadRequest;
        }
        hid_.mediaKey(mk);
        return CommandResult::Ok;
    }

    // System-control keys - these act on the host's power state.
    if (strcmp(type, "system") == 0) {
        const char *k = doc["key"] | "";
        SystemKey sk;
        bool ok = true;
        if      (strcmp(k, "sleep") == 0) sk = SystemKey::Sleep;
        else if (strcmp(k, "power") == 0) sk = SystemKey::PowerOff;
        else if (strcmp(k, "wake")  == 0) sk = SystemKey::Wake;
        else ok = false;
        if (!ok) {
            reply(outResponse, outSize,
                  "{\"type\":\"error\",\"error\":\"unknown system key\"}");
            return CommandResult::BadRequest;
        }
        hid_.systemKey(sk);
        return CommandResult::Ok;
    }

    // --- safety -------------------------------------------------------------
    if (strcmp(type, "release_all") == 0) {
        hid_.releaseAll();
        return CommandResult::Ok;
    }

    // --- configuration ------------------------------------------------------
    // Secrets are deliberately write-only: we report whether each is set, never
    // its value. Otherwise anyone holding the token could read the Wi-Fi
    // password straight out of the device.
    if (strcmp(type, "get_config") == 0) {
        // Built with ArduinoJson, not printf: several fields are device-supplied
        // strings (SSID, screen-server host, the TLS error text with the host
        // embedded, the wire-supplied server name) and a raw %s would let a
        // quote or control byte break or reshape the JSON the UI trusts (M4).
        // ArduinoJson escapes every string; the size guard replaces M5's manual
        // truncation check for this response. Pointers stay valid through the
        // serialize call below, so no copies are needed.
        JsonDocument out;
        out["type"]           = "config";
        out["sta_ssid"]       = config_.staSsid();
        out["sta_pass_set"]   = config_.staPassword()[0] != '\0';
        out["ap_pass_set"]    = true;
        out["token_set"]      = config_.authToken()[0] != '\0';
        out["name"]           = config_.deviceName();
        out["ap_always"]      = config_.apAlways();
        out["reboot_pending"] = config_.rebootPending();
        out["kvm_on"]         = config_.deskflowEnabled();
        out["kvm_host"]       = config_.deskflowHost();
        out["kvm_port"]       = config_.deskflowPort();
        out["kvm_screen"]     = config_.deskflowScreen();
        out["kvm_w"]          = config_.deskflowWidth();
        out["kvm_h"]          = config_.deskflowHeight();
        out["kvm_state"]      = deskflow_ ? deskflow_->statusText() : "unknown";
        out["kvm_server"]     = deskflow_ ? deskflow_->serverName() : "";
        out["kvm_tls"]        = config_.deskflowTls();
        out["kvm_fp"]         = deskflow_ ? deskflow_->fingerprint() : "";
        out["kvm_ca_set"]     = config_.deskflowServerCert()[0] != '\0';
        // Trust on first use: a captured, not-yet-confirmed server certificate.
        out["kvm_cert_pending"] = deskflow_ ? deskflow_->certTrustPending() : false;
        out["kvm_pending_fp"]   = deskflow_ ? deskflow_->pendingFingerprint() : "";
        out["scroll_invert"]  = config_.scrollInvert();
        if (measureJson(out) + 1 > outSize) {
            reply(outResponse, outSize,
                  "{\"type\":\"error\",\"error\":\"response too large\"}");
        } else {
            serializeJson(out, outResponse, outSize);
        }
        return CommandResult::Ok;
    }

    // Confirm a captured (trust-on-first-use) server certificate. Pinning is a
    // security-sensitive change, so it rides the same authenticated session as
    // set_config; the fingerprint the user is confirming came from get_config.
    if (strcmp(type, "kvm_trust_cert") == 0) {
        if (deskflow_ == nullptr || !deskflow_->certTrustPending()) {
            reply(outResponse, outSize,
                  "{\"type\":\"error\",\"error\":\"no certificate is awaiting confirmation\"}");
            return CommandResult::BadRequest;
        }
        deskflow_->trustPendingCert();
        reply(outResponse, outSize, "{\"type\":\"kvm_trust_cert\",\"ok\":true}");
        return CommandResult::Ok;
    }

    if (strcmp(type, "set_config") == 0) {
        const char *err = nullptr;

        if (doc["sta_ssid"].is<const char *>()) {
            const char *ssid = doc["sta_ssid"] | "";
            // Omitting sta_pass keeps the stored one, so you can correct a
            // typo'd SSID without re-entering the password.
            const char *pass = doc["sta_pass"].is<const char *>()
                                   ? doc["sta_pass"].as<const char *>()
                                   : config_.staPassword();
            if (!config_.setStation(ssid, pass)) err = "invalid station credentials";
        }
        if (!err && doc["ap_pass"].is<const char *>()) {
            if (!config_.setApPassword(doc["ap_pass"].as<const char *>()))
                err = "ap password must be 8-63 characters";
        }
        if (!err && doc["token"].is<const char *>()) {
            if (!config_.setAuthToken(doc["token"].as<const char *>()))
                err = "token must be empty (disables auth) or 6-48 characters";
        }
        if (!err && doc["ap_always"].is<bool>()) {
            config_.setApAlways(doc["ap_always"].as<bool>());
        }
        if (!err && doc["scroll_invert"].is<bool>()) {
            const bool inv = doc["scroll_invert"].as<bool>();
            config_.setScrollInvert(inv);
            hid_.setInvertScroll(inv);          // live, no reboot needed
        }
        // Screen-client settings apply immediately - the client is told to
        // reconnect - so they are deliberately not part of reboot_pending.
        bool kvmChanged = false;
        if (!err && (doc["kvm_host"].is<const char *>() || doc["kvm_port"].is<int>())) {
            const char *host = doc["kvm_host"] | config_.deskflowHost();
            const uint16_t port = (uint16_t)(doc["kvm_port"] | (int)config_.deskflowPort());
            if (!config_.setDeskflowServer(host, port)) err = "bad screen server host or port";
            else kvmChanged = true;
        }
        if (!err && doc["kvm_screen"].is<const char *>()) {
            if (!config_.setDeskflowScreen(doc["kvm_screen"].as<const char *>()))
                err = "screen name must be 1-31 characters";
            else kvmChanged = true;
        }
        if (!err && (doc["kvm_w"].is<int>() || doc["kvm_h"].is<int>())) {
            const uint16_t w = (uint16_t)(doc["kvm_w"] | (int)config_.deskflowWidth());
            const uint16_t h = (uint16_t)(doc["kvm_h"] | (int)config_.deskflowHeight());
            if (!config_.setDeskflowScreenSize(w, h)) err = "screen size out of range";
            else kvmChanged = true;
        }
        if (!err && doc["kvm_ca"].is<const char *>()) {
            if (!config_.setDeskflowServerCert(doc["kvm_ca"].as<const char *>()))
                err = "server certificate must be PEM and under 4000 bytes";
            else kvmChanged = true;
        }
        if (!err && doc["kvm_tls"].is<bool>()) {
            config_.setDeskflowTls(doc["kvm_tls"].as<bool>());
            kvmChanged = true;
        }
        bool kvmNeedsReboot = false;
        if (!err && doc["kvm_on"].is<bool>()) {
            const bool on = doc["kvm_on"].as<bool>();
            if (on && config_.deskflowHost()[0] == '\0') err = "set a server address first";
            else {
                const bool wasOff = !config_.deskflowEnabled();
                config_.setDeskflowEnabled(on);
                kvmChanged = true;
                // A TLS handshake needs 16KB contiguous. That exists at boot,
                // before the web server starts, and often does not afterwards -
                // so enabling it now may connect or may fail on memory, and the
                // user should not have to guess which.
                if (on && wasOff && config_.deskflowTls() && g_bootComplete) {
                    kvmNeedsReboot = ESP.getMaxAllocHeap() < 48 * 1024;
                }
            }
        }
        if (!err && kvmChanged && deskflow_ != nullptr) deskflow_->reconnect();

        if (!err && doc["name"].is<const char *>()) {
            if (!config_.setDeviceName(doc["name"].as<const char *>()))
                err = "name must be letters, digits or hyphens";
        }

        if (err) {
            reply(outResponse, outSize,
                  "{\"type\":\"error\",\"error\":\"%s\"}", err);
            return CommandResult::BadRequest;
        }
        // Report what actually needs a restart rather than always claiming
        // one: a token change is live on the next connection.
        reply(outResponse, outSize,
              "{\"type\":\"config_saved\",\"reboot_required\":%s,\"kvm_reboot\":%s}",
              (config_.rebootPending() || kvmNeedsReboot) ? "true" : "false",
              kvmNeedsReboot ? "true" : "false");
        return CommandResult::Ok;
    }

    if (strcmp(type, "reboot") == 0) {
        // Flag it; main() reboots from loop(). Restarting inside a network
        // callback would tear down the stack from under itself.
        rebootRequested_ = true;
        hid_.releaseAll();
        reply(outResponse, outSize, "{\"type\":\"rebooting\"}");
        return CommandResult::Ok;
    }

    reply(outResponse, outSize, "{\"type\":\"error\",\"error\":\"unknown type\"}");
    return CommandResult::BadRequest;
}

}  // namespace ghosthid
