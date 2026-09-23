// Host-test recording stub for HidDevice. Implements the out-of-line methods
// (the inline ones live in HidDevice.h) with realistic held-state tracking, so
// CommandProcessor's orchestration — especially "release everything on
// disconnect" — can be asserted without any USB hardware.
#include "hid/HidDevice.h"
#include "test_hooks.h"

namespace hidhook {
int     releaseAllCalls = 0;
int     keyDownCalls = 0;
int     keyUpCalls = 0;
int     typeTextCalls = 0;
int     mouseMoveCalls = 0;
int     mouseAbsCalls = 0;
int     mouseWheelCalls = 0;
int     mouseBtnDownCalls = 0;
int     mediaCalls = 0;
int     systemCalls = 0;
uint8_t lastKeyDown = 0;
bool    ready = true;
bool    endpointBusy = false;
float   lastAbsX = -1;
int32_t lastRelDx = 0;
void reset() {
    releaseAllCalls = keyDownCalls = keyUpCalls = typeTextCalls = 0;
    mouseMoveCalls = mouseAbsCalls = mouseWheelCalls = mouseBtnDownCalls = 0;
    mediaCalls = systemCalls = 0;
    lastKeyDown = 0;
    ready = true;
    endpointBusy = false;
    lastAbsX = -1;
    lastRelDx = 0;
}
}  // namespace hidhook

namespace ghosthid {

void HidDevice::begin() { begun_ = true; }
bool HidDevice::ready() const { return hidhook::ready; }
bool HidDevice::waitUntilReady(uint32_t) { return hidhook::ready; }

void HidDevice::keyDown(uint8_t key) {
    hidhook::keyDownCalls++; hidhook::lastKeyDown = key;
    if (heldKeyCount_ < kMaxHeldKeys) heldKeys_[heldKeyCount_++] = key;
}
void HidDevice::keyUp(uint8_t key) {
    hidhook::keyUpCalls++;
    for (size_t i = 0; i < heldKeyCount_; ++i) {
        if (heldKeys_[i] == key) {
            heldKeys_[i] = heldKeys_[--heldKeyCount_];
            break;
        }
    }
}
void HidDevice::tapKey(uint8_t key, uint32_t) { keyDown(key); keyUp(key); }
void HidDevice::typeText(const char *) { hidhook::typeTextCalls++; }

void HidDevice::mouseMove(int32_t dx, int32_t) { hidhook::mouseMoveCalls++; hidhook::lastRelDx = dx; }
void HidDevice::mouseMoveAbsolute(float x, float y) {
    hidhook::mouseAbsCalls++;
    hidhook::lastAbsX = x;
    absX_ = x < 0 ? 0 : (x > 1 ? 1 : x);
    absY_ = y < 0 ? 0 : (y > 1 ? 1 : y);
}
uint32_t HidDevice::droppedReports() const { return 0; }
bool HidDevice::tryMouseMoveAbsolute(float x, float y) {
    if (hidhook::endpointBusy) { ++pointerBusy_; return false; }
    mouseMoveAbsolute(x, y);
    return true;
}
bool HidDevice::tryMouseMoveStep(int32_t &dx, int32_t &dy) {
    if (dx == 0 && dy == 0) return true;
    if (hidhook::endpointBusy) { ++pointerBusy_; return false; }
    const int32_t sx = dx > 127 ? 127 : (dx < -127 ? -127 : dx);
    const int32_t sy = dy > 127 ? 127 : (dy < -127 ? -127 : dy);
    hidhook::mouseMoveCalls++;
    hidhook::lastRelDx = sx;
    dx -= sx;
    dy -= sy;
    return true;
}
void HidDevice::mouseButtonDown(MouseButton b) {
    hidhook::mouseBtnDownCalls++;
    heldMouseButtons_ |= (uint8_t)(1u << (uint8_t)b);
}
void HidDevice::mouseButtonUp(MouseButton b) {
    heldMouseButtons_ &= (uint8_t)~(1u << (uint8_t)b);
}
void HidDevice::mouseClick(MouseButton b, uint32_t) { mouseButtonDown(b); mouseButtonUp(b); }
void HidDevice::mouseWheel(int32_t) { hidhook::mouseWheelCalls++; }
void HidDevice::mousePan(int32_t) {}

void HidDevice::mediaKey(MediaKey) { hidhook::mediaCalls++; }
void HidDevice::systemKey(SystemKey) { hidhook::systemCalls++; }

bool HidDevice::capsLock() const { return false; }
bool HidDevice::numLock() const { return false; }
bool HidDevice::scrollLock() const { return false; }
uint8_t HidDevice::hostLeds() const { return 0; }
uint32_t HidDevice::hostLedReports() const { return 0; }

void HidDevice::releaseAll() {
    hidhook::releaseAllCalls++;
    heldKeyCount_ = 0;
    heldMouseButtons_ = 0;
}
bool HidDevice::anythingHeld() const { return heldKeyCount_ > 0 || heldMouseButtons_ != 0; }

// Private helpers referenced by nothing in the tested path, but defined so the
// unit links cleanly if the compiler emits references.
void HidDevice::sendMouseReport(int8_t, int8_t, int8_t, int8_t) {}
void HidDevice::trackKeyDown(uint8_t) {}
void HidDevice::trackKeyUp(uint8_t) {}

}  // namespace ghosthid
