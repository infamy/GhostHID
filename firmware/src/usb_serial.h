// Sealed-mode serial control.
//
// The device must be able to enumerate with NO USB serial (CDC) interface at all
// when sealed - "not on the bus" is less attack surface than "present but muted".
// arduino-esp32 can't remove CDC once ARDUINO_USB_CDC_ON_BOOT starts it before
// setup(), so this build sets CDC_ON_BOOT=0 and brings CDC up ourselves, only
// when unsealed, before USB.begin() finalises the descriptor.
//
// `UsbSerial` is that CDC object. This header is force-included into every
// translation unit of the S3 build (see platformio.ini `-include`) and remaps the
// `Serial` name to it, so all existing `Serial.print*` / console code drives the
// USB CDC without a source-wide rename. When we never call UsbSerial.begin()
// (sealed), writes are harmless no-ops and the interface is absent from the
// descriptor -> HID-only enumeration. The BOOT-hold unseal path calls begin() for
// the first time that boot, so the RX queue is created fresh and input works.
//
// Force-inclusion reaches C sources too (e.g. font tables), so everything here is
// gated on __cplusplus - the remap only matters to C++ console/logging code.

#pragma once

#ifdef __cplusplus

#include <Arduino.h>     // defines `Serial` (= UART0 when CDC_ON_BOOT=0); pulled first
#include <USBCDC.h>

extern USBCDC UsbSerial;

// Route the `Serial` name at the console/logging layer to our CDC object. Arduino.h
// above has already done its own `#define Serial Serial0`; override it here, after,
// so every later use in this build resolves to UsbSerial.
#ifdef Serial
#undef Serial
#endif
#define Serial UsbSerial

#endif  // __cplusplus
