// GhostHID - board configuration / hardware abstraction
//
// Keep everything board-specific in this file. Porting GhostHID to another
// native-USB ESP32-S2/S3 board should mean editing this header and the
// `board` line in platformio.ini, nothing else.

#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

#ifndef GHOSTHID_VERSION
#define GHOSTHID_VERSION "0.6.16"
#endif

// How the target computer sees us in its USB device list.
//
// These live in platformio.ini as -DUSB_VID / -DUSB_PID / -DUSB_MANUFACTURER /
// -DUSB_PRODUCT, NOT here, because the core's USB.cpp reads those macros at
// compile time and the USB stack starts before setup() runs. Setting them at
// runtime does nothing. See PLAN.md "Known Gaps" for the VID/PID question.

// ---------------------------------------------------------------------------
// Board peripherals (all optional - set to -1 if the board lacks one)
// ---------------------------------------------------------------------------

#ifndef GHOSTHID_PIN_LED
#define GHOSTHID_PIN_LED 15    // status LED; -1 to disable
#endif

#ifndef GHOSTHID_PIN_LED_ACTIVE_LOW
#define GHOSTHID_PIN_LED_ACTIVE_LOW 0
#endif

// BOOT button is GPIO0 on essentially every ESP32-S2/S3 board.
#ifndef GHOSTHID_PIN_BUTTON
#define GHOSTHID_PIN_BUTTON 0  // -1 to disable
#endif

#ifndef GHOSTHID_PIN_BUTTON_ACTIVE_LOW
#define GHOSTHID_PIN_BUTTON_ACTIVE_LOW 1
#endif

// ---------------------------------------------------------------------------
// HID behaviour
// ---------------------------------------------------------------------------

// A single USB HID relative-mouse report carries a signed 8-bit delta, so any
// larger movement must be split across several reports. See HidDevice::mouseMove.
#define GHOSTHID_MOUSE_MAX_STEP 127

// Upper bound on a single mouse_move / mouse_wheel command, clamped at the
// protocol boundary. HidDevice splits a large delta into 127-px reports, each
// of which blocks on the USB completion semaphore; an unclamped int32 (the API
// used to accept the full range) is millions of blocking reports inside one
// network callback, which trips the 5s task watchdog and reboots the device.
// A move larger than a couple of desktops is never a real input event.
#ifndef GHOSTHID_MOUSE_MAX_MOVE
#define GHOSTHID_MOUSE_MAX_MOVE 8192
#endif
#ifndef GHOSTHID_WHEEL_MAX
#define GHOSTHID_WHEEL_MAX 64
#endif

// Upper bound on a single `text` command, in characters. Each character is a
// press+release (two blocking reports); an unbounded string in one frame is the
// same watchdog hazard as an unclamped mouse move.
#ifndef GHOSTHID_TEXT_MAX
#define GHOSTHID_TEXT_MAX 256
#endif

// Extra delay between consecutive HID reports (ms).
//
// Zero by default, and that is not an optimisation gamble: USBHID::SendReport
// already blocks on a semaphore released by tud_hid_report_complete_cb, so the
// USB stack provides its own backpressure and will not let us outrun the host.
// The delay this replaced cost 2ms on every single report - per keystroke, per
// mouse chunk - for no benefit.
//
// Raise it only if a specific target proves to drop back-to-back reports; some
// BIOS/UEFI implementations and KVM switches reportedly do.
#ifndef GHOSTHID_HID_REPORT_GAP_MS
#define GHOSTHID_HID_REPORT_GAP_MS 0
#endif

// How long to wait after USB enumeration before the host has loaded its HID
// driver and will actually accept input.
#ifndef GHOSTHID_HID_SETTLE_MS
#define GHOSTHID_HID_SETTLE_MS 1000
#endif

// ---------------------------------------------------------------------------
// Networking (Phase 2)
// ---------------------------------------------------------------------------

#ifndef GHOSTHID_AP_SSID_PREFIX
#define GHOSTHID_AP_SSID_PREFIX "GhostHID"
#endif

// WPA2 passphrase for the device's own access point. MUST be >= 8 characters.
// Change this for anything beyond bench use - it is the only thing stopping a
// passer-by from associating and reaching the WebSocket.
#ifndef GHOSTHID_AP_PASSWORD
#define GHOSTHID_AP_PASSWORD "ghosthid-setup"
#endif

// Application-layer pairing token. Empty string disables the auth step.
#ifndef GHOSTHID_AUTH_TOKEN
#define GHOSTHID_AUTH_TOKEN "ghosthid"
#endif

#ifndef GHOSTHID_MDNS_NAME
#define GHOSTHID_MDNS_NAME "ghosthid"
#endif

// If the controller holds input and then goes quiet for this long, release
// everything. This is the backstop for abrupt link loss (power cut, out of
// range) where no disconnect event ever arrives; a clean WebSocket close
// releases immediately and never reaches this timer.
//
// Deliberately far below the 2s in the original plan: a modifier stuck for two
// seconds has already autorepeated or fired a chord on the target.
#ifndef GHOSTHID_HEARTBEAT_TIMEOUT_MS
#define GHOSTHID_HEARTBEAT_TIMEOUT_MS 750
#endif

// The same backstop for the screen-client (Deskflow) path, which cannot use the
// 750ms figure: there is no per-key heartbeat, only the server's ~1s keep-alive
// traffic, so the shortest silence we can call "dead" without false-firing
// between keep-alives is a couple of intervals. If input is held and no server
// traffic (keep-alive included) arrives for this long, release everything.
// Still far below the 15s keep-alive timeout that governs a plain disconnect.
#ifndef GHOSTHID_KVM_HELD_TIMEOUT_MS
#define GHOSTHID_KVM_HELD_TIMEOUT_MS 2500
#endif

// Station mode (PLAN.md Mode B). If GHOSTHID_STA_SSID is a non-empty string,
// GhostHID ALSO joins that network, while keeping its own AP up.
//
// Both run at once on purpose. Station mode is the convenient path (your
// controller machine stays on its normal network), but it depends on
// infrastructure that may not exist where this device gets used - an air-gapped
// lab, a machine sitting at a BIOS prompt. The AP is the guarantee that the
// device is always reachable; STA is the convenience layered on top.
#ifndef GHOSTHID_STA_SSID
#define GHOSTHID_STA_SSID ""
#endif

#ifndef GHOSTHID_STA_PASSWORD
#define GHOSTHID_STA_PASSWORD ""
#endif

// How long to wait for the station association before carrying on. The AP is
// already up by then, so a failure here is not fatal.
#ifndef GHOSTHID_STA_TIMEOUT_MS
#define GHOSTHID_STA_TIMEOUT_MS 8000
#endif
