#include "CommandProcessor.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "Keymap.h"
#include "board_config.h"
#include "config/Config.h"
#include "net/DeskflowClient.h"

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

void reply(char *out, size_t outSize, const char *fmt, ...) {
    if (out == nullptr || outSize == 0) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(out, outSize, fmt, args);
    va_end(args);
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
    if (locked_) hid_.releaseAll();
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
        if (tok != nullptr && tok[0] != '\0' && strcmp(given, tok) != 0) {
            reply(outResponse, outSize, "{\"type\":\"auth\",\"ok\":false}");
            return CommandResult::Unauthenticated;
        }
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
        reply(outResponse, outSize, "{\"type\":\"pong\",\"usb\":%s,\"kvm\":%u}",
              hid_.ready() ? "true" : "false",
              (unsigned)(deskflow_ ? deskflow_->stateCode() : 0));
        return CommandResult::Ok;
    }

    if (strcmp(type, "status") == 0) {
        reply(outResponse, outSize,
              "{\"type\":\"status\",\"version\":\"%s\",\"usb\":%s,\"held\":%u,"
              "\"heap_free\":%u,\"heap_largest\":%u,"
              "\"heap_boot\":%u,\"heap_wifi\":%u,\"heap_server\":%u,"
              "\"stack_main\":%u,\"stack_kvm\":%u,\"stack_async\":%u}",
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
                         ? uxTaskGetStackHighWaterMark(xTaskGetHandle("async_tcp")) : 0));
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
        strcmp(type, "mouse_wheel") == 0;
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
        hid_.typeText(text);
        return CommandResult::Ok;
    }

    // --- mouse --------------------------------------------------------------
    if (strcmp(type, "mouse_move") == 0) {
        // Accept the full int32 range; HidDevice splits it across reports.
        hid_.mouseMove(doc["dx"] | 0, doc["dy"] | 0);
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
        hid_.mouseMoveAbsolute(doc["x"].as<float>(), doc["y"].as<float>());
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
        hid_.mouseWheel(doc["delta"] | 0);
        if (doc["pan"].is<int>()) hid_.mousePan(doc["pan"].as<int>());
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
        reply(outResponse, outSize,
              "{\"type\":\"config\",\"sta_ssid\":\"%s\",\"sta_pass_set\":%s,"
              "\"ap_pass_set\":true,\"token_set\":%s,\"name\":\"%s\","
              "\"ap_always\":%s,\"reboot_pending\":%s,"
              "\"kvm_on\":%s,\"kvm_host\":\"%s\",\"kvm_port\":%u,"
              "\"kvm_screen\":\"%s\",\"kvm_w\":%u,\"kvm_h\":%u,"
              "\"kvm_state\":\"%s\",\"kvm_server\":\"%s\","
              "\"kvm_tls\":%s,\"kvm_fp\":\"%s\",\"kvm_ca_set\":%s}",
              config_.staSsid(),
              config_.staPassword()[0] ? "true" : "false",
              config_.authToken()[0]   ? "true" : "false",
              config_.deviceName(),
              config_.apAlways()      ? "true" : "false",
              config_.rebootPending() ? "true" : "false",
              config_.deskflowEnabled() ? "true" : "false",
              config_.deskflowHost(),
              (unsigned)config_.deskflowPort(),
              config_.deskflowScreen(),
              (unsigned)config_.deskflowWidth(),
              (unsigned)config_.deskflowHeight(),
              deskflow_ ? deskflow_->statusText() : "unknown",
              deskflow_ ? deskflow_->serverName() : "",
              config_.deskflowTls() ? "true" : "false",
              deskflow_ ? deskflow_->fingerprint() : "",
              config_.deskflowServerCert()[0] ? "true" : "false");
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
