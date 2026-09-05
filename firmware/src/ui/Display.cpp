#include "Display.h"

#ifdef GHOSTHID_HAS_LCD

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

extern "C" {
#include "qrcode.h"
}

namespace ghosthid {
namespace {

// Pins come from -DGHOSTHID_LCD_* (platformio.ini), matching the Waveshare
// ESP32-S3-LCD-1.47 wiring.
constexpr int8_t PIN_SCLK = GHOSTHID_LCD_SCLK;
constexpr int8_t PIN_MOSI = GHOSTHID_LCD_MOSI;
constexpr int8_t PIN_CS   = GHOSTHID_LCD_CS;
constexpr int8_t PIN_DC   = GHOSTHID_LCD_DC;
constexpr int8_t PIN_RST  = GHOSTHID_LCD_RST;
constexpr int8_t PIN_BL   = GHOSTHID_LCD_BL;

// Native panel is 172x320; rotation 3 gives a 320x172 landscape.
constexpr int16_t SCR_W = 320;
constexpr int16_t SCR_H = 172;

// RGB565 palette (a scope/logic-analyser look, matching the web UI's spirit).
constexpr uint16_t C_BG    = 0x0000;
constexpr uint16_t C_CYAN  = 0x07FF;
constexpr uint16_t C_GREEN = 0x07E8;
constexpr uint16_t C_RED   = 0xF800;
constexpr uint16_t C_AMBER = 0xFD20;
constexpr uint16_t C_GREY  = 0x8410;
constexpr uint16_t C_WHITE = 0xFFFF;

// Backlight PWM (8-bit duty) and idle timeouts.
constexpr uint8_t  BL_FULL   = 255;
constexpr uint8_t  BL_DIM    = 36;
constexpr uint32_t BL_DIM_MS = 2u * 60 * 1000;
constexpr uint32_t BL_OFF_MS = 10u * 60 * 1000;

Adafruit_ST7789 tft(&SPI, PIN_CS, PIN_DC, PIN_RST);

enum class Bl : uint8_t { On, Dim, Off };
Bl       g_bl = Bl::On;
uint32_t g_blActivity = 0;

void blSet(uint8_t duty) { ledcWrite(PIN_BL, duty); }

// One labelled status line: "label" in grey, "value" in `vc`.
void line(int16_t y, const char *label, const char *value, uint16_t vc) {
    tft.setTextSize(1);
    tft.setCursor(8, y);
    tft.setTextColor(C_GREY);
    tft.print(label);
    tft.setTextColor(vc);
    tft.print(value);
}

}  // namespace

bool Display::begin() {
    if (begun_) return true;

    ledcAttach(PIN_BL, 5000, 8);          // PWM so the backlight can dim (arduino 3.x API)
    blSet(0);                             // stay dark until the first frame is drawn

    SPI.begin(PIN_SCLK, -1, PIN_MOSI, PIN_CS);
    tft.init(172, 320);                   // native panel dimensions
    tft.setSPISpeed(40000000);
    tft.setRotation(3);                   // landscape, 320x172
    tft.fillScreen(C_BG);

    g_blActivity = millis();
    g_bl = Bl::On;
    blSet(BL_FULL);
    begun_ = true;
    return true;
}

int Display::drawQr(int x, int y, int scale, const char *text) {
    QRCode qr;
    // Version 4 (33x33 modules) at medium ECC holds ~62 bytes - comfortably more
    // than a "WIFI:S:...;P:...;;" join string. Buffer sized for that version.
    static uint8_t buf[256];
    if (qrcode_initText(&qr, buf, 4, ECC_MEDIUM, text) != 0) return 0;

    const int side = qr.size * scale;
    const int q = scale * 2;              // quiet zone
    // White field + black modules scans far more reliably than the inverse.
    tft.fillRect(x - q, y - q, side + 2 * q, side + 2 * q, C_WHITE);
    for (uint8_t j = 0; j < qr.size; ++j) {
        for (uint8_t i = 0; i < qr.size; ++i) {
            if (qrcode_getModule(&qr, i, j)) {
                tft.fillRect(x + i * scale, y + j * scale, scale, scale, C_BG);
            }
        }
    }
    return side;
}

void Display::showStatus(const DisplayStatus &s) {
    if (!begun_) return;
    noteActivity();
    tft.fillScreen(C_BG);

    // --- title -------------------------------------------------------------
    tft.setTextSize(2);
    tft.setTextColor(C_CYAN);
    tft.setCursor(8, 6);
    tft.print(s.deviceName && s.deviceName[0] ? s.deviceName : "GhostHID");
    tft.drawFastHLine(8, 26, 180, 0x2104);

    // --- left column status ------------------------------------------------
    int16_t y = 38;
    const int16_t dy = 17;
    line(y, "USB  ", s.usbReady ? "ready" : "not ready", s.usbReady ? C_GREEN : C_RED); y += dy;

    const bool kvmOff = !s.kvmState || strcmp(s.kvmState, "off") == 0;
    line(y, "KVM  ", kvmOff ? "off" : s.kvmState,
         kvmOff ? C_GREY : C_CYAN); y += dy;

    line(y, "STA  ", (s.staIp && s.staIp[0]) ? s.staIp : "(AP only)",
         (s.staIp && s.staIp[0]) ? C_WHITE : C_GREY); y += dy;

    line(y, "AP   ", (s.apIp && s.apIp[0]) ? s.apIp : "-", C_WHITE); y += dy;

    char c[24];
    snprintf(c, sizeof(c), "%d", s.clients);
    line(y, "ctrl ", c, s.clients > 0 ? C_GREEN : C_GREY); y += dy;

    // --- AP join QR (right) ------------------------------------------------
    // Standard Wi-Fi join payload; a phone camera joins the device's AP from it.
    if (s.apSsid && s.apSsid[0]) {
        char payload[96];
        snprintf(payload, sizeof(payload), "WIFI:S:%s;T:WPA;P:%s;;",
                 s.apSsid, s.apPass ? s.apPass : "");
        const int qx = 214, qy = 40, scale = 3;
        const int side = drawQr(qx, qy, scale, payload);
        if (side > 0) {
            tft.setTextSize(1);
            tft.setTextColor(C_GREY);
            tft.setCursor(qx - 4, qy + side + 8);
            tft.print("scan to join AP");
        }
    }
}

void Display::noteActivity() {
    g_blActivity = millis();
    if (g_bl != Bl::On) { g_bl = Bl::On; blSet(BL_FULL); }
}

void Display::loop() {
    if (!begun_) return;
    const uint32_t idle = millis() - g_blActivity;
    const Bl want = idle >= BL_OFF_MS ? Bl::Off
                  : idle >= BL_DIM_MS ? Bl::Dim
                                      : Bl::On;
    if (want != g_bl) {
        g_bl = want;
        blSet(want == Bl::On ? BL_FULL : want == Bl::Dim ? BL_DIM : 0);
    }
}

}  // namespace ghosthid

#endif  // GHOSTHID_HAS_LCD
