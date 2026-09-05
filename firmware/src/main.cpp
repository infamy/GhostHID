// GhostHID - Phase 2: Wi-Fi command receiver
//
// Plug the board into a target computer. It enumerates as a composite USB
// keyboard + mouse, brings up its own WPA2 access point, and serves a
// WebSocket that accepts keyboard and mouse commands.
//
// Layering (PLAN.md "Architecture Principles"):
//
//   Network  ->  CommandProcessor  ->  HidDevice  ->  USB
//
// Network never touches HID; HidDevice never learns where a command came from.

#include <Arduino.h>

#include "board_config.h"
#include "config/Config.h"
#include "config/SerialConsole.h"
#include "hid/HidDevice.h"
#include "net/DeskflowClient.h"
#include "net/Network.h"
#include "protocol/CommandProcessor.h"

namespace {

ghosthid::Config           config;
ghosthid::HidDevice        hid;
ghosthid::CommandProcessor processor(hid, config);
ghosthid::Network          network(processor, config);
ghosthid::DeskflowClient   deskflow(hid, config);
ghosthid::SerialConsole    console(config, processor, network, deskflow);

// --- Status LED ------------------------------------------------------------

void ledBegin() {
#if GHOSTHID_PIN_LED >= 0
    pinMode(GHOSTHID_PIN_LED, OUTPUT);
#endif
}

void ledSet(bool on) {
#if GHOSTHID_PIN_LED >= 0
#if GHOSTHID_PIN_LED_ACTIVE_LOW
    digitalWrite(GHOSTHID_PIN_LED, on ? LOW : HIGH);
#else
    digitalWrite(GHOSTHID_PIN_LED, on ? HIGH : LOW);
#endif
#else
    (void)on;
#endif
}

// --- BOOT button (edge-triggered, debounced) -------------------------------

void buttonBegin() {
#if GHOSTHID_PIN_BUTTON >= 0
    pinMode(GHOSTHID_PIN_BUTTON, GHOSTHID_PIN_BUTTON_ACTIVE_LOW ? INPUT_PULLUP : INPUT_PULLDOWN);
#endif
}

bool buttonPressedRaw() {
#if GHOSTHID_PIN_BUTTON >= 0
    const int level = digitalRead(GHOSTHID_PIN_BUTTON);
    return GHOSTHID_PIN_BUTTON_ACTIVE_LOW ? (level == LOW) : (level == HIGH);
#else
    return false;
#endif
}

bool buttonJustPressed() {
    static bool wasPressed = false;
    static uint32_t lastChangeMs = 0;
    const bool now = buttonPressedRaw();
    if (now != wasPressed && (millis() - lastChangeMs) > 50) {
        lastChangeMs = millis();
        wasPressed = now;
        return now;
    }
    return false;
}

}  // namespace

void setup() {
    Serial.begin(115200);
    ledBegin();
    buttonBegin();

    // Settings must load before the radio comes up: they carry the SSID,
    // passphrases and token the network layer needs.
    config.begin();

    hid.begin();
    const bool enumerated = hid.waitUntilReady(10000);

    delay(500);
    Serial.println();
    Serial.println("=== GhostHID " GHOSTHID_VERSION " ===");
    Serial.printf("USB enumerated: %s\r\n", enumerated ? "yes" : "no (timed out)");

    // Networking comes up whether or not USB enumerated, so the device stays
    // reachable and diagnosable when plugged into a dumb charger or a port
    // that never configured it.
    network.begin();

    Serial.printf("[auth] token %s\r\n",
                  (config.authToken()[0] == '\0') ? "disabled" : "required");
    Serial.println("Press BOOT to release all held input.");
    console.begin();
}

void loop() {
    network.loop();
    console.feed();
    deskflow.loop();

    // Reboot requested over the API (config change). Done here rather than in
    // the network callback so the stack is not torn down from inside itself.
    if (processor.rebootRequested()) {
        Serial.println("[config] rebooting to apply settings");
        delay(250);              // let the WebSocket reply flush first
        ESP.restart();
    }

    // Backstop for abrupt link loss. A clean disconnect already released
    // everything via CommandProcessor::endSession().
    if (processor.serviceWatchdog(GHOSTHID_HEARTBEAT_TIMEOUT_MS)) {
        Serial.println("[watchdog] controller went quiet - released all input");
    }

    // Physical panic button: release everything, no network required.
    if (buttonJustPressed()) {
        hid.releaseAll();
        Serial.println("[button] released all input");
    }

    // Solid while a controller is connected, slow pulse when idle.
    static uint32_t lastBlink = 0;
    if (network.clientConnected()) {
        ledSet(true);
    } else if (millis() - lastBlink > 2000) {
        lastBlink = millis();
        ledSet(true); delay(40); ledSet(false);
    }

    delay(2);
}
