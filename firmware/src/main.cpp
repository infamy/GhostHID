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

// A debounced button with short/long/very-long distinction. Long and very-long
// each fire once at their threshold (while still held); short fires on release
// if it never became a long press. On a board with an LCD, short cycles pages
// and long is the panic release; without an LCD any press is the panic release.
// Very-long (~5s) is the sealed-mode unseal gesture: it re-enables the serial
// console on a sealed device (see loop()).
enum class BtnEvent : uint8_t { None, Short, Long, VeryLong };
constexpr uint32_t kBtnLongMs     = 800;
constexpr uint32_t kBtnVeryLongMs = 5000;

BtnEvent buttonEvent() {
#if GHOSTHID_PIN_BUTTON >= 0
    static bool wasDown = false;
    static uint32_t downAt = 0;
    static bool longFired = false;
    static bool veryLongFired = false;
    const bool down = buttonPressedRaw();
    const uint32_t now = millis();
    if (down && !wasDown) {
        wasDown = true; downAt = now; longFired = false; veryLongFired = false;
    } else if (down && wasDown && !veryLongFired && (now - downAt) >= kBtnVeryLongMs) {
        veryLongFired = true; return BtnEvent::VeryLong;
    } else if (down && wasDown && !longFired && (now - downAt) >= kBtnLongMs) {
        longFired = true; return BtnEvent::Long;
    } else if (!down && wasDown) {
        wasDown = false;
        if (!longFired && (now - downAt) > 30) return BtnEvent::Short;
    }
#endif
    return BtnEvent::None;
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
    // Whether this boot has a USB serial console was decided at static init
    // (usb_serial.cpp) from the sealed / unseal-window NVS flags - it must be, as
    // the CDC interface can only join the descriptor before USB.begin(). Here we
    // just start the driver on the CDC if one exists (unsealed, or an unseal
    // window); a sealed boot has none and every Serial.* call is a safe no-op.
    if (UsbSerial.present()) {
        Serial.begin(115200);
        // Never let the USB-CDC console block the firmware. arduino-esp32's USBCDC
        // blocks Serial.write() when a host has the port open but is not draining
        // it fast enough - e.g. a serial monitor over a slow (SSH) link. A blocked
        // write stalls whatever task is logging; during a screen-client reconnect
        // that is the Deskflow task, which then misses keep-alives and is dropped
        // by the server, which triggers another reconnect and more logging - a
        // self-sustaining stall loop. Timeout 0 makes the console drop output
        // rather than block on it, so a monitor can never wedge the device.
        Serial.setTxTimeoutMs(0);
    }
    g_heapAfterBoot = ESP.getMaxAllocHeap();
    ledBegin();
    buttonBegin();

    // Settings must load before the radio comes up: they carry the SSID,
    // passphrases and token the network layer needs.
    config.begin();

    // Unseal window: a ~5s BOOT hold on a sealed device set this flag and rebooted
    // so the console could come up (it just did, above). Consume the flag now -
    // one-shot, so if the operator doesn't unseal, the next boot is HID-only again
    // - and arm the `unseal` command. The device stays sealed (network gates hold)
    // until `unseal` actually runs.
    if (config.unsealWindow()) {
        config.setUnsealWindow(false);
        console.armUnseal();
    }

    if (config.justProvisioned()) {
        Serial.println();
        Serial.println("=== First boot: generated per-device credentials ===");
        Serial.printf("  AP password  : %s\r\n", config.apPassword());
        Serial.printf("  Pairing token: %s\r\n", config.authToken());
        Serial.println("  (also on the LCD Wi-Fi page - change them in Settings)");
    }
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
    hid.setInvertScroll(config.scrollInvert());
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
    } else if (config.sealed() && config.deskflowEnabled() && !config.deskflowTls()) {
        // A sealed device refuses the plaintext screen-client path: it is an
        // unauthenticated keystroke channel, which a locked-down deployment must
        // not run. Encrypted (kvm_tls on) KVM is unaffected and handled above.
        Serial.println("[boot] sealed: refusing plaintext screen client "
                       "(turn kvm_tls on to use KVM while sealed)");
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
// Build the current status and hand it to the display, which rate-limits,
// change-detects and redraws only what changed (LCD + RGB LED).
void serviceDisplay() {
    ghosthid::DisplayStatus st;
    st.deviceName = config.deviceName();
    st.apSsid     = network.ssid();
    st.apPass     = config.apPassword();
    st.token      = config.authToken();
    st.apIp       = network.apAddress();
    st.staIp      = network.staAddress();
    st.usbReady   = hid.ready();
    st.kvmState   = !config.deskflowEnabled() ? "off"
                    : deskflow.certTrustPending() ? "confirm cert"
                    : (deskflow.connected() ? "connected" : "connecting");
    st.kvmFocus   = deskflow.hasFocus();
    st.clients    = (int)network.clientCount();
    st.version    = GHOSTHID_VERSION;
    st.heapFreeKb = ESP.getFreeHeap() / 1024;
    st.uptimeSec  = millis() / 1000;
    if (WiFi.status() == WL_CONNECTED) {
        st.rssi    = WiFi.RSSI();
        st.channel = WiFi.channel();
    }
    st.capsLock   = hid.capsLock();
    st.numLock    = hid.numLock();
    st.scrollLock = hid.scrollLock();
    st.sealed     = config.sealed();
    st.unsealArmed = console.unsealArmed();
    display.update(st);
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

    // Backstop for an OTA that locked the HID and then never completed (client
    // vanished mid-upload). Without this the device would refuse all input
    // until a manual reboot.
    if (processor.lockedTooLong(60000)) {
        processor.setLocked(false, nullptr);
        Serial.println("[watchdog] OTA lock timed out - input re-enabled");
    }

    // Physical BOOT button. On an LCD board a short press cycles the screen
    // pages and a long press is the panic release; without an LCD any press is
    // the panic release. The release path is never lost.
    switch (buttonEvent()) {
        case BtnEvent::VeryLong:
            // ~5s hold on a SEALED device requests unsealing. A sealed device has
            // no serial console and one cannot be added at runtime (TinyUSB refuses
            // an interface once USB has started), so persist a one-shot window flag
            // and reboot: the next boot brings the console up and arms `unseal`
            // (see setup()). The long-press panic release already fired at 800ms.
            // No-op on an unsealed device.
            if (config.sealed() && !config.unsealWindow()) {
                config.setUnsealWindow(true);
                delay(150);
                ESP.restart();
            }
            break;
        case BtnEvent::Short:
#ifdef GHOSTHID_HAS_LCD
            display.nextPage();
            break;
#endif
            // fall through: no LCD, a short press is a release
        case BtnEvent::Long:
            hid.releaseAll();
            Serial.println("[button] released all input");
            break;
        default:
            break;
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
