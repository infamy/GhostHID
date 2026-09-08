// The one definition of the sealed-mode serial facade. See usb_serial.h.
//
// Not part of the native test build (excluded by that env's build_src_filter).
// The underlying USBCDC is created lazily by begin(); constructing it is what
// adds the CDC interface to the USB descriptor, so a sealed boot - which never
// calls begin() - stays HID-only.
#include "usb_serial.h"

SealAwareSerial UsbSerial;
