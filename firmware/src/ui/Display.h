// On-device status LCD + RGB status LED (ESP32-S3-LCD-1.47, or any board that
// defines the pins). Entirely compiled out unless GHOSTHID_HAS_LCD is set (S3
// build only), so the S2 build is untouched.
//
// The panel is a 172x320 ST7789 over SPI, run landscape (320x172). Pins come
// from -DGHOSTHID_LCD_* / -DGHOSTHID_PIN_RGB in platformio.ini.
//
// Usage: begin() once; update(status) every loop() pass (it rate-limits and
// only redraws on change); nextPage() on a BOOT short-press to cycle pages.

#pragma once

#include <stdint.h>

namespace ghosthid {

struct DisplayStatus {
    const char *deviceName  = "GhostHID";
    const char *apSsid      = "";
    const char *apPass      = "";        // for the join QR
    const char *token       = "";        // pairing token, shown on the Wi-Fi page
    const char *apIp        = "";
    const char *staIp       = "";        // "" when the station link is down
    bool        usbReady    = false;
    const char *kvmState    = "off";     // "off" / "connecting" / "connected"
    bool        kvmFocus    = false;     // this screen currently holds the pointer
    int         clients     = 0;         // web controllers connected
    const char *version     = "";
    uint32_t    heapFreeKb  = 0;
    uint32_t    uptimeSec   = 0;
    int         rssi        = 0;        // station signal, dBm; 0 when not joined
    int         channel     = 0;
    bool        capsLock    = false;    // host lock-LED state (feedback from target)
    bool        numLock     = false;
    bool        scrollLock  = false;
    bool        sealed      = false;    // sealed mode active - overrides every page
    bool        unsealArmed = false;    // BOOT held: serial re-enabled for unsealing
};

class Display {
public:
    enum class Page : uint8_t { Status, Qr, Token, Info, COUNT };

    bool begin();
    bool available() const { return begun_; }

    // Call every loop() pass. Cheap: rate-limits the RGB LED, manages the
    // backlight timeout, and redraws the LCD only when the shown state (or the
    // page) actually changed.
    void update(const DisplayStatus &s);

    // Advance to the next page and force a redraw. Also counts as activity
    // (wakes the backlight).
    void nextPage();

    void noteActivity();

private:
    bool begun_ = false;
    Page page_  = Page::Status;
    bool dirty_ = true;                   // force a redraw on the next update()

    void render(const DisplayStatus &s);  // full redraw of the current page
    void drawStatusPage(const DisplayStatus &s);
    void drawQrPage(const DisplayStatus &s);
    void drawTokenPage(const DisplayStatus &s);
    void drawInfoPage(const DisplayStatus &s);
    void drawSealedPage(const DisplayStatus &s);  // shown whenever s.sealed
    int  drawQr(int x, int y, int scale, const char *text);  // returns pixel side, 0 on fail
    void updateLed(const DisplayStatus &s);
};

}  // namespace ghosthid
