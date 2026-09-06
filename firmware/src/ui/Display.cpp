#include "Display.h"

#ifdef GHOSTHID_HAS_LCD

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#ifdef GHOSTHID_PIN_RGB
#include <Adafruit_NeoPixel.h>
#endif

extern "C" {
#include "qrcode.h"
}

namespace ghosthid {
namespace {

constexpr int8_t PIN_SCLK = GHOSTHID_LCD_SCLK;
constexpr int8_t PIN_MOSI = GHOSTHID_LCD_MOSI;
constexpr int8_t PIN_CS   = GHOSTHID_LCD_CS;
constexpr int8_t PIN_DC   = GHOSTHID_LCD_DC;
constexpr int8_t PIN_RST  = GHOSTHID_LCD_RST;
constexpr int8_t PIN_BL   = GHOSTHID_LCD_BL;

constexpr int16_t SCR_W = 320;
constexpr int16_t SCR_H = 172;
// The panel's corners are physically rounded, so content shoved pixel-tight into
// a corner loses a few pixels. Keep corner text/markers this far off the edges.
constexpr int16_t MARGIN = 18;

constexpr uint16_t C_BG    = 0x0000;
constexpr uint16_t C_CYAN  = 0x07FF;
constexpr uint16_t C_GREEN = 0x07E8;
constexpr uint16_t C_RED   = 0xF800;
constexpr uint16_t C_AMBER = 0xFD20;
constexpr uint16_t C_GREY  = 0x8410;
constexpr uint16_t C_WHITE = 0xFFFF;
constexpr uint16_t C_LINE  = 0x2104;

constexpr uint8_t  BL_FULL   = 255;
constexpr uint8_t  BL_DIM    = 36;
constexpr uint32_t BL_DIM_MS = 2u * 60 * 1000;
constexpr uint32_t BL_OFF_MS = 10u * 60 * 1000;
// Revert to the status/brand page after this long on any other page, so the
// Wi-Fi page (AP password + pairing token in clear) is never left up for
// someone who glances at it and walks away.
constexpr uint32_t PAGE_REVERT_MS = 60u * 1000;

Adafruit_ST7789 tft(&SPI, PIN_CS, PIN_DC, PIN_RST);
#ifdef GHOSTHID_PIN_RGB
Adafruit_NeoPixel rgb(1, GHOSTHID_PIN_RGB, NEO_GRB + NEO_KHZ800);
#endif

enum class Bl : uint8_t { On, Dim, Off };
Bl       g_bl = Bl::On;
uint32_t g_blActivity = 0;

void blSet(uint8_t duty) { ledcWrite(PIN_BL, duty); }

// One labelled line at the given text size (2 = 12x16 px, readable on a 1.47").
void line(int16_t y, const char *label, const char *value, uint16_t vc, uint8_t size = 2) {
    tft.setTextSize(size);
    tft.setCursor(MARGIN, y);
    tft.setTextColor(C_GREY);
    tft.print(label);
    tft.setTextColor(vc);
    tft.print(value);
}

// The GhostHID mark: a rounded dome, scalloped "feet", two eyes - the favicon,
// drawn with primitives so there's no bitmap to embed. (x,y) is the top-left.
void drawGhost(int x, int y, int w, int h, uint16_t col, uint16_t bg) {
    const int r = w / 2;
    tft.fillRoundRect(x, y, w, h, r, col);   // rounded top...
    tft.fillRect(x, y + h - r, w, r, col);   // ...square lower body
    // Scalloped bottom: carve background semicircles for the ghost's feet.
    const int n = 3, sw = w / n;
    for (int i = 0; i < n; ++i)
        tft.fillCircle(x + sw / 2 + i * sw, y + h, sw / 2 + 1, bg);
    // Eyes.
    const int ew = w / 6, eh = h / 4, ey = y + h / 3;
    tft.fillRect(x + w / 3 - ew / 2,     ey, ew, eh, bg);
    tft.fillRect(x + 2 * w / 3 - ew / 2, ey, ew, eh, bg);
}

// A small dot marking the physical BOOT button (bottom-right, where it sits) so
// its "press to cycle pages" role is discoverable.
void buttonHint(const char *what) {
    tft.fillCircle(SCR_W - MARGIN, SCR_H - MARGIN, 3, C_CYAN);
    tft.setTextSize(1);
    tft.setTextColor(C_GREY);
    int16_t w = (int16_t)strlen(what) * 6;
    tft.setCursor(SCR_W - MARGIN - 8 - w, SCR_H - MARGIN - 3);
    tft.print(what);
}

}  // namespace

bool Display::begin() {
    if (begun_) return true;

    ledcAttach(PIN_BL, 5000, 8);
    blSet(0);

    SPI.begin(PIN_SCLK, -1, PIN_MOSI, PIN_CS);
    tft.init(172, 320);
    tft.setSPISpeed(40000000);
    tft.setRotation(3);
    tft.fillScreen(C_BG);

#ifdef GHOSTHID_PIN_RGB
    rgb.begin();
    rgb.setBrightness(60);
    rgb.clear();
    rgb.show();
#endif

    g_blActivity = millis();
    g_bl = Bl::On;
    blSet(BL_FULL);
    begun_ = true;
    dirty_ = true;
    return true;
}

int Display::drawQr(int x, int y, int scale, const char *text) {
    QRCode qr;
    static uint8_t buf[256];              // >= qrcode_getBufferSize(4)
    if (qrcode_initText(&qr, buf, 4, ECC_MEDIUM, text) != 0) return 0;

    const int side = qr.size * scale;
    const int q = scale * 2;              // quiet zone
    tft.fillRect(x - q, y - q, side + 2 * q, side + 2 * q, C_WHITE);
    for (uint8_t j = 0; j < qr.size; ++j)
        for (uint8_t i = 0; i < qr.size; ++i)
            if (qrcode_getModule(&qr, i, j))
                tft.fillRect(x + i * scale, y + j * scale, scale, scale, C_BG);
    return side;
}

// Small text pinned to a corner (size 1). rightAlign anchors x at the right edge.
void corner(int16_t x, int16_t y, const char *s, uint16_t col, bool rightAlign) {
    tft.setTextSize(1);
    tft.setTextColor(col);
    if (rightAlign) x -= (int16_t)strlen(s) * 6;
    tft.setCursor(x, y);
    tft.print(s);
}

void Display::drawStatusPage(const DisplayStatus &s) {
    // The brand is the point: a big white ghost + wordmark, centred, with just a
    // little status in the corners. The AP-join QR is on page 2 (BOOT to cycle).
    // Centred vertically between the corner status rows; ghost in the logo's
    // cyan (matches the favicon), wordmark two-tone below.
    const int gw = 60, gh = 66, gx = (SCR_W - gw) / 2, gy = 34;
    drawGhost(gx, gy, gw, gh, C_CYAN, C_BG);

    // Wordmark, two-tone, centred under the ghost: "Ghost" white, "HID" cyan.
    tft.setTextSize(3);
    const char *a = "Ghost", *b = "HID";
    const int wa = (int)strlen(a) * 18, wb = (int)strlen(b) * 18;
    const int wx = (SCR_W - (wa + wb)) / 2, wy = gy + gh + 10;
    tft.setTextColor(C_WHITE); tft.setCursor(wx, wy);       tft.print(a);
    tft.setTextColor(C_CYAN);  tft.setCursor(wx + wa, wy);  tft.print(b);

    // Host lock-LED state, centred under the wordmark: lit green when the target
    // has the lock on, dim grey otherwise. This is the target talking back.
    tft.setTextSize(1);
    int lx = (SCR_W - 86) / 2;
    const int ly = wy + 30;
    tft.setTextColor(s.capsLock   ? C_GREEN : C_GREY); tft.setCursor(lx, ly); tft.print("CAPS"); lx += 34;
    tft.setTextColor(s.numLock    ? C_GREEN : C_GREY); tft.setCursor(lx, ly); tft.print("NUM");  lx += 28;
    tft.setTextColor(s.scrollLock ? C_GREEN : C_GREY); tft.setCursor(lx, ly); tft.print("SCRL");

    // --- corners: small operational status ---------------------------------
    corner(MARGIN, MARGIN, s.usbReady ? "USB ok" : "USB --", s.usbReady ? C_GREEN : C_RED, false);

    const bool kvmOff  = !s.kvmState || strcmp(s.kvmState, "off") == 0;
    const bool kvmConn = s.kvmState && strcmp(s.kvmState, "connected") == 0;
    corner(SCR_W - MARGIN, MARGIN,
           kvmOff ? "off" : (kvmConn ? (s.kvmFocus ? "active" : "connected") : "connecting"),
           kvmOff ? C_GREY : (kvmConn ? C_GREEN : C_AMBER), true);

    corner(MARGIN, SCR_H - MARGIN - 7, (s.staIp && s.staIp[0]) ? s.staIp : "AP only",
           (s.staIp && s.staIp[0]) ? C_CYAN : C_GREY, false);

    char c[16];
    snprintf(c, sizeof(c), "%d ctrl", s.clients);
    corner(SCR_W - MARGIN, SCR_H - MARGIN - 7, c, s.clients > 0 ? C_GREEN : C_GREY, true);
}

void Display::drawQrPage(const DisplayStatus &s) {
    tft.setTextSize(2);
    tft.setTextColor(C_CYAN);
    tft.setCursor(MARGIN, 8);
    tft.print("Join Wi-Fi");

    if (s.apSsid && s.apSsid[0]) {
        char payload[96];
        snprintf(payload, sizeof(payload), "WIFI:S:%s;T:WPA;P:%s;;",
                 s.apSsid, s.apPass ? s.apPass : "");
        // Version 4 (33 modules) x scale 4 = 132px on the left.
        drawQr(MARGIN, 36, 4, payload);
    }
    // SSID + AP password in clear on the right, for manual entry. The pairing
    // token has its own page.
    tft.setTextSize(2);
    tft.setTextColor(C_GREY);  tft.setCursor(150, 44);  tft.print("SSID");
    tft.setTextColor(C_WHITE); tft.setCursor(150, 62);  tft.print(s.apSsid ? s.apSsid : "");
    tft.setTextColor(C_GREY);  tft.setCursor(150, 100); tft.print("PASS");
    tft.setTextColor(C_WHITE); tft.setCursor(150, 118); tft.print(s.apPass ? s.apPass : "");
    buttonHint("page");
}

// Groups a token into 4-char blocks ("ABCD EFGH"). Display only - the stored
// token has no spaces and the web UI strips whitespace on entry.
static void groupToken(const char *tok, char *out, size_t cap) {
    size_t j = 0;
    for (size_t i = 0; tok[i] && j < cap - 2; ++i) {
        if (i && (i % 4) == 0) out[j++] = ' ';
        out[j++] = tok[i];
    }
    out[j] = '\0';
}

void Display::drawTokenPage(const DisplayStatus &s) {
    tft.setTextSize(2);
    tft.setTextColor(C_CYAN);
    tft.setCursor(MARGIN, 8);
    tft.print("Pairing token");
    tft.drawFastHLine(MARGIN, 34, SCR_W - 2 * MARGIN, C_LINE);

    if (s.token && s.token[0]) {
        char grp[80];
        groupToken(s.token, grp, sizeof(grp));
        const int len = (int)strlen(grp);
        // Pick the biggest font that still fits across the panel.
        const int avail = SCR_W - 2 * MARGIN;
        uint8_t sz = 4;                                   // 24px/char
        if (len * 24 > avail) sz = 3;                     // 18px/char
        if (len * 18 > avail) sz = 2;                     // 12px/char
        const int cw = sz == 4 ? 24 : sz == 3 ? 18 : 12;
        int x = (SCR_W - len * cw) / 2;
        if (x < MARGIN) x = MARGIN;
        tft.setTextSize(sz);
        tft.setTextColor(C_WHITE);
        tft.setCursor(x, 84);
        tft.print(grp);
    } else {
        tft.setTextSize(2);
        tft.setTextColor(C_GREY);
        tft.setCursor(MARGIN, 90);
        tft.print("(auth disabled)");
    }

    tft.setTextSize(1);
    tft.setTextColor(C_GREY);
    tft.setCursor(MARGIN, SCR_H - MARGIN - 7);
    tft.print("enter this in the web UI");
    buttonHint("page");
}

void Display::drawInfoPage(const DisplayStatus &s) {
    tft.setTextSize(3);
    tft.setTextColor(C_CYAN);
    tft.setCursor(MARGIN, 8);
    tft.print("Info");
    tft.drawFastHLine(MARGIN, 36, SCR_W - 2 * MARGIN, C_LINE);

    int16_t y = 46;
    const int16_t dy = 25;
    line(y, "ver  ", s.version && s.version[0] ? s.version : "?", C_WHITE); y += dy;

    char b[28];
    snprintf(b, sizeof(b), "%uK", (unsigned)s.heapFreeKb);
    line(y, "heap ", b, s.heapFreeKb < 20 ? C_AMBER : C_WHITE); y += dy;

    const uint32_t up = s.uptimeSec;
    snprintf(b, sizeof(b), "%uh%02um", (unsigned)(up / 3600), (unsigned)((up % 3600) / 60));
    line(y, "up   ", b, C_WHITE); y += dy;

    line(y, "ip   ", (s.staIp && s.staIp[0]) ? s.staIp : "AP only", C_WHITE);
    buttonHint("page");
}

void Display::render(const DisplayStatus &s) {
    tft.fillScreen(C_BG);
    switch (page_) {
        case Page::Status: drawStatusPage(s); break;
        case Page::Qr:     drawQrPage(s);     break;
        case Page::Token:  drawTokenPage(s);  break;
        case Page::Info:   drawInfoPage(s);   break;
        default:           drawStatusPage(s); break;
    }
}

void Display::updateLed(const DisplayStatus &s) {
#ifdef GHOSTHID_PIN_RGB
    // Priority: a target that has not enumerated us is the loudest (red); then
    // an active screen session (green with focus, cyan connected); then a web
    // controller (cyan); then idle (dim - blue on a network, magenta AP-only).
    uint32_t c;
    const bool kvmConn = s.kvmState && strcmp(s.kvmState, "connected") == 0;
    if (!s.usbReady)              c = rgb.Color(120, 0, 0);
    else if (kvmConn)            c = rgb.Color(0, 120, s.kvmFocus ? 40 : 90);
    else if (s.clients > 0)      c = rgb.Color(0, 60, 90);
    else if (s.staIp && s.staIp[0]) c = rgb.Color(0, 10, 24);
    else                         c = rgb.Color(24, 0, 20);

    static uint32_t last = 0xFFFFFFFF;
    if (c != last) { last = c; rgb.setPixelColor(0, c); rgb.show(); }
#else
    (void)s;
#endif
}

void Display::nextPage() {
    page_ = static_cast<Page>((static_cast<uint8_t>(page_) + 1) %
                              static_cast<uint8_t>(Page::COUNT));
    dirty_ = true;
    noteActivity();
}

void Display::noteActivity() {
    g_blActivity = millis();
    if (g_bl != Bl::On) { g_bl = Bl::On; blSet(BL_FULL); }
}

void Display::update(const DisplayStatus &s) {
    if (!begun_) return;

    // Auto-revert to the status page (BOOT cycling is the only activity, so
    // g_blActivity is the last page change). Keeps the token off screen when
    // left unattended.
    if (page_ != Page::Status && millis() - g_blActivity > PAGE_REVERT_MS) {
        page_ = Page::Status;
        dirty_ = true;
    }

    // Backlight timeout.
    const uint32_t idle = millis() - g_blActivity;
    const Bl want = idle >= BL_OFF_MS ? Bl::Off : idle >= BL_DIM_MS ? Bl::Dim : Bl::On;
    if (want != g_bl) {
        g_bl = want;
        blSet(want == Bl::On ? BL_FULL : want == Bl::Dim ? BL_DIM : 0);
    }

    // RGB LED, rate-limited to ~5Hz (it change-detects internally too).
    static uint32_t ledAt = 0;
    if (millis() - ledAt > 200) { ledAt = millis(); updateLed(s); }

    // LCD: redraw only when the shown state or the page changed. The signature
    // deliberately excludes uptime/heap on the status/QR pages (they'd force a
    // constant redraw); the Info page opts them in via a coarse bucket.
    static uint32_t drawAt = 0;
    if (millis() - drawAt < 500 && !dirty_) return;
    drawAt = millis();

    char sig[224];
    snprintf(sig, sizeof(sig), "%d|%s|%s|%s|%s|%d|%s|%d|%d|%s|%d%d%d|%u",
             (int)page_, s.deviceName, s.apSsid, s.apIp, s.staIp,
             s.usbReady ? 1 : 0, s.kvmState, s.kvmFocus ? 1 : 0, s.clients,
             s.version,
             s.capsLock ? 1 : 0, s.numLock ? 1 : 0, s.scrollLock ? 1 : 0,
             page_ == Page::Info ? (unsigned)(s.uptimeSec / 30) : 0u);  // Info: refresh ~2x/min
    static char lastSig[224] = {0};
    if (!dirty_ && strcmp(sig, lastSig) == 0) return;

    strncpy(lastSig, sig, sizeof(lastSig) - 1);
    dirty_ = false;
    render(s);
}

}  // namespace ghosthid

#endif  // GHOSTHID_HAS_LCD
