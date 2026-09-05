#include "DeskflowClient.h"

#include <Arduino.h>
#include <string.h>

#include "board_config.h"
#include "config/Config.h"
#include "hid/HidDevice.h"

#include <mbedtls/oid.h>
#include <mbedtls/x509_crt.h>

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
// The common name of a PEM certificate, or an empty string.
//
// mbedTLS verifies the hostname against the server's certificate, and these
// servers use a self-signed certificate with a fixed CN ("Deskflow",
// "Synergy") while being reached by IP address - so the check fails on a name
// mismatch even though the certificate is exactly the one we pinned. Since we
// pinned it, matching its own CN is the correct check to make.
bool commonNameOf(const char *pem, char *out, size_t outSize) {
    if (pem == nullptr || pem[0] == '\0') return false;
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    bool ok = false;
    if (mbedtls_x509_crt_parse(&crt, (const unsigned char *)pem, strlen(pem) + 1) == 0) {
        for (const mbedtls_x509_name *n = &crt.subject; n != nullptr; n = n->next) {
            if (MBEDTLS_OID_CMP(MBEDTLS_OID_AT_CN, &n->oid) == 0) {
                const size_t len = n->val.len < outSize - 1 ? n->val.len : outSize - 1;
                memcpy(out, n->val.p, len);
                out[len] = '\0';
                ok = len > 0;
                break;
            }
        }
    }
    mbedtls_x509_crt_free(&crt);
    return ok;
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

uint8_t DeskflowClient::stateCode() const {
    if (!config_.deskflowEnabled()) return 0;
    if (state_ != State::Active)    return 1;
    return hasFocus_ ? 3 : 2;
}

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
        if (!sock_->connected()) return false;
        const int n = sock_->read(dst + got, len - got);
        if (n > 0) { got += n; continue; }
        if (millis() - start > timeoutMs) return false;
        delay(1);
    }
    return true;
}

bool DeskflowClient::readMessage(uint8_t *buf, size_t cap, size_t &outLen) {
    if (sock_->available() < 4) return false;

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
    sock_->write(hdr, 4);
    sock_->write(payload, len);
    sock_->flush();
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
    if (is(m, len, "LSYN")) { return; }                     // language sync (1.8+)

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
    if (is(m, len, "EUNK")) {
        // The commonest setup mistake, so say what to do about it.
        char msg[80];
        snprintf(msg, sizeof(msg), "server has no screen named '%s'", config_.deskflowScreen());
        disconnect(msg);
        backoffMs_ = 30000;     // no point retrying hard; this needs a human
        return;
    }
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
    if (sock_) sock_->stop();
    state_ = State::Idle;
    lastAttemptMs_ = millis();
}

void DeskflowClient::reconnect() {
    if (state_ != State::Idle) disconnect("settings changed");
    lastError_[0] = '\0';
    backoffMs_ = 0;          // retry at once rather than serving out a backoff
    lastAttemptMs_ = 0;
}

void DeskflowClient::serviceOnce() {
    if (!config_.deskflowEnabled()) {
        if (state_ != State::Idle) disconnect("");
        return;
    }

    if (state_ == State::Idle) {
        if (WiFi.status() != WL_CONNECTED) return;
        if (millis() - lastAttemptMs_ < backoffMs_) return;
        lastAttemptMs_ = millis();

        // Pick the transport before connecting. The certificate is self-signed
        // and identified by fingerprint in this protocol's own model, so chain
        // verification is not applicable - setInsecure() is the correct
        // behaviour here rather than a shortcut.
        if (config_.deskflowTls()) {
            // These servers require MUTUAL TLS: without a client certificate
            // the handshake is refused with "certificate required" and nothing
            // further happens, which looks exactly like the server ignoring us.
            if (!identity_.begin(config_.deskflowScreen())) {
                snprintf(lastError_, sizeof(lastError_), "could not create a TLS identity");
                backoffMs_ = 30000;
                return;
            }
            // Do NOT call setInsecure() here. arduino-esp32 loads the client
            // certificate only when verification is enabled:
            //     if (!insecure && cli_cert != NULL && cli_key != NULL)
            // so setInsecure() silently drops our identity, and the server
            // rejects the handshake with "peer did not return a certificate".
            // Pinning the server's certificate keeps verification on, which is
            // both what makes mutual TLS work and the trust model this protocol
            // uses in the first place.
            if (config_.deskflowServerCert()[0] == '\0') {
                snprintf(lastError_, sizeof(lastError_),
                         "server certificate not set - paste the server's PEM in settings");
                backoffMs_ = 30000;
                return;
            }
            tls_.setCACert(config_.deskflowServerCert());
            tls_.setCertificate(identity_.certificatePem());
            tls_.setPrivateKey(identity_.privateKeyPem());
            tls_.setTimeout(12);
            sock_ = &tls_;
        } else {
            sock_ = &plain_;
        }

        if (config_.deskflowTls()) {
            Serial.printf("[deskflow] pre-handshake heap: %u free, %u largest block\r\n",
                          (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
        }
        bool ok;
        if (config_.deskflowTls()) {
            // Resolve first so we can connect by address while presenting the
            // certificate's own common name for verification.
            IPAddress addr;
            if (!addr.fromString(config_.deskflowHost()) &&
                !WiFi.hostByName(config_.deskflowHost(), addr)) {
                snprintf(lastError_, sizeof(lastError_), "cannot resolve %s",
                         config_.deskflowHost());
                backoffMs_ = backoffMs_ < 30000 ? backoffMs_ * 2 : 30000;
                return;
            }
            char cn[64] = {};
            const bool haveCn = commonNameOf(config_.deskflowServerCert(), cn, sizeof(cn));
            tls_.setTimeout(12);
            ok = tls_.connect(addr, config_.deskflowPort(),
                              haveCn ? cn : config_.deskflowHost(),
                              config_.deskflowServerCert(),
                              identity_.certificatePem(),
                              identity_.privateKeyPem());
            if (!ok && haveCn) {
                Serial.printf("[deskflow] verified against CN '%s'\r\n", cn);
            }
        } else {
            ok = plain_.connect(config_.deskflowHost(), config_.deskflowPort(), 4000);
        }
        if (!ok) {
            if (config_.deskflowTls()) {
                // A failed TLS handshake and an unreachable host look identical
                // from connect()'s return value, so ask mbedTLS what happened.
                char detail[96] = {};
                const int err = tls_.lastError(detail, sizeof(detail));
                if (err == -32512 /* MBEDTLS_ERR_SSL_ALLOC_FAILED */) {
                    // Say what to do rather than what went wrong. The handshake
                    // wants 16KB in one piece; the web server fragments the
                    // heap, and a reboot connects before it starts.
                    snprintf(lastError_, sizeof(lastError_),
                             "not enough contiguous memory (%uK largest, needs 16K) "
                             "- reboot to connect during startup",
                             (unsigned)(ESP.getMaxAllocHeap() / 1024));
                } else {
                    snprintf(lastError_, sizeof(lastError_),
                             "TLS failed (%d) %s [heap %uK free, %uK largest]", err, detail,
                             (unsigned)(ESP.getFreeHeap() / 1024),
                             (unsigned)(ESP.getMaxAllocHeap() / 1024));
                }
                Serial.printf("[deskflow] TLS connect failed: %d %s\r\n", err, detail);
                Serial.printf("[deskflow] free heap %u, largest block %u\r\n",
                              (unsigned)ESP.getFreeHeap(),
                              (unsigned)ESP.getMaxAllocHeap());
            } else {
                snprintf(lastError_, sizeof(lastError_), "cannot reach %s:%u",
                         config_.deskflowHost(), (unsigned)config_.deskflowPort());
            }
            // Back off up to 30s so a wrong address does not hammer the network.
            backoffMs_ = backoffMs_ < 30000 ? backoffMs_ * 2 : 30000;
            return;
        }
        if (!config_.deskflowTls()) plain_.setNoDelay(true);
        state_ = State::Handshaking;
        lastTrafficMs_ = millis();
        Serial.printf("[deskflow] connected to %s:%u%s\r\n",
                      config_.deskflowHost(), (unsigned)config_.deskflowPort(),
                      config_.deskflowTls() ? " over TLS" : "");
        return;
    }

    if (sock_ == nullptr) { state_ = State::Idle; return; }
    if (!sock_->connected()) { disconnect("connection lost"); return; }

    uint8_t buf[kMaxMessage];
    size_t len = 0;
    // Drain hard. Pointer motion arrives as a dense burst of DMMV, and leaving
    // any of it queued shows up directly as lag. The cap only exists so a
    // pathological peer cannot hold this task forever.
    int guard = 256;
    while (guard-- > 0 && sock_->available() >= 4) {
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

void DeskflowClient::run() {
    for (;;) {
        serviceOnce();
        // Yield for a single tick while input is flowing, and back off when
        // idle. Sleeping a fixed 5ms even mid-burst added latency for no gain.
        const bool busy = (sock_ != nullptr && sock_->connected() && sock_->available() > 0);
        vTaskDelay(busy ? 1 : pdMS_TO_TICKS(5));
    }
}

void DeskflowClient::taskEntry(void *self) {
    static_cast<DeskflowClient *>(self)->run();
}

void DeskflowClient::begin() {
    // 8KB of stack: a TLS handshake needs real depth. Priority 1 sits below
    // the Wi-Fi and TCP stacks, so a blocking handshake yields to them rather
    // than starving them.
    xTaskCreate(taskEntry, "deskflow", 8192, this, 1, nullptr);
}

}  // namespace ghosthid
