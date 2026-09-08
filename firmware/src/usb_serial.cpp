// Boot-time decision of whether the device exposes a USB serial console. See
// usb_serial.h for the TinyUSB constraint that forces this to happen here, at
// static-init time, rather than in setup().
//
// The one real constraint (confirmed against arduino-esp32 3.3.x + ESP-IDF 5.5):
// a USB interface can only be added BEFORE USB.begin()/tinyusb_init()
// (tinyusb_enable_interface() silently fails once USB has started). Constructing
// the USBCDC at static init satisfies that. Endpoint numbers are fixed and
// disjoint (HID keyboard = EP1 in/out; CDC = EP3 out, EP4/EP5 in), so order
// relative to the HID globals does NOT matter - the init_priority below only
// orders UsbSerial before EarlyCdc, which attaches to it.
//
// NVS is initialised explicitly here because at static-init time the core has not
// done so yet: a plain Preferences read returns the default and would fall open to
// "serial present" on a sealed device.
//
// Not part of the native test build (excluded by that env's build_src_filter).
#include "usb_serial.h"

#include <Preferences.h>
#include <nvs_flash.h>

// init_priority: construct before the default-priority USBHID globals so, when we
// do construct a USBCDC, its endpoints are reserved first.
SealAwareSerial UsbSerial __attribute__((init_priority(101)));

namespace {

struct EarlyCdc {
    EarlyCdc() {
        // NVS is not initialised this early; do it ourselves so the read is real
        // rather than silently defaulting (which would expose serial on a sealed
        // device). Idempotent - the app's later nvs_flash_init()/Preferences reuse
        // it. Recover the partition if it needs an erase.
        esp_err_t err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            nvs_flash_erase();
            nvs_flash_init();
        }
        bool sealed = false, window = false;
        Preferences p;
        if (p.begin("ghosthid", /*readOnly=*/true)) {
            sealed = p.getBool("sealed", false);
            window = p.getBool("unseal_win", false);
            p.end();
        }
        // Register a CDC (construct the USBCDC) only when serial should exist this
        // boot: unsealed, or a one-shot unseal window. Sealed with no window ->
        // no USBCDC -> HID-only enumeration, no port on the bus.
        if (!sealed || window) {
            UsbSerial.attach(new USBCDC(0));
        }
    }
};

EarlyCdc g_earlyCdc __attribute__((init_priority(102)));   // after UsbSerial(101)

}  // namespace
