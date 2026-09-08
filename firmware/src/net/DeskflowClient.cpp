#include "DeskflowClient.h"

#include <Arduino.h>
#include <string.h>

#include "board_config.h"
#include "config/Config.h"
#include "hid/HidDevice.h"
#include "DeskflowWire.h"

#include <new>

#include <mbedtls/oid.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>
// arduino-esp32's low-level TLS client, reached for only by the cert-capture
// probe: it sets up the socket, entropy and mbedTLS config, and exposes the raw
// mbedtls_ssl_context so we can drop verification to OPTIONAL and read the
// server's certificate during a completed mutual handshake - something the
// high-level WiFiClientSecure connect() cannot do (setInsecure() would drop our
// client cert, and it frees the context before we could read the peer cert).
#include <ssl_client.h>

namespace ghosthid {
namespace {

constexpr int16_t kProtocolMajor = 1;
constexpr int16_t kProtocolMinor = 8;
constexpr size_t  kMaxMessage    = 512;   // input messages are tiny; clipboard is ignored

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



}  // namespace

uint8_t DeskflowClient::stateCode() const {
    if (!config_.deskflowEnabled()) return 0;
    if (state_ != State::Active)    return 1;
    return hasFocus_ ? 3 : 2;
}

const char *DeskflowClient::statusText() const {
    switch (state_) {
        case State::Idle:        return awaitingTrust_
                                     ? "confirm the server's certificate fingerprint"
                                     : (lastError_[0] ? lastError_ : "not connected");
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
        // Hard total deadline, checked every pass. Previously it was only tested
        // on a zero-length read, so a peer dribbling one byte at a time could
        // hold this task - and any key it was holding - indefinitely.
        if (millis() - start > timeoutMs) return false;
        const int n = sock_->read(dst + got, len - got);
        if (n > 0) { got += n; continue; }
        delay(1);
    }
    return true;
}

bool DeskflowClient::readMessage(uint8_t *buf, size_t cap, size_t &outLen) {
    if (sock_->available() < 4) return false;

    uint8_t hdr[4];
    if (!readExactly(hdr, 4, 500)) { disconnect("truncated length"); return false; }
    const uint32_t len = rd32(hdr);

    if (len == 0) { disconnect("zero-length message"); return false; }

    // Sanity ceiling. We PARSE only small messages (<= cap); anything larger is a
    // clipboard transfer we drain and discard, so the ceiling only needs to guard
    // against a desynced/garbage length - it must NOT be so low that a real
    // clipboard trips it. The server sends the WHOLE clipboard to this screen on
    // every focus-enter, and an image or a large text selection on the source
    // machine is easily megabytes; a 64KB cap here dropped the link on every such
    // copy, and since the server resends on reconnect it looped until the
    // clipboard changed to something small. Drain up to a few MB instead.
    constexpr uint32_t kMaxDrain = 4u * 1024 * 1024;
    if (len > kMaxDrain) { disconnect("implausible message length"); return false; }

    if (len > cap) {
        // Almost certainly a clipboard transfer. We have no clipboard to offer,
        // so drain it rather than dropping the connection over it. Drain into the
        // caller's full buffer, not a 64-byte sink: Barrier resends the whole
        // clipboard every time focus enters this screen, and draining tens of KB
        // 64 bytes at a time blocked this task long enough (tens of ms) that
        // incoming data piled up in lwIP and the largest heap block collapsed,
        // killing the session. cap (512) bytes per read is 8x fewer iterations
        // and reuses memory already on the stack.
        uint32_t left = len;
        while (left > 0) {
            const size_t chunk = left < cap ? left : cap;
            if (!readExactly(buf, chunk, 2000)) { disconnect("drain failed"); return false; }
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
    // M4: this is 7 raw bytes off the wire, later echoed into get_config's JSON
    // (kvm_server) and the serial log. Clamp it to a safe charset so a hostile
    // or spoofed server can't inject quotes/backslashes/control bytes there.
    for (char *p = serverName_; *p; ++p) {
        const char c = *p;
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == ' ' || c == '_' ||
                        c == '-' || c == '.';
        if (!ok) *p = '?';
    }
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


// --- trust on first use ----------------------------------------------------

void DeskflowClient::freePending() {
    free(pendingPem_);
    pendingPem_ = nullptr;
    pendingFp_[0] = '\0';
}

// Turn the captured DER certificate into the pinnable PEM and the hex
// fingerprint the user confirms. The fingerprint is the SHA-256 of the DER -
// the exact value Deskflow/Barrier write to their trusted lists - so a user can
// compare the two directly.
bool DeskflowClient::storePending(const unsigned char *der, size_t derLen) {
    uint8_t hash[32];
    if (mbedtls_sha256(der, derLen, hash, 0) != 0) return false;
    for (int i = 0; i < 32; ++i) snprintf(pendingFp_ + i * 2, 3, "%02x", hash[i]);

    // DER -> PEM: base64 the DER, wrap at 64 columns, add the armour lines, so
    // the result is byte-for-byte what a pasted PEM would be and pins the same
    // way through setDeskflowServerCert()/setCACert().
    size_t b64len = 0;
    mbedtls_base64_encode(nullptr, 0, &b64len, der, derLen);   // query length
    if (b64len == 0 || b64len > 3600) { pendingFp_[0] = '\0'; return false; }
    unsigned char *b64 = static_cast<unsigned char *>(malloc(b64len + 1));
    if (b64 == nullptr) { pendingFp_[0] = '\0'; return false; }
    if (mbedtls_base64_encode(b64, b64len + 1, &b64len, der, derLen) != 0) {
        free(b64); pendingFp_[0] = '\0'; return false;
    }
    const size_t lines  = (b64len + 63) / 64;
    const size_t pemCap = 28 /*BEGIN\n*/ + b64len + lines /*\n per line*/ +
                          26 /*END\n*/ + 1;
    char *pem = static_cast<char *>(malloc(pemCap));
    if (pem == nullptr) { free(b64); pendingFp_[0] = '\0'; return false; }
    size_t w = 0;
    w += snprintf(pem + w, pemCap - w, "-----BEGIN CERTIFICATE-----\n");
    for (size_t i = 0; i < b64len; i += 64) {
        const size_t n = (b64len - i < 64) ? (b64len - i) : 64;
        memcpy(pem + w, b64 + i, n); w += n;
        pem[w++] = '\n';
    }
    w += snprintf(pem + w, pemCap - w, "-----END CERTIFICATE-----\n");
    pem[w] = '\0';
    free(b64);
    free(pendingPem_);
    pendingPem_ = pem;
    return true;
}

// One throwaway handshake whose only job is to capture the server's
// certificate. See the ssl_client.h include note for why this drops below
// WiFiClientSecure.
bool DeskflowClient::captureServerCert() {
    if (!identity_.begin(config_.deskflowScreen())) {
        snprintf(lastError_, sizeof(lastError_), "could not create a TLS identity");
        return false;
    }
    IPAddress addr;
    if (!addr.fromString(config_.deskflowHost()) &&
        !WiFi.hostByName(config_.deskflowHost(), addr)) {
        snprintf(lastError_, sizeof(lastError_), "cannot resolve %s", config_.deskflowHost());
        return false;
    }
    // The context carries mbedTLS structs; keep it off this task's 8KB stack.
    sslclient_context *ctx = new (std::nothrow) sslclient_context();
    if (ctx == nullptr) {
        snprintf(lastError_, sizeof(lastError_), "out of memory for cert capture");
        return false;
    }
    ssl_init(ctx);
    ctx->handshake_timeout = 12000;
    // start_ssl_client only loads our client cert when a CA is supplied and
    // verification is on. We do not have the server's CA yet - that is the
    // point - so pass our own cert as a throwaway CA to take that path, then
    // drop verification to OPTIONAL below so the bogus CA is ignored and the
    // handshake completes. Mutual TLS still works (the server receives our real
    // client cert); the server presents its certificate in its first flight,
    // which is what we read out afterwards.
    const int sock = start_ssl_client(
        ctx, addr, config_.deskflowPort(), config_.deskflowHost(), 12000,
        /*rootCABuff=*/identity_.certificatePem(), /*useBundle=*/false,
        /*cli_cert=*/identity_.certificatePem(), /*cli_key=*/identity_.privateKeyPem(),
        /*pskIdent=*/nullptr, /*psKey=*/nullptr, /*insecure=*/false, /*alpn=*/nullptr);
    bool ok = false;
    if (sock >= 0) {
        mbedtls_ssl_conf_authmode(&ctx->ssl_conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
        const uint32_t start = millis();
        int ret;
        while ((ret = mbedtls_ssl_handshake(&ctx->ssl_ctx)) != 0) {
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) break;
            if (millis() - start > 12000) { ret = -1; break; }
            vTaskDelay(2);
        }
        if (ret == 0) {
            const mbedtls_x509_crt *crt = mbedtls_ssl_get_peer_cert(&ctx->ssl_ctx);
            if (crt != nullptr && crt->raw.p != nullptr && crt->raw.len > 0) {
                ok = storePending(crt->raw.p, crt->raw.len);
                if (!ok) snprintf(lastError_, sizeof(lastError_),
                                  "captured certificate but could not store it");
            } else {
                snprintf(lastError_, sizeof(lastError_), "server sent no certificate");
            }
        } else {
            snprintf(lastError_, sizeof(lastError_),
                     "could not reach the server to read its certificate");
        }
    } else {
        snprintf(lastError_, sizeof(lastError_), "cannot reach %s:%u",
                 config_.deskflowHost(), (unsigned)config_.deskflowPort());
    }
    stop_ssl_socket(ctx);
    delete ctx;
    return ok;
}

// --- lifecycle -------------------------------------------------------------

void DeskflowClient::disconnect(const char *why) {
    if (why && why[0]) {
        snprintf(lastError_, sizeof(lastError_), "%s", why);
        Serial.printf("[deskflow] disconnected: %s\r\n", why);
    }
    if (hasFocus_ || hid_.anythingHeld()) hid_.releaseAll();
    hasFocus_ = false;
    heldByButtonCount_ = 0;
    haveAbs_ = false;
    relDx_ = relDy_ = 0;
    if (sock_) sock_->stop();
    // Free the session's private copy of the CA PEM now the socket is gone.
    free(caCopy_);
    caCopy_ = nullptr;
    state_ = State::Idle;
    lastAttemptMs_ = millis();
}

// suspend()/reconnect() run on the *network* task. They must not touch the
// socket or TLS state, which this client's own task is reading - a cross-task
// sock_->stop() frees the mbedTLS contexts (and the two TlsArena blocks) out
// from under a live handshake. So they only raise a flag; serviceOnce() acts on
// it on this task.
void DeskflowClient::suspend() {
    suspendReq_ = true;
}

void DeskflowClient::reconnect() {
    reconnectReq_ = true;
}

void DeskflowClient::serviceOnce() {
    // Act on cross-task requests here, on our own task, before anything else.
    if (suspendReq_) {
        suspendReq_ = false;
        if (state_ != State::Idle) disconnect("suspended for a firmware update");
        backoffMs_ = 60000;     // long, so it does not race an update for memory
        lastAttemptMs_ = millis();
    }
    if (reconnectReq_) {
        reconnectReq_ = false;
        if (state_ != State::Idle) disconnect("settings changed");
        // Settings changed - a freshly pasted cert, a new host - so any pending
        // capture is stale. Drop it and re-evaluate from scratch.
        awaitingTrust_ = false;
        freePending();
        lastError_[0] = '\0';
        backoffMs_ = 0;         // retry at once rather than serving out a backoff
        lastAttemptMs_ = 0;
    }

    // Confirm a captured certificate: pin it, then connect for real. Done on
    // this task so it does not race the TLS state the connect path reads.
    if (trustReq_) {
        trustReq_ = false;
        if (pendingPem_ != nullptr) {
            config_.setDeskflowServerCert(pendingPem_);   // pins it in NVS
            freePending();
            awaitingTrust_ = false;
            if (state_ != State::Idle) disconnect("certificate trusted");
            lastError_[0] = '\0';
            backoffMs_ = 0;
            lastAttemptMs_ = 0;
        }
    }

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
            // Take a private copy of the pinned CA under Config's lock. mbedTLS
            // reads this buffer throughout the handshake; a set_config on the
            // network task replacing Config's own buffer mid-parse was a
            // use-after-free. Freed on disconnect.
            free(caCopy_);
            caCopy_ = static_cast<char *>(malloc(4001));
            if (caCopy_ == nullptr) {
                snprintf(lastError_, sizeof(lastError_),
                         "out of memory for the server certificate");
                backoffMs_ = 30000;
                return;
            }
            config_.copyServerCert(caCopy_, 4001);
            if (caCopy_[0] == '\0') {
                // No certificate pinned yet. Rather than making the user paste a
                // PEM, capture the server's certificate once and wait for them
                // to confirm its fingerprint (trust on first use). We never use
                // an unconfirmed certificate for a real session, so the confirm
                // step is the out-of-band check that defeats a first-connection
                // MITM.
                free(caCopy_);
                caCopy_ = nullptr;
                if (awaitingTrust_) {
                    // Already captured; idle until the user confirms. The state
                    // and fingerprint are surfaced in the UI.
                    backoffMs_ = 60000;
                    return;
                }
                if (captureServerCert()) {
                    awaitingTrust_ = true;
                    snprintf(lastError_, sizeof(lastError_),
                             "new server - confirm its certificate fingerprint to connect");
                    backoffMs_ = 60000;
                } else {
                    // lastError_ already says why the capture failed; retry.
                    backoffMs_ = backoffMs_ < 30000 ? backoffMs_ * 2 : 30000;
                }
                return;
            }
            tls_.setCACert(caCopy_);
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
            const bool haveCn = commonNameOf(caCopy_, cn, sizeof(cn));
            tls_.setTimeout(12);
            ok = tls_.connect(addr, config_.deskflowPort(),
                              haveCn ? cn : config_.deskflowHost(),
                              caCopy_,
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
        // Both transports: Nagle batching is wrong for a stream of tiny input
        // events, and the TLS socket was previously left with it enabled.
        if (config_.deskflowTls()) tls_.setNoDelay(true);
        else                       plain_.setNoDelay(true);
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

    // One pointer report per pass, carrying the newest position. This is the
    // difference between tracking the pointer and chasing it.
    flushPointer();

    // Held-input backstop. If something is down and the server has gone silent
    // (a crash, a power cut, Wi-Fi loss - no TCP FIN), release it well before
    // the 15s keep-alive timeout below would. Keep-alive traffic normally keeps
    // lastTrafficMs_ fresh, so this only fires when the server is genuinely gone.
    if (state_ == State::Active && hid_.anythingHeld() &&
        millis() - lastTrafficMs_ > GHOSTHID_KVM_HELD_TIMEOUT_MS) {
        Serial.println("[deskflow] server silent while input held - releasing all");
        hid_.releaseAll();
        heldByButtonCount_ = 0;
    }

    // The server sends keep-alives; prolonged silence means the link is dead
    // even though TCP has not noticed yet.
    if (state_ == State::Active && millis() - lastTrafficMs_ > 15000) {
        disconnect("no keep-alive from server");
    }
}




void DeskflowClient::run() {
    // A fixed beat, not a conditional one. The previous version measured
    // "busy" *after* draining, when available() is almost always zero, so it
    // took the slow branch during active motion - roughly 200Hz delivered at an
    // uneven interval. Evenness matters more than raw rate here: a steady
    // cadence reads as smooth motion, a varying one reads as stepping even at
    // the same average rate.
    TickType_t last = xTaskGetTickCount();
    uint32_t statAt = 0, nMovePrev = 0;
    uint32_t worstPassUs = 0;
    for (;;) {
        const uint32_t t0 = micros();
        serviceOnce();
        const uint32_t dt = micros() - t0;
        if (dt > worstPassUs) worstPassUs = dt;

        // TEMP diagnostic: watch what changes as sustained mouse motion "goes
        // downhill". Every 2s over serial (non-blocking): heap, this task's
        // stack headroom, the DMMV rate, refused HID reports, and the slowest
        // serviceOnce pass in the window. A leak shows as falling heap; USB
        // backpressure as rising drop / worst-pass; a backlog as a low move rate.
        const uint32_t now = millis();
        if (now - statAt > 2000) {
            Serial.printf("[stat] heap=%u/%u kb stack=%u move/s=%u drop=%u worstpass=%uus state=%d\r\n",
                          (unsigned)(ESP.getFreeHeap() / 1024),
                          (unsigned)(ESP.getMaxAllocHeap() / 1024),
                          (unsigned)uxTaskGetStackHighWaterMark(nullptr),
                          (unsigned)((nMove_ - nMovePrev) / 2),
                          (unsigned)hid_.droppedReports(),
                          (unsigned)worstPassUs, (int)state_);
            statAt = now; nMovePrev = nMove_; worstPassUs = 0;
        }
        vTaskDelayUntil(&last, 1);     // one tick, and it does not drift
    }
}

void DeskflowClient::taskEntry(void *self) {
    static_cast<DeskflowClient *>(self)->run();
}

void DeskflowClient::begin() {
    // 8KB of stack: a TLS handshake needs real depth.
    //
    // Priority 3: above the idle-ish band so input is not starved, below the
    // networking stacks.
#if CONFIG_FREERTOS_UNICORE
    // Single-core (ESP32-S2): everything shares one core. Wi-Fi, lwIP, USB and
    // this task compete; under load this task loses and the screen session
    // drops. Nothing to pin to - this is the limitation the S3 build solves.
    xTaskCreate(taskEntry, "deskflow", 8192, this, 3, nullptr);
#else
    // Dual-core (ESP32-S3): pin the input task to the APP core (1). Wi-Fi and
    // lwIP run on the PRO core (0), so a Wi-Fi/TLS burst on core 0 can no longer
    // stall keep-alive handling or pointer reports here. This is the fix for the
    // single-core CPU starvation that dropped the session under load on the S2
    // (measured: 166ms ICMP spikes, 48-128ms serviceOnce stalls, all core
    // contention, not RAM and not RF).
    xTaskCreatePinnedToCore(taskEntry, "deskflow", 8192, this, 3, nullptr, 1);
#endif
}

}  // namespace ghosthid
