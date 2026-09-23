#include "HidDevice.h"

#include <Arduino.h>
#include <USB.h>
#include <USBHIDKeyboard.h>
#include <USBHIDMouse.h>
#include <USBHIDConsumerControl.h>
#include <USBHIDSystemControl.h>

#include "AbsoluteMouse.h"

#include "board_config.h"

namespace ghosthid {

// Pin our USB-free keycode constants to the framework's. If arduino-esp32 ever
// remaps these, this fails to compile rather than silently typing wrong keys.
static_assert(key::LeftCtrl   == KEY_LEFT_CTRL,   "keycode drift: LeftCtrl");
static_assert(key::LeftShift  == KEY_LEFT_SHIFT,  "keycode drift: LeftShift");
static_assert(key::LeftAlt    == KEY_LEFT_ALT,    "keycode drift: LeftAlt");
static_assert(key::LeftGui    == KEY_LEFT_GUI,    "keycode drift: LeftGui");
static_assert(key::RightCtrl  == KEY_RIGHT_CTRL,  "keycode drift: RightCtrl");
static_assert(key::RightShift == KEY_RIGHT_SHIFT, "keycode drift: RightShift");
static_assert(key::RightAlt   == KEY_RIGHT_ALT,   "keycode drift: RightAlt");
static_assert(key::RightGui   == KEY_RIGHT_GUI,   "keycode drift: RightGui");
static_assert(key::Return     == KEY_RETURN,      "keycode drift: Return");
static_assert(key::Escape     == KEY_ESC,         "keycode drift: Escape");
static_assert(key::Backspace  == KEY_BACKSPACE,   "keycode drift: Backspace");
static_assert(key::Tab        == KEY_TAB,         "keycode drift: Tab");
static_assert(key::CapsLock   == KEY_CAPS_LOCK,   "keycode drift: CapsLock");
static_assert(key::Insert     == KEY_INSERT,      "keycode drift: Insert");
static_assert(key::Home       == KEY_HOME,        "keycode drift: Home");
static_assert(key::PageUp     == KEY_PAGE_UP,     "keycode drift: PageUp");
static_assert(key::Delete     == KEY_DELETE,      "keycode drift: Delete");
static_assert(key::End        == KEY_END,         "keycode drift: End");
static_assert(key::PageDown   == KEY_PAGE_DOWN,   "keycode drift: PageDown");
static_assert(key::RightArrow == KEY_RIGHT_ARROW, "keycode drift: RightArrow");
static_assert(key::LeftArrow  == KEY_LEFT_ARROW,  "keycode drift: LeftArrow");
static_assert(key::DownArrow  == KEY_DOWN_ARROW,  "keycode drift: DownArrow");
static_assert(key::UpArrow    == KEY_UP_ARROW,    "keycode drift: UpArrow");
static_assert(key::F1         == KEY_F1,          "keycode drift: F1");
static_assert(key::F12        == KEY_F12,         "keycode drift: F12");

namespace {

USBHIDKeyboard       g_keyboard;
USBHIDMouse          g_mouse;
AbsoluteMouse        g_absMouse;
USBHIDConsumerControl g_consumer;
USBHIDSystemControl   g_system;

// Host lock-LED state, written from the USB event task and read from any task.
// Plain aligned words: a torn read is impossible on this core and the worst a
// stale read costs is one frame of a wrong indicator, so no lock is warranted
// (matching the lockless read policy documented above mouseMove).
volatile uint8_t  s_hostLeds       = 0;
volatile uint32_t s_hostLedReports = 0;

// Fired by the USB stack whenever the host sends a keyboard output report -
// i.e. the lock-key LED state changed, or the device was just configured. This
// runs on the Arduino USB event task, not a HID caller, so it only stores.
void onKeyboardLed(void *, esp_event_base_t, int32_t id, void *data) {
    if (id != ARDUINO_USB_HID_KEYBOARD_LED_EVENT) return;
    auto *d = static_cast<arduino_usb_hid_keyboard_event_data_t *>(data);
    s_hostLeds = d->leds;
    ++s_hostLedReports;
}

uint16_t mediaUsage(MediaKey k) {
    switch (k) {
        case MediaKey::VolumeUp:       return CONSUMER_CONTROL_VOLUME_INCREMENT;
        case MediaKey::VolumeDown:     return CONSUMER_CONTROL_VOLUME_DECREMENT;
        case MediaKey::Mute:           return CONSUMER_CONTROL_MUTE;
        case MediaKey::PlayPause:      return CONSUMER_CONTROL_PLAY_PAUSE;
        case MediaKey::Next:           return CONSUMER_CONTROL_SCAN_NEXT;
        case MediaKey::Previous:       return CONSUMER_CONTROL_SCAN_PREVIOUS;
        case MediaKey::Stop:           return CONSUMER_CONTROL_STOP;
        case MediaKey::BrightnessUp:   return CONSUMER_CONTROL_BRIGHTNESS_INCREMENT;
        case MediaKey::BrightnessDown: return CONSUMER_CONTROL_BRIGHTNESS_DECREMENT;
    }
    return 0;
}

uint8_t systemUsage(SystemKey k) {
    switch (k) {
        case SystemKey::Sleep:    return SYSTEM_CONTROL_STANDBY;
        case SystemKey::PowerOff: return SYSTEM_CONTROL_POWER_OFF;
        case SystemKey::Wake:     return SYSTEM_CONTROL_WAKE_HOST;
    }
    return SYSTEM_CONTROL_NONE;
}

inline uint8_t mouseButtonMask(MouseButton button) {
    switch (button) {
        case MouseButton::Left:   return MOUSE_LEFT;
        case MouseButton::Right:  return MOUSE_RIGHT;
        case MouseButton::Middle: return MOUSE_MIDDLE;
    }
    return 0;
}

inline void reportGap() {
#if GHOSTHID_HID_REPORT_GAP_MS > 0
    delay(GHOSTHID_HID_REPORT_GAP_MS);
#endif
}

// Clamp an int32 delta into one HID report's signed 8-bit field.
inline int8_t clampStep(int32_t v) {
    if (v >  GHOSTHID_MOUSE_MAX_STEP) return  GHOSTHID_MOUSE_MAX_STEP;
    if (v < -GHOSTHID_MOUSE_MAX_STEP) return -GHOSTHID_MOUSE_MAX_STEP;
    return static_cast<int8_t>(v);
}

// RAII lock for the HID mutex. Tolerates a null handle (before begin()) so the
// single-threaded boot path works without a special case.
struct Lock {
    SemaphoreHandle_t m;
    explicit Lock(SemaphoreHandle_t mm) : m(mm) {
        if (m) xSemaphoreTake(m, portMAX_DELAY);
    }
    ~Lock() { if (m) xSemaphoreGive(m); }
    Lock(const Lock &) = delete;
    Lock &operator=(const Lock &) = delete;
};

}  // namespace

void HidDevice::begin() {
    if (!mutex_) mutex_ = xSemaphoreCreateMutex();
    if (begun_) return;

    g_keyboard.begin();
    g_mouse.begin();
    g_absMouse.begin();
    g_consumer.begin();
    g_system.begin();

    // Listen for the host's keyboard output reports (lock-LED state). This is
    // our only channel of feedback *from* the target.
    g_keyboard.onEvent(ARDUINO_USB_HID_KEYBOARD_LED_EVENT, onKeyboardLed);

    // NOTE: do not set VID/PID/product name here. When ARDUINO_USB_CDC_ON_BOOT=1
    // the USB stack is already running by the time setup() executes, so these
    // setters are silently ignored and the device enumerates under the board's
    // default identity. The identity is set at build time instead -- see the
    // -DUSB_* flags in platformio.ini.
    USB.begin();

    begun_ = true;
}

bool HidDevice::ready() const {
    // USBDevice reports "mounted" once the host has configured the device.
    return begun_ && static_cast<bool>(USB);
}

bool HidDevice::waitUntilReady(uint32_t timeoutMs) {
    const uint32_t start = millis();
    while (!ready() && (millis() - start) < timeoutMs) {
        delay(10);
    }
    return ready();
}

// --- Keyboard --------------------------------------------------------------

void HidDevice::trackKeyDown(uint8_t key) {
    for (size_t i = 0; i < heldKeyCount_; ++i) {
        if (heldKeys_[i] == key) return;  // already held
    }
    if (heldKeyCount_ < kMaxHeldKeys) {
        heldKeys_[heldKeyCount_++] = key;
    }
}

void HidDevice::trackKeyUp(uint8_t key) {
    for (size_t i = 0; i < heldKeyCount_; ++i) {
        if (heldKeys_[i] == key) {
            heldKeys_[i] = heldKeys_[--heldKeyCount_];
            return;
        }
    }
}

void HidDevice::keyDown(uint8_t key) {
    Lock lk(mutex_);
    if (!ready()) return;
    g_keyboard.press(key);
    trackKeyDown(key);
    reportGap();
}

void HidDevice::keyUp(uint8_t key) {
    Lock lk(mutex_);
    if (!ready()) return;
    g_keyboard.release(key);
    trackKeyUp(key);
    reportGap();
}

void HidDevice::tapKey(uint8_t key, uint32_t holdMs) {
    // No lock here: this composes the locking keyDown/keyUp, and holding the
    // mutex across delay(holdMs) would block every other task from the HID for
    // the hold duration.
    keyDown(key);
    delay(holdMs);
    keyUp(key);
}

void HidDevice::typeText(const char *text) {
    Lock lk(mutex_);
    if (!ready() || text == nullptr) return;
    for (const char *p = text; *p != '\0'; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c > 0x7F) continue;  // not representable on a US HID keyboard
        g_keyboard.write(c);
        reportGap();
    }
}

// --- Mouse -----------------------------------------------------------------

void HidDevice::sendMouseReport(int8_t dx, int8_t dy, int8_t wheel, int8_t pan) {
    if (!ready()) return;
    g_mouse.move(dx, dy, wheel, pan);
    reportGap();
}

// The pointer-motion methods below are deliberately LOCKLESS. They are the hot
// path (up to ~200 reports/sec during a drag) and they touch only the mouse /
// absolute-mouse report objects, never the shared keyboard report that the
// mutex exists to protect. Serialising them against the keyboard path added
// contention that showed up as choppy motion and delayed the screen client's
// keep-alive replies enough for the server to drop it. A stale read of
// heldMouseButtons_ here (written under the lock by the button methods) costs at
// most one frame of wrong button state, which is cosmetic and self-correcting.
void HidDevice::mouseMove(int32_t dx, int32_t dy) {
    if (!ready()) return;
    // Split large movements across as many reports as needed. Without this a
    // caller asking for +400px would silently get a wrapped/clamped delta.
    while (dx != 0 || dy != 0) {
        const int8_t sx = clampStep(dx);
        const int8_t sy = clampStep(dy);
        sendMouseReport(sx, sy, 0, 0);
        dx -= sx;
        dy -= sy;
    }
}

uint32_t HidDevice::droppedReports() const { return g_absMouse.dropped(); }

void HidDevice::mouseMoveAbsolute(float x, float y) {
    if (!ready()) return;   // lockless hot path - see note above mouseMove

    if (x < 0.0f) x = 0.0f; else if (x > 1.0f) x = 1.0f;
    if (y < 0.0f) y = 0.0f; else if (y > 1.0f) y = 1.0f;
    absX_ = x;
    absY_ = y;

    // Buttons are mirrored from the relative mouse's held mask so the two
    // pointer collections never report contradictory button state.
    g_absMouse.moveTo(static_cast<uint16_t>(x * kAbsoluteAxisMax),
                      static_cast<uint16_t>(y * kAbsoluteAxisMax),
                      heldMouseButtons_);
    reportGap();
}

bool HidDevice::tryMouseMoveAbsolute(float x, float y) {
    if (!ready()) return true;   // nothing can be sent; don't hold it pending forever
    if (!g_absMouse.endpointReady()) { ++pointerBusy_; return false; }
    mouseMoveAbsolute(x, y);
    return true;
}

bool HidDevice::tryMouseMoveStep(int32_t &dx, int32_t &dy) {
    if (!ready()) { dx = dy = 0; return true; }
    if (dx == 0 && dy == 0) return true;
    if (!g_absMouse.endpointReady()) { ++pointerBusy_; return false; }
    const int8_t sx = clampStep(dx);
    const int8_t sy = clampStep(dy);
    sendMouseReport(sx, sy, 0, 0);
    dx -= sx;
    dy -= sy;
    return true;
}

void HidDevice::mouseButtonDown(MouseButton button) {
    Lock lk(mutex_);
    if (!ready()) return;
    const uint8_t mask = mouseButtonMask(button);
    g_mouse.press(mask);
    heldMouseButtons_ |= mask;
    reportGap();
}

void HidDevice::mouseButtonUp(MouseButton button) {
    Lock lk(mutex_);
    if (!ready()) return;
    const uint8_t mask = mouseButtonMask(button);
    g_mouse.release(mask);
    heldMouseButtons_ &= static_cast<uint8_t>(~mask);
    reportGap();
}

void HidDevice::mouseClick(MouseButton button, uint32_t holdMs) {
    // No lock: composes the locking button methods, and the mutex must not be
    // held across delay(holdMs).
    mouseButtonDown(button);
    delay(holdMs);
    mouseButtonUp(button);
}

void HidDevice::mouseWheel(int32_t delta) {
    if (!ready()) return;   // lockless hot path - see note above mouseMove
    if (invertScroll_) delta = -delta;
    while (delta != 0) {
        const int8_t step = clampStep(delta);
        sendMouseReport(0, 0, step, 0);
        delta -= step;
    }
}

void HidDevice::mousePan(int32_t delta) {
    if (!ready()) return;   // lockless hot path - see note above mouseMove
    if (invertScroll_) delta = -delta;
    while (delta != 0) {
        const int8_t step = clampStep(delta);
        sendMouseReport(0, 0, 0, step);
        delta -= step;
    }
}

// --- Media / system --------------------------------------------------------
// These take the mutex: like the keyboard path they mutate a shared report
// object read-modify-write, so they must not race releaseAll() or each other.

void HidDevice::mediaKey(MediaKey k) {
    Lock lk(mutex_);
    if (!ready()) return;
    const uint16_t usage = mediaUsage(k);
    if (usage == 0) return;
    g_consumer.press(usage);
    g_consumer.release();
    reportGap();
}

void HidDevice::systemKey(SystemKey k) {
    Lock lk(mutex_);
    if (!ready()) return;
    g_system.press(systemUsage(k));
    g_system.release();
    reportGap();
}

// --- Host feedback ---------------------------------------------------------

uint8_t  HidDevice::hostLeds() const       { return s_hostLeds; }
uint32_t HidDevice::hostLedReports() const { return s_hostLedReports; }
// Bit order is the USB boot-keyboard output report: b0 Num, b1 Caps, b2 Scroll.
bool HidDevice::numLock()    const { return (s_hostLeds & 0x01) != 0; }
bool HidDevice::capsLock()   const { return (s_hostLeds & 0x02) != 0; }
bool HidDevice::scrollLock() const { return (s_hostLeds & 0x04) != 0; }

// --- Safety ----------------------------------------------------------------

void HidDevice::releaseAll() {
    Lock lk(mutex_);
    if (!begun_) return;

    // Always send the release reports even if our bookkeeping says nothing is
    // held: our state can drift from the host's (e.g. after a reset), and a
    // stuck Ctrl on the target computer is far worse than a redundant report.
    //
    // The lock is what makes this actually safe: without it, a keyDown() on
    // another task could slip in between releaseAll()'s zeroing of the report
    // and its send, re-asserting a key we just cleared. See the mutex_ comment.
    g_keyboard.releaseAll();
    g_mouse.release(MOUSE_LEFT | MOUSE_RIGHT | MOUSE_MIDDLE);

    heldKeyCount_ = 0;
    heldMouseButtons_ = 0;
}

bool HidDevice::anythingHeld() const {
    // Deliberately lockless. This is a watchdog gate read from two tasks (the
    // main loop's stuck-key backstop and the Deskflow held-input check) at up to
    // ~1.5kHz combined; taking the mutex here put those reads in contention with
    // the pointer-report writes on the hot path, which showed up directly as
    // choppy motion. Each field is an aligned word/byte (an atomic load on this
    // core), and a momentarily-stale read only ever costs the watchdog one extra
    // cycle - harmless. The writers still hold the mutex, which is where the
    // actual race was.
    return heldKeyCount_ > 0 || heldMouseButtons_ != 0;
}

}  // namespace ghosthid
