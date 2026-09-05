#include "DeskflowClient.h"

#include <Arduino.h>
#include <string.h>

#include "board_config.h"
#include "config/Config.h"
#include "hid/HidDevice.h"

namespace ghosthid {
namespace {

constexpr int16_t kProtocolMajor = 1;
constexpr int16_t kProtocolMinor = 8;
constexpr size_t  kMaxMessage    = 512;   // input messages are tiny; clipboard is ignored

// Big-endian field helpers. The protocol is entirely network byte order.
inline uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
inline int16_t  rdS16(const uint8_t *p) { return (int16_t)rd16(p); }
inline uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
inline void wr16(uint8_t *p, int16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
inline void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
inline bool is(const uint8_t *m, size_t len, const char *code) {
    return len >= 4 && memcmp(m, code, 4) == 0;
}

// --- key mapping -----------------------------------------------------------
//
// KeyIDs are X11 keysyms with the 0xFF00 page moved to 0xEF00. Printable ASCII
// maps to itself, which our HID layer already accepts directly; everything else
// needs this table.
struct KeyMap { uint16_t keyId; uint8_t hid; };

const KeyMap kSpecialKeys[] = {
    {0xEF08, key::Backspace}, {0xEF09, key::Tab},       {0xEF0D, key::Return},
    {0xEF1B, key::Escape},    {0xEFFF, key::Delete},    {0xEF50, key::Home},
    {0xEF51, key::LeftArrow}, {0xEF52, key::UpArrow},   {0xEF53, key::RightArrow},
    {0xEF54, key::DownArrow}, {0xEF55, key::PageUp},    {0xEF56, key::PageDown},
    {0xEF57, key::End},       {0xEF63, key::Insert},    {0xEFE5, key::CapsLock},
    {0xEF8D, key::Return},    {0xEF80, key::Space},     {0xEF89, key::Tab},

    {0xEFE1, key::LeftShift}, {0xEFE2, key::RightShift},
    {0xEFE3, key::LeftCtrl},  {0xEFE4, key::RightCtrl},
    {0xEFE9, key::LeftAlt},   {0xEFEA, key::RightAlt},
    {0xEF7E, key::RightAlt},                            // AltGr
    // Meta and Super both land on GUI: a keyboard has one such key per side,
    // and the target decides what it means.
    {0xEFE7, key::LeftGui},   {0xEFE8, key::RightGui},
    {0xEFEB, key::LeftGui},   {0xEFEC, key::RightGui},

    {0xEFBE, key::F1},  {0xEFBF, key::F2},  {0xEFC0, key::F3},  {0xEFC1, key::F4},
    {0xEFC2, key::F5},  {0xEFC3, key::F6},  {0xEFC4, key::F7},  {0xEFC5, key::F8},
    {0xEFC6, key::F9},  {0xEFC7, key::F10}, {0xEFC8, key::F11}, {0xEFC9, key::F12},
};

// Returns 0 when the key has no USB HID equivalent worth sending.
uint8_t keyIdToHid(uint16_t keyId) {
    if (keyId >= 0x20 && keyId <= 0x7E) return (uint8_t)keyId;   // printable ASCII
    for (const KeyMap &k : kSpecialKeys) {
        if (k.keyId == keyId) return k.hid;
    }
    return 0;
}

}  // namespace

const char *DeskflowClient::statusText() const {
    switch (state_) {
        case State::Idle:        return lastError_[0] ? lastError_ : "not connected";
        case State::Connecting:  return "connecting";
        case State::Handshaking: return "handshaking";
        case State::Active:      return hasFocus_ ? "connected (this screen has focus)"
                                                  : "connected (idle)";
    }
    return "?";
}

// --- framing ---------------------------------------------------------------

bool DeskflowClient::readExactly(uint8_t *dst, size_t len, uint32_t timeoutMs) {
    const uint32_t start = millis();
    size_t got = 0;
    while (got < len) {
        if (!sock_.connected()) return false;
        const int n = sock_.read(dst + got, len - got);
        if (n > 0) { got += n; continue; }
        if (millis() - start > timeoutMs) return false;
        delay(1);
    }
    return true;
}

bool DeskflowClient::readMessage(uint8_t *buf, size_t cap, size_t &outLen) {
    if (sock_.available() < 4) return false;

    uint8_t hdr[4];
    if (!readExactly(hdr, 4, 500)) { disconnect("truncated length"); return false; }
    const uint32_t len = rd32(hdr);

    if (len == 0 || len > 64 * 1024) { disconnect("implausible message length"); return false; }

    if (len > cap) {
        // Almost certainly a clipboard transfer. We have no clipboard to offer,
        // so drain it rather than dropping the connection over it.
        uint8_t sink[64];
        uint32_t left = len;
        while (left > 0) {
            const size_t chunk = left < sizeof(sink) ? left : sizeof(sink);
            if (!readExactly(sink, chunk, 2000)) { disconnect("drain failed"); return false; }
            left -= chunk;
        }
        outLen = 0;
        return true;
    }

    if (!readExactly(buf, len, 2000)) { disconnect("truncated body"); return false; }
    outLen = len;
    return true;
}

void DeskflowClient::sendMessage(const uint8_t *payload, size_t len) {
    uint8_t hdr[4];
    wr32(hdr, (uint32_t)len);
    sock_.write(hdr, 4);
    sock_.write(payload, len);
    sock_.flush();
}

void DeskflowClient::sendCode(const char *code) {
    sendMessage(reinterpret_cast<const uint8_t *>(code), 4);
}

// --- handshake -------------------------------------------------------------

void DeskflowClient::handshake(const uint8_t *msg, size_t len) {
    // Server hello is "%7s%2i%2i": a 7-byte protocol name then major/minor.
    if (len < 11) { disconnect("short hello"); return; }

    memcpy(serverName_, msg, 7);
    serverName_[7] = '\0';
    const int16_t major = rdS16(msg + 7);
    const int16_t minor = rdS16(msg + 9);
    Serial.printf("[deskflow] server '%s' protocol %d.%d\r\n", serverName_, major, minor);

    if (major != kProtocolMajor) {
        disconnect("incompatible protocol major version");
        return;
    }

    // Reply "%7s%2i%2i%s": echo the server's own name so we work with Synergy,
    // Barrier and Deskflow alike, then our version and this screen's name.
    const char *screen = config_.deskflowScreen();
    const size_t nameLen = strlen(screen);
    uint8_t out[7 + 2 + 2 + 4 + 64];
    size_t o = 0;
    memcpy(out + o, serverName_, 7); o += 7;
    wr16(out + o, kProtocolMajor);   o += 2;
    wr16(out + o, kProtocolMinor);   o += 2;
    wr32(out + o, (uint32_t)nameLen); o += 4;      // %s is a length-prefixed string
    memcpy(out + o, screen, nameLen); o += nameLen;
    sendMessage(out, o);

    state_ = State::Active;
    lastError_[0] = '\0';
    backoffMs_ = 2000;
    Serial.printf("[deskflow] joined as screen '%s'\r\n", screen);
}

void DeskflowClient::sendScreenInfo() {
    // DINF: x, y, width, height, obsolete warp size, cursor x, cursor y.
    // The size we claim is the coordinate space the server will address us in,
    // so it should match the target's real resolution.
    const int16_t w = config_.deskflowWidth();
    const int16_t h = config_.deskflowHeight();
    uint8_t out[4 + 7 * 2];
    memcpy(out, "DINF", 4);
    wr16(out + 4,  0);
    wr16(out + 6,  0);
    wr16(out + 8,  w);
    wr16(out + 10, h);
    wr16(out + 12, 0);
    wr16(out + 14, (int16_t)(w / 2));
    wr16(out + 16, (int16_t)(h / 2));
    sendMessage(out, sizeof(out));
}

// --- dispatch --------------------------------------------------------------

void DeskflowClient::dispatch(const uint8_t *m, size_t len) {
    lastTrafficMs_ = millis();

    if (is(m, len, "CALV")) { sendCode("CALV"); return; }   // keep-alive
    if (is(m, len, "CNOP")) { return; }
    if (is(m, len, "QINF")) { sendScreenInfo(); return; }
    if (is(m, len, "CIAK")) { return; }
    if (is(m, len, "CROP")) { return; }
    if (is(m, len, "DSOP")) { return; }                     // options: nothing to set
    if (is(m, len, "DCLP")) { return; }                     // no clipboard on a HID device
    if (is(m, len, "CSEC")) { return; }                     // screensaver

    if (is(m, len, "CINN")) {                               // pointer entered this screen
        if (len >= 12) {
            const uint16_t x = rd16(m + 4), y = rd16(m + 6);
            hid_.mouseMoveAbsolute((float)x / config_.deskflowWidth(),
                                   (float)y / config_.deskflowHeight());
        }
        hasFocus_ = true;
        return;
    }

    if (is(m, len, "COUT")) {                               // pointer left this screen
        // Whatever was held when the pointer left must not stay held: the
        // controller has stopped sending us key-up events.
        hid_.releaseAll();
        hasFocus_ = false;
        return;
    }

    if (is(m, len, "DMMV") && len >= 8) {                   // absolute move
        const int16_t x = rdS16(m + 4), y = rdS16(m + 6);
        hid_.mouseMoveAbsolute((float)x / config_.deskflowWidth(),
                               (float)y / config_.deskflowHeight());
        return;
    }

    if (is(m, len, "DMRM") && len >= 8) {                   // relative move
        hid_.mouseMove(rdS16(m + 4), rdS16(m + 6));
        return;
    }

    if ((is(m, len, "DMDN") || is(m, len, "DMUP")) && len >= 5) {
        const bool down = (m[3] == 'N');
        MouseButton b;
        switch (m[4]) {                                     // 1 left, 2 middle, 3 right
            case 1:  b = MouseButton::Left;   break;
            case 2:  b = MouseButton::Middle; break;
            case 3:  b = MouseButton::Right;  break;
            default: return;
        }
        if (down) hid_.mouseButtonDown(b); else hid_.mouseButtonUp(b);
        return;
    }

    if (is(m, len, "DMWM") && len >= 8) {                   // wheel: x then y delta
        const int16_t xd = rdS16(m + 4), yd = rdS16(m + 6);
        // The protocol works in units of 120 per detent, as Windows does.
        if (yd) hid_.mouseWheel(yd / 120 ? yd / 120 : (yd > 0 ? 1 : -1));
        if (xd) hid_.mousePan(xd / 120 ? xd / 120 : (xd > 0 ? 1 : -1));
        return;
    }

    if ((is(m, len, "DKDN") || is(m, len, "DKUP")) && len >= 10) {
        const uint16_t keyId = rd16(m + 4);
        const uint8_t code = keyIdToHid(keyId);
        if (code == 0) return;                              // nothing sensible to send
        if (m[3] == 'N') hid_.keyDown(code); else hid_.keyUp(code);
        return;
    }

    if (is(m, len, "DKRP") && len >= 12) {                  // auto-repeat
        const uint16_t keyId = rd16(m + 4);
        const uint8_t code = keyIdToHid(keyId);
        // The target does its own auto-repeat once a key is held, so a repeat
        // message needs no action; re-sending would double it.
        (void)code;
        return;
    }

    if (is(m, len, "CBYE")) { disconnect("server closed the session"); return; }
    if (is(m, len, "EBSY")) { disconnect("screen name already in use"); return; }
    if (is(m, len, "EUNK")) { disconnect("server does not know this screen name"); return; }
    if (is(m, len, "EBAD")) { disconnect("server reported a protocol violation"); return; }
    if (is(m, len, "EICV")) { disconnect("incompatible protocol version"); return; }
}

// --- lifecycle -------------------------------------------------------------

void DeskflowClient::disconnect(const char *why) {
    if (why && why[0]) {
        snprintf(lastError_, sizeof(lastError_), "%s", why);
        Serial.printf("[deskflow] disconnected: %s\r\n", why);
    }
    if (hasFocus_ || hid_.anythingHeld()) hid_.releaseAll();
    hasFocus_ = false;
    sock_.stop();
    state_ = State::Idle;
    lastAttemptMs_ = millis();
}

void DeskflowClient::loop() {
    if (!config_.deskflowEnabled()) {
        if (state_ != State::Idle) disconnect("");
        return;
    }

    if (state_ == State::Idle) {
        if (WiFi.status() != WL_CONNECTED) return;
        if (millis() - lastAttemptMs_ < backoffMs_) return;
        lastAttemptMs_ = millis();

        if (!sock_.connect(config_.deskflowHost(), config_.deskflowPort(), 4000)) {
            snprintf(lastError_, sizeof(lastError_), "cannot reach %s:%u",
                     config_.deskflowHost(), (unsigned)config_.deskflowPort());
            // Back off up to 30s so a wrong address does not hammer the network.
            backoffMs_ = backoffMs_ < 30000 ? backoffMs_ * 2 : 30000;
            return;
        }
        sock_.setNoDelay(true);
        state_ = State::Handshaking;
        lastTrafficMs_ = millis();
        Serial.printf("[deskflow] connected to %s:%u\r\n",
                      config_.deskflowHost(), (unsigned)config_.deskflowPort());
        return;
    }

    if (!sock_.connected()) { disconnect("connection lost"); return; }

    uint8_t buf[kMaxMessage];
    size_t len = 0;
    // Drain everything queued: input arrives in bursts and leaving messages
    // waiting a loop iteration each would add visible lag.
    int guard = 32;
    while (guard-- > 0 && sock_.available() >= 4) {
        if (!readMessage(buf, sizeof(buf), len)) return;
        if (len == 0) continue;                     // drained an oversized message
        if (state_ == State::Handshaking) handshake(buf, len);
        else                              dispatch(buf, len);
    }

    // The server sends keep-alives; prolonged silence means the link is dead
    // even though TCP has not noticed yet.
    if (state_ == State::Active && millis() - lastTrafficMs_ > 15000) {
        disconnect("no keep-alive from server");
    }
}

}  // namespace ghosthid
