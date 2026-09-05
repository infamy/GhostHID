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
#include "net/TlsArena.h"
#include "net/Network.h"
#include "protocol/CommandProcessor.h"
#include "ui/Display.h"

namespace {

ghosthid::Config           config;
ghosthid::HidDevice        hid;
ghosthid::CommandProcessor processor(hid, config);
ghosthid::Network          network(processor, config);
ghosthid::DeskflowClient   deskflow(hid, config);
ghosthid::SerialConsole    console(config, processor, network, deskflow);
#ifdef GHOSTHID_HAS_LCD
ghosthid::Display          display;
#endif

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

// Largest contiguous allocation available at each stage of boot. TLS needs a
// 16KB block, so knowing which stage costs it is the difference between fixing
// this and guessing at it.
uint32_t g_heapAfterBoot = 0, g_heapAfterWifi = 0, g_heapAfterServer = 0;

// True once the web server is running. After that the largest contiguous heap
// block is roughly a third of what it was, which is the difference between a
// TLS handshake succeeding and failing.
bool g_bootComplete = false;

void setup() {
    Serial.begin(115200);
    // Never let the USB-CDC console block the firmware. arduino-esp32's USBCDC
    // blocks Serial.write() when a host has the port open but is not draining it
    // fast enough - e.g. a serial monitor over a slow (SSH) link. A blocked
    // write stalls whatever task is logging; during a screen-client reconnect
    // that is the Deskflow task, which then misses keep-alives and is dropped by
    // the server, which triggers another reconnect and more logging - a
    // self-sustaining stall loop. Timeout 0 makes the console drop output rather
    // than ever block on it, so attaching a monitor can never wedge the device.
    Serial.setTxTimeoutMs(0);
    g_heapAfterBoot = ESP.getMaxAllocHeap();
    ledBegin();
    buttonBegin();

    // Settings must load before the radio comes up: they carry the SSID,
    // passphrases and token the network layer needs.
    config.begin();
    processor.attachDeskflow(&deskflow);
    network.attachDeskflow(&deskflow);

    // Reserve the TLS buffers now, while the heap is still whole. Doing it
    // later is the whole problem: once the web server has run, the largest
    // free block is around 13KB and a 16KB request cannot be satisfied, so a
    // dropped session could not reconnect without a reboot. Only reserved when
    // TLS is actually going to be used, since it is permanent.
    if (config.deskflowEnabled() && config.deskflowTls()) {
        ghosthid::TlsArena::begin();
    }

    hid.begin();
    const bool enumerated = hid.waitUntilReady(10000);

    delay(500);
    Serial.println();
    Serial.println("=== GhostHID " GHOSTHID_VERSION " ===");
    Serial.printf("USB enumerated: %s\r\n", enumerated ? "yes" : "no (timed out)");

    // Networking comes up whether or not USB enumerated, so the device stays
    // reachable and diagnosable when plugged into a dumb charger or a port
    // that never configured it.
    network.beginRadio();
    g_heapAfterWifi = ESP.getMaxAllocHeap();

    // Order matters. A TLS handshake needs a 16KB contiguous block plus more
    // after it, and starting the web server drops the largest available block
    // from roughly 135KB to 47KB. So give the screen client its handshake
    // while the heap is still whole, then start the servers.
    if (config.deskflowEnabled() && config.deskflowTls()) {
        // Mark the attempt before making it. If the device does not get as far
        // as clearing this, the next boot disables the client rather than
        // looping - the web server must always come up, because it is the only
        // way back in once the device is not on a cable.
        config.markDeskflowAttempt(true);
        deskflow.begin();
        Serial.println("[boot] giving the screen client the heap before the web server");
        const uint32_t until = millis() + 8000;
        while (millis() < until && !deskflow.connected()) delay(100);
        config.markDeskflowAttempt(false);
        Serial.printf("[boot] screen client %s\r\n",
                      deskflow.connected() ? "connected" : "not connected, carrying on");
    } else {
        deskflow.begin();
    }

    network.beginServers();
    g_heapAfterServer = ESP.getMaxAllocHeap();
    g_bootComplete = true;

    Serial.printf("[auth] token %s\r\n",
                  (config.authToken()[0] == '\0') ? "disabled" : "required");
    Serial.println("Press BOOT to release all held input.");
    console.begin();

#ifdef GHOSTHID_HAS_LCD
    display.begin();
#endif
}

#ifdef GHOSTHID_HAS_LCD
// Push current state to the LCD, but only when it actually changed - a full
// redraw every tick would flicker and waste SPI time. The KVM field is reduced
// to off/connecting/connected so a rapidly-toggling focus state does not force
// constant redraws.
void serviceDisplay() {
    display.loop();                     // backlight timeout, every pass
    static uint32_t last = 0;
    if (millis() - last < 750) return;
    last = millis();

    ghosthid::DisplayStatus st;
    st.deviceName = config.deviceName();
    st.apSsid     = network.ssid();
    st.apPass     = config.apPassword();
    st.apIp       = network.apAddress();
    st.staIp      = network.staAddress();
    st.usbReady   = hid.ready();
    st.kvmState   = !config.deskflowEnabled() ? "off"
                    : (deskflow.connected() ? "connected" : "connecting");
    st.clients    = network.clientConnected() ? 1 : 0;

    char sig[192];
    snprintf(sig, sizeof(sig), "%s|%s|%s|%s|%d|%s|%d",
             st.deviceName, st.apSsid, st.apIp, st.staIp,
             st.usbReady ? 1 : 0, st.kvmState, st.clients);
    static char lastSig[192] = {0};
    if (strcmp(sig, lastSig) != 0) {
        strncpy(lastSig, sig, sizeof(lastSig) - 1);
        display.showStatus(st);
    }
}
#endif

void loop() {
    network.loop();
    console.feed();
#ifdef GHOSTHID_HAS_LCD
    serviceDisplay();
#endif

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
