// On-device status LCD (ESP32-S3-LCD-1.47 and any board that defines the pins).
//
// Optional and self-contained: the whole implementation is compiled out unless
// GHOSTHID_HAS_LCD is defined (it is only set in the S3 build env), so the S2
// build is untouched. main.cpp calls begin() once and showStatus() whenever the
// state it displays changes.
//
// The panel is a 172x320 ST7789 driven over SPI; we run it landscape (320x172).
// Pin numbers come from -DGHOSTHID_LCD_* in platformio.ini, matching the
// Waveshare wiring.

#pragma once

#include <stdint.h>

namespace ghosthid {

// A snapshot of everything the screen shows. Strings are borrowed for the
// duration of the call only.
struct DisplayStatus {
    const char *deviceName = "GhostHID";
    const char *apSsid     = "";
    const char *apPass     = "";        // for the join QR; never shown in clear unless asked
    const char *apIp       = "";        // e.g. "192.168.4.1"
    const char *staIp      = "";        // "" when the station link is down
    bool        usbReady   = false;     // target has enumerated us
    const char *kvmState   = "off";     // screen-client state text, or "off"
    int         clients    = 0;         // web controllers connected
};

class Display {
public:
    // Brings up SPI + the panel + backlight. Returns false (and is inert
    // thereafter) if the board has no LCD configured or init fails.
    bool begin();
    bool available() const { return begun_; }

    // Full redraw with the given state. Cheap enough to call on any change; the
    // caller decides when the state actually changed.
    void showStatus(const DisplayStatus &s);

    // Call from loop(): manages the backlight dim/off timeout.
    void loop();

    // Reset the backlight idle timer (e.g. on a client connect or button press).
    void noteActivity();

private:
    bool begun_ = false;

    // Draws a QR encoding `text` as filled modules, top-left at (x,y), each
    // module `scale` pixels. Returns the drawn pixel size, or 0 on failure.
    int  drawQr(int x, int y, int scale, const char *text);
};

}  // namespace ghosthid
