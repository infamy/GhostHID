#include "AbsoluteMouse.h"

#include <string.h>

namespace ghosthid {
namespace {

// Report ID 7. The framework's own enum (USBHID.h) runs 1..6 - keyboard, mouse,
// gamepad, consumer, system, vendor - so 7 cannot collide with a device the
// core registers later.
constexpr uint8_t kAbsMouseReportId = 7;

const uint8_t kReportDescriptor[] = {
    0x05, 0x01,                    // Usage Page (Generic Desktop)
    0x09, 0x02,                    // Usage (Mouse)
    0xA1, 0x01,                    // Collection (Application)
    0x85, kAbsMouseReportId,       //   Report ID
    0x09, 0x01,                    //   Usage (Pointer)
    0xA1, 0x00,                    //   Collection (Physical)

    0x05, 0x09,                    //     Usage Page (Button)
    0x19, 0x01,                    //     Usage Minimum (Button 1)
    0x29, 0x05,                    //     Usage Maximum (Button 5)
    0x15, 0x00,                    //     Logical Minimum (0)
    0x25, 0x01,                    //     Logical Maximum (1)
    0x95, 0x05,                    //     Report Count (5)
    0x75, 0x01,                    //     Report Size (1)
    0x81, 0x02,                    //     Input (Data, Variable, Absolute)
    0x95, 0x01,                    //     Report Count (1)
    0x75, 0x03,                    //     Report Size (3)
    0x81, 0x01,                    //     Input (Constant) - pad to a byte

    0x05, 0x01,                    //     Usage Page (Generic Desktop)
    0x09, 0x30,                    //     Usage (X)
    0x09, 0x31,                    //     Usage (Y)
    0x16, 0x00, 0x00,              //     Logical Minimum (0)
    0x26, 0xFF, 0x7F,              //     Logical Maximum (32767)
    0x75, 0x10,                    //     Report Size (16)
    0x95, 0x02,                    //     Report Count (2)
    0x81, 0x02,                    //     Input (Data, Variable, ABSOLUTE)

    0x09, 0x38,                    //     Usage (Wheel)
    0x15, 0x81,                    //     Logical Minimum (-127)
    0x25, 0x7F,                    //     Logical Maximum (127)
    0x75, 0x08,                    //     Report Size (8)
    0x95, 0x01,                    //     Report Count (1)
    0x81, 0x06,                    //     Input (Data, Variable, Relative)

    0xC0,                          //   End Collection
    0xC0,                          // End Collection
};

struct __attribute__((packed)) AbsReport {
    uint8_t  buttons;
    uint16_t x;
    uint16_t y;
    int8_t   wheel;
};
static_assert(sizeof(AbsReport) == 6, "absolute mouse report must be 6 bytes");

}  // namespace

AbsoluteMouse::AbsoluteMouse() : hid_() {
    static bool registered = false;
    if (!registered) {
        registered = true;
        hid_.addDevice(this, sizeof(kReportDescriptor));
    }
}

uint16_t AbsoluteMouse::_onGetDescriptor(uint8_t *dst) {
    memcpy(dst, kReportDescriptor, sizeof(kReportDescriptor));
    return sizeof(kReportDescriptor);
}

void AbsoluteMouse::begin() {
    hid_.begin();
}

bool AbsoluteMouse::moveTo(uint16_t x, uint16_t y, uint8_t buttons) {
    if (x > kAbsoluteAxisMax) x = kAbsoluteAxisMax;
    if (y > kAbsoluteAxisMax) y = kAbsoluteAxisMax;
    AbsReport report = {buttons, x, y, 0};
    const bool ok = hid_.SendReport(kAbsMouseReportId, &report, sizeof(report));
    if (!ok) ++dropped_;
    return ok;
}

}  // namespace ghosthid
