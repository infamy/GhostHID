// GhostHID - HID abstraction layer
//
// This is the ONLY part of the firmware that talks to USB. Transports
// (WebSocket, BLE, ESP-NOW, the self-test in main.cpp) call into this class
// and never touch USB directly, so a new transport is purely additive.
//
// It also owns the *held-input state*, which is what makes "release everything
// on disconnect" possible: whatever put a key down, releaseAll() can lift it.

#pragma once

#include <stdint.h>
#include <stddef.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace ghosthid {

// Named keycodes, so callers never need to include a USB header. Values match
// the arduino-esp32 Keyboard mapping and are static_assert-checked against it
// in HidDevice.cpp, so drift in the framework is a compile error, not a bug.
namespace key {

// Modifiers
constexpr uint8_t LeftCtrl   = 0x80;
constexpr uint8_t LeftShift  = 0x81;
constexpr uint8_t LeftAlt    = 0x82;
constexpr uint8_t LeftGui    = 0x83;   // Windows / Command
constexpr uint8_t RightCtrl  = 0x84;
constexpr uint8_t RightShift = 0x85;
constexpr uint8_t RightAlt   = 0x86;   // AltGr
constexpr uint8_t RightGui   = 0x87;

// Editing / control
constexpr uint8_t Return    = 0xB0;
constexpr uint8_t Escape    = 0xB1;
constexpr uint8_t Backspace = 0xB2;
constexpr uint8_t Tab       = 0xB3;
constexpr uint8_t Space     = ' ';
constexpr uint8_t CapsLock  = 0xC1;

// Navigation
constexpr uint8_t Insert     = 0xD1;
constexpr uint8_t Home       = 0xD2;
constexpr uint8_t PageUp     = 0xD3;
constexpr uint8_t Delete     = 0xD4;
constexpr uint8_t End        = 0xD5;
constexpr uint8_t PageDown   = 0xD6;
constexpr uint8_t RightArrow = 0xD7;
constexpr uint8_t LeftArrow  = 0xD8;
constexpr uint8_t DownArrow  = 0xD9;
constexpr uint8_t UpArrow    = 0xDA;

// Function keys
constexpr uint8_t F1  = 0xC2;
constexpr uint8_t F2  = 0xC3;
constexpr uint8_t F3  = 0xC4;
constexpr uint8_t F4  = 0xC5;
constexpr uint8_t F5  = 0xC6;
constexpr uint8_t F6  = 0xC7;
constexpr uint8_t F7  = 0xC8;
constexpr uint8_t F8  = 0xC9;
constexpr uint8_t F9  = 0xCA;
constexpr uint8_t F10 = 0xCB;
constexpr uint8_t F11 = 0xCC;
constexpr uint8_t F12 = 0xCD;

}  // namespace key

enum class MouseButton : uint8_t {
    Left   = 0,
    Right  = 1,
    Middle = 2,
};

// Consumer-control (media) keys. Named here so callers never touch a USB
// header; HidDevice.cpp maps each to its CONSUMER_CONTROL_* usage.
enum class MediaKey : uint8_t {
    VolumeUp, VolumeDown, Mute,
    PlayPause, Next, Previous, Stop,
    BrightnessUp, BrightnessDown,
};

// System-control keys. These act on the host machine's power state.
enum class SystemKey : uint8_t {
    Sleep, PowerOff, Wake,
};

class HidDevice {
public:
    // Brings up the composite USB device (keyboard + mouse). Call once from
    // setup(). Returns immediately; USB enumeration happens asynchronously.
    void begin();

    // True once the host has enumerated and configured us. Sending reports
    // before this is a no-op.
    bool ready() const;

    // Blocks until ready() or `timeoutMs` elapses. Returns ready().
    bool waitUntilReady(uint32_t timeoutMs);

    // --- Keyboard ----------------------------------------------------------
    // `key` is an arduino-esp32 keycode (KEY_LEFT_CTRL, KEY_RETURN, 'a', ...).

    void keyDown(uint8_t key);
    void keyUp(uint8_t key);
    void tapKey(uint8_t key, uint32_t holdMs = 12);

    // Types a US-layout ASCII string, synthesising shift where needed.
    // Non-ASCII bytes are skipped rather than typed as garbage.
    void typeText(const char *text);

    // --- Mouse -------------------------------------------------------------

    // Relative movement. Deltas outside +/-127 are automatically split across
    // multiple HID reports, so callers can pass any int32 value.
    void mouseMove(int32_t dx, int32_t dy);

    // Absolute positioning, as a fraction of the target's desktop: (0,0) is the
    // top-left corner, (1,1) the bottom-right. Fractions rather than pixels
    // because the firmware has no way to learn the target's resolution, and a
    // fraction stays correct when it changes.
    //
    // Unlike relative movement this is not touched by the target's pointer
    // acceleration, so the pointer lands exactly where asked and the caller
    // always knows where it is. Out-of-range values are clamped.
    void mouseMoveAbsolute(float x, float y);

    // Last absolute position we commanded, as a fraction. Meaningless until
    // mouseMoveAbsolute has been called at least once.
    float absoluteX() const { return absX_; }
    float absoluteY() const { return absY_; }

    // Absolute reports the host refused. Should stay at zero.
    uint32_t droppedReports() const;

    // Non-blocking pointer motion, for the screen client's hot path. The blocking
    // calls above wait for the host to collect each report (up to 100ms in the
    // framework); a host that polls slowly then stalls the caller and the stream
    // of positions backs up behind it, which is felt as lag. These instead return
    // false WITHOUT sending when the HID endpoint is still busy with the previous
    // report, so the caller keeps the motion pending and retries next pass with
    // whatever is newest by then.
    //
    // tryMouseMoveAbsolute: same arguments as mouseMoveAbsolute.
    // tryMouseMoveStep: sends at most ONE relative report (each axis clamped to
    // the report's range) and subtracts what it sent from dx/dy, leaving the
    // remainder for the next call.
    bool tryMouseMoveAbsolute(float x, float y);
    bool tryMouseMoveStep(int32_t &dx, int32_t &dy);

    // Times a non-blocking pointer call found the endpoint busy. High counts mean
    // the host is polling slower than motion arrives.
    uint32_t pointerBusyCount() const { return pointerBusy_; }

    void mouseButtonDown(MouseButton button);
    void mouseButtonUp(MouseButton button);
    void mouseClick(MouseButton button, uint32_t holdMs = 20);

    // Positive = scroll up / right.
    void mouseWheel(int32_t delta);
    void mousePan(int32_t delta);

    // --- Media / system ----------------------------------------------------
    // A tap: the key is pressed and released in one call, which is how
    // consumer/system controls are meant to be sent.

    void mediaKey(MediaKey k);
    void systemKey(SystemKey k);

    // --- Host feedback -----------------------------------------------------
    // The host sends an HID *output* report whenever its lock-key state
    // changes (and once right after it configures the device). Capturing it is
    // the only proof from the target's own side that our keyboard is not just
    // enumerated but actually being driven - and it surfaces Caps/Num/Scroll
    // Lock on a keyboard that has no lights of its own.

    bool capsLock()   const;
    bool numLock()    const;
    bool scrollLock() const;

    // Raw host LED bitmap (bit0 Num, bit1 Caps, bit2 Scroll), as the host last
    // sent it - matches the USB boot-keyboard output report.
    uint8_t hostLeds() const;

    // Count of output reports the host has sent. Non-zero is hard proof the
    // target has our keyboard and is processing it; it climbs on every real
    // lock-key change.
    uint32_t hostLedReports() const;

    // Natural scrolling: invert the wheel (and pan) direction. Applies to every
    // scroll source, so the web trackpad and the screen client agree.
    void setInvertScroll(bool on) { invertScroll_ = on; }
    bool invertScroll() const { return invertScroll_; }

    // --- Safety ------------------------------------------------------------

    // Releases every key and mouse button currently held. Safe to call at any
    // time, including when nothing is held. This is the panic path invoked on
    // controller disconnect and on heartbeat timeout.
    void releaseAll();

    // True if anything at all is currently held down.
    bool anythingHeld() const;

    size_t heldKeyCount() const { return heldKeyCount_; }
    uint8_t heldMouseButtons() const { return heldMouseButtons_; }

private:
    void sendMouseReport(int8_t dx, int8_t dy, int8_t wheel, int8_t pan);
    void trackKeyDown(uint8_t key);
    void trackKeyUp(uint8_t key);

    // Serialises every method that touches the USB report state. HidDevice is
    // called from at least two FreeRTOS tasks - the WebSocket handler on
    // async_tcp and the Deskflow client on its own task - and the framework's
    // keyboard report is a shared object mutated read-modify-write *before* the
    // report is sent. Without this, releaseAll() racing a keyDown() can re-send
    // the very key it just cleared and leave it stuck on the target, which is
    // the one failure this whole layer exists to prevent.
    SemaphoreHandle_t mutex_ = nullptr;

    // A USB boot keyboard reports at most 6 simultaneous non-modifier keys
    // plus 8 modifiers; 16 slots covers any legitimate state with headroom.
    static constexpr size_t kMaxHeldKeys = 16;

    uint8_t heldKeys_[kMaxHeldKeys] = {};
    size_t  heldKeyCount_ = 0;
    uint8_t heldMouseButtons_ = 0;   // bitmask indexed by MouseButton
    bool    invertScroll_ = false;
    bool    begun_ = false;
    float   absX_ = 0.5f;
    float   absY_ = 0.5f;
    volatile uint32_t pointerBusy_ = 0;
};

}  // namespace ghosthid
