// An absolute-positioning HID pointer, alongside the existing relative mouse.
//
// Why both: relative reports are what a trackpad-style drag wants, but they are
// rescaled by the target's pointer acceleration and accumulate permanent drift,
// so the controller can never know where the pointer actually is. Absolute
// reports are not accelerated - the pointer goes exactly where we say - which
// is what makes "put the pointer at 62% across, 31% down" work, and is a hard
// prerequisite for edge-crossing and for Deskflow-style clients whose wire
// protocols carry absolute screen coordinates.
//
// Both live in the descriptor because relative still wins for fine nudging and
// because some KVMs and BIOS implementations handle absolute pointers poorly.

#pragma once

#include <USBHID.h>
#include <stdint.h>

namespace ghosthid {

// Absolute axes are reported over the full 0..32767 logical range, which is the
// conventional choice: it is resolution-independent, so the firmware never needs
// to know the target's screen size.
constexpr uint16_t kAbsoluteAxisMax = 32767;

class AbsoluteMouse : public USBHIDDevice {
public:
    AbsoluteMouse();

    void begin();

    // x and y are 0..kAbsoluteAxisMax, spanning the target's whole desktop.
    // `buttons` mirrors the relative mouse's held-button mask so the two
    // collections never disagree about button state.
    // Returns false if the host did not accept the report. Silently discarding
    // this hid whether stepping was our cadence or dropped reports.
    bool moveTo(uint16_t x, uint16_t y, uint8_t buttons);
    uint32_t dropped() const { return dropped_; }

    uint16_t _onGetDescriptor(uint8_t *buffer) override;

private:
    USBHID hid_;
    uint32_t dropped_ = 0;
};

}  // namespace ghosthid
