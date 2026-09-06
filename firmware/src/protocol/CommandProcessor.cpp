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

void CommandProcessor::beginSession() {
    sessionActive_  = true;
    // An empty token means auth is disabled; treat the session as already
    // authenticated so the device is usable without a pairing step.
    const char *tok = config_.authToken();
    authenticated_  = (tok == nullptr || tok[0] == '\0');
    lastMessageMs_  = millis();
}

void CommandProcessor::endSession() {
    sessionActive_ = false;
    authenticated_ = false;
    // The whole point of PLAN.md section 7: whatever the controller was
    // holding when it vanished must not stay held on the target.
    hid_.releaseAll();
}

uint32_t CommandProcessor::millisSinceLastMessage() const {
    return millis() - lastMessageMs_;
}

bool CommandProcessor::serviceWatchdog(uint32_t timeoutMs) {
    if (!sessionActive_) return false;
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

CommandResult CommandProcessor::handleMessage(const char *json, size_t len,
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
        const char *given = doc["token"] | "";
        const char *tok = config_.authToken();
        const bool needTok = (tok != nullptr && tok[0] != '\0');
        const uint32_t now = millis();

        // Lockout window after repeated failures: refuse without even checking,
        // and ask the transport to drop the socket so a script must reconnect
        // (and wait) between guesses.
        if (needTok && authCooldownUntil_ != 0 &&
            static_cast<int32_t>(authCooldownUntil_ - now) > 0) {
            reply(outResponse, outSize,
                  "{\"type\":\"auth\",\"ok\":false,\"error\":\"too many attempts\"}");
            disconnectReq_ = true;
            return CommandResult::Unauthenticated;
        }
        if (needTok && !ctEquals(given, tok)) {
            // Three strikes -> a cooldown and a forced disconnect. The counter
            // survives reconnects (see the header note), so this actually bounds
            // the guess rate instead of resetting on every new socket.
            if (++authFails_ >= 3) {
                authCooldownUntil_ = now + 30000;
                authFails_ = 0;
                disconnectReq_ = true;
            }
            reply(outResponse, outSize, "{\"type\":\"auth\",\"ok\":false}");
            return CommandResult::Unauthenticated;
        }
        authFails_ = 0;
        authenticated_ = true;
        reply(outResponse, outSize,
              "{\"type\":\"auth\",\"ok\":true,\"version\":\"%s\",\"usb\":%s}",
              GHOSTHID_VERSION, hid_.ready() ? "true" : "false");
        return CommandResult::Ok;
    }

    // Everything past this point requires a valid session. Refusing HID
    // actions - not just rejecting the connection - is what stops an
    // unauthenticated peer on the network from typing on the target.
    if (!authenticated_) {
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
              "{\"type\":\"pong\",\"usb\":%s,\"kvm\":%u,\"locks\":%u,\"hled\":%u}",
              hid_.ready() ? "true" : "false",
              (unsigned)(deskflow_ ? deskflow_->stateCode() : 0),
              (unsigned)hid_.hostLeds(),
              (unsigned)hid_.hostLedReports());
        return CommandResult::Ok;
    }

    if (strcmp(type, "status") == 0) {
        reply(outResponse, outSize,
              "{\"type\":\"status\",\"version\":\"%s\",\"usb\":%s,\"held\":%u,"
              "\"heap_free\":%u,\"heap_largest\":%u,"
              "\"heap_boot\":%u,\"heap_wifi\":%u,\"heap_server\":%u,"
              "\"stack_main\":%u,\"stack_kvm\":%u,\"stack_async\":%u,"
              "\"n_move\":%u,\"n_key\":%u,\"n_btn\":%u,\"n_other\":%u,"
              "\"last_unhandled\":\"%s\",\"last_key_raw\":\"%s\",\"last_keydown_raw\":\"%s\",\"last_other_raw\":\"%s\",\"hid_dropped\":%u,\"tls_reserved\":%u,\"tls_blocks_lent\":%u}",
              GHOSTHID_VERSION, hid_.ready() ? "true" : "false",
              static_cast<unsigned>(hid_.heldKeyCount()),
              (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
              (unsigned)g_heapAfterBoot, (unsigned)g_heapAfterWifi,
              (unsigned)g_heapAfterServer,
              // Unused stack, in bytes. Anything with a large margin is memory
              // sitting idle that could be given back.
              (unsigned)uxTaskGetStackHighWaterMark(nullptr),
              (unsigned)(xTaskGetHandle("deskflow")
                         ? uxTaskGetStackHighWaterMark(xTaskGetHandle("deskflow")) : 0),
              (unsigned)(xTaskGetHandle("async_tcp")
                         ? uxTaskGetStackHighWaterMark(xTaskGetHandle("async_tcp")) : 0),
              (unsigned)(deskflow_ ? deskflow_->countMove()  : 0),
              (unsigned)(deskflow_ ? deskflow_->countKey()   : 0),
              (unsigned)(deskflow_ ? deskflow_->countBtn()   : 0),
              (unsigned)(deskflow_ ? deskflow_->countOther() : 0),
              deskflow_ ? deskflow_->lastUnhandled() : "",
              deskflow_ ? deskflow_->lastKeyRaw() : "",
              deskflow_ ? deskflow_->lastKeyDownRaw() : "",
              deskflow_ ? deskflow_->lastOtherRaw() : "",
              (unsigned)hid_.droppedReports(),
              (unsigned)TlsArena::reservedBytes(),
              (unsigned)TlsArena::inUse());
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
        out["scroll_invert"]  = config_.scrollInvert();
        if (measureJson(out) + 1 > outSize) {
            reply(outResponse, outSize,
                  "{\"type\":\"error\",\"error\":\"response too large\"}");
        } else {
            serializeJson(out, outResponse, outSize);
        }
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
                err = "token too long";
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
