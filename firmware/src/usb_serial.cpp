// Boot-time decision of whether the device exposes a USB serial console. See
// usb_serial.h for the TinyUSB constraint that forces this to happen here, at
// static-init time, rather than in setup().
//
// Not part of the native test build (excluded by that env's build_src_filter).
#include "usb_serial.h"

#include <Preferences.h>

SealAwareSerial UsbSerial;

namespace {

// Runs during C++ static initialisation - before setup(), and therefore before
// USB.begin()/tinyusb_init(), the only window in which a CDC interface can be
// added to the descriptor. Construct (and thereby register) the CDC only when the
// device should expose serial this boot:
//   - not sealed, OR
//   - a one-shot "unseal window": a ~5s BOOT hold on a sealed device set this flag
//     and rebooted, precisely so the CDC can come up on this next boot.
// Sealed with no window -> no USBCDC -> HID-only enumeration, no port on the bus.
//
// If NVS can't be read this early, the safe default is to expose serial (fail
// open to a console, never to a wide-open sealed device: the console stays token-
// gated and, on a truly sealed unit, config.sealed() still gates every command).
struct EarlyCdc {
    EarlyCdc() {
        bool sealed = false, window = false;
        Preferences p;
        if (p.begin("ghosthid", /*readOnly=*/true)) {
            sealed = p.getBool("sealed", false);
            window = p.getBool("unseal_win", false);
            p.end();
        }
        if (!sealed || window) {
            UsbSerial.attach(new USBCDC(0));
        }
    }
};

EarlyCdc g_earlyCdc;   // constructed after UsbSerial (same TU, declared earlier)

}  // namespace
