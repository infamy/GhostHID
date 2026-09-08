// Boot-time decision of whether the device exposes a USB serial console. See
// usb_serial.h for the TinyUSB constraint that forces this to happen here, at
// static-init time, rather than in setup().
//
// Two hardware findings shaped this:
//   - A CDC interface can only be added before USB.begin()/tinyusb_init().
//   - It must be added before the HID globals reserve their endpoints, or CDC's
//     endpoint reservation fails - so this runs at a high init_priority, ahead of
//     the default-priority USBHID objects, and NVS is initialised explicitly
//     because it is not yet up this early (a plain Preferences read fails and
//     would fall open to "serial present").
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

EarlyCdc g_earlyCdc __attribute__((init_priority(102)));   // after UsbSerial(101), before HID

}  // namespace
