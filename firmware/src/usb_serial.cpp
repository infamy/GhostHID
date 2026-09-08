// The one definition of the sealed-mode USB CDC console object. See usb_serial.h.
//
// Not part of the native test build (excluded by that env's build_src_filter).
// itf 0 matches the single-CDC layout arduino-esp32 uses; begun conditionally in
// main()'s setup() (unsealed boots) or by the BOOT-hold unseal gesture.
#include <USBCDC.h>

USBCDC UsbSerial(0);
