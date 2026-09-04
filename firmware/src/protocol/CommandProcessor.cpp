#include "CommandProcessor.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "Keymap.h"
#include "board_config.h"
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
    authenticated_  = (token_ == nullptr || token_[0] == '\0');
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
        if (token_ != nullptr && token_[0] != '\0' && strcmp(given, token_) != 0) {
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
        reply(outResponse, outSize, "{\"type\":\"pong\",\"usb\":%s}",
              hid_.ready() ? "true" : "false");
        return CommandResult::Ok;
    }

    if (strcmp(type, "status") == 0) {
        reply(outResponse, outSize,
              "{\"type\":\"status\",\"version\":\"%s\",\"usb\":%s,\"held\":%u}",
              GHOSTHID_VERSION, hid_.ready() ? "true" : "false",
              static_cast<unsigned>(hid_.heldKeyCount()));
        return CommandResult::Ok;
    }

    // Refuse input commands when the target has not enumerated us. Silently
    // dropping them - which is what the HID layer does on its own - looks
    // identical to success from the controller's side, and that is the single
    // most confusing failure this device can present.
    const bool isInput =
        strcmp(type, "key") == 0 || strcmp(type, "text") == 0 ||
        strcmp(type, "mouse_move") == 0 || strcmp(type, "mouse_button") == 0 ||
        strcmp(type, "mouse_wheel") == 0;
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

    reply(outResponse, outSize, "{\"type\":\"error\",\"error\":\"unknown type\"}");
    return CommandResult::BadRequest;
}

}  // namespace ghosthid
