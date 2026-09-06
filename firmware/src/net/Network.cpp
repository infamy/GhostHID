#include "Network.h"

#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <esp_mac.h>
#include <Update.h>
#include <new>

#include "board_config.h"
#include "DeskflowClient.h"
#include "config/Config.h"
#include "protocol/CommandProcessor.h"
#include "web/WebAssets.h"

namespace ghosthid {
namespace {

AsyncWebServer    g_server(80);
AsyncWebSocket    g_ws("/ws");
Network          *g_network = nullptr;
CommandProcessor *g_processor = nullptr;
Config           *g_config = nullptr;
DeskflowClient   *g_deskflow = nullptr;

// Derives a stable 4-hex-digit device suffix from the Wi-Fi MAC, so two
// GhostHIDs in the same room do not collide.
void deviceSuffix(char out[5]) {
    // Read the base MAC from efuse, not WiFi.macAddress(): the latter returns
    // 00:00:.. in AP-only mode (no STA netif), which produced the "-0000" SSID.
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, 5, "%02X%02X", mac[4], mac[5]);
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
               AwsEventType type, void *arg, uint8_t *data, size_t len) {
    switch (type) {
        case WS_EVT_CONNECT:
            // Up to kMaxControllers may attach at once, each authenticating on
            // its own. Only a genuinely full device is refused - so a stray or
            // duplicate tab no longer locks out the operator.
            if (!g_network->acquireClientSlot(client->id())) {
                client->close(1013, "device full (too many controllers)");
                return;
            }
            // Nagle batches small writes, which is exactly wrong for a stream
            // of tiny input events; without this each report can wait for an
            // ACK or a 40ms coalescing timer.
            client->client()->setNoDelay(true);
            g_processor->beginSession(client->id());
            Serial.printf("[ws] client %u connected from %s (%u total)\r\n",
                          client->id(), client->remoteIP().toString().c_str(),
                          (unsigned)g_network->clientCount());
            break;

        case WS_EVT_DISCONNECT:
        case WS_EVT_ERROR:
            // A connection we refused (device full) also fires this; only act on
            // one we actually accepted. Each accepted client ends its own
            // session; held input is released once the last one leaves.
            if (!g_network->isClientConnected(client->id())) break;
            g_network->releaseClientSlot(client->id());
            g_processor->endSession(client->id());
            Serial.printf("[ws] client %u disconnected (%u left)\r\n",
                          client->id(), (unsigned)g_network->clientCount());
            break;

        case WS_EVT_DATA: {
            // Ignore anything from a client we refused: it is not a session.
            if (!g_network->isClientConnected(client->id())) return;
            AwsFrameInfo *info = static_cast<AwsFrameInfo *>(arg);
            if (info->opcode != WS_TEXT || !info->final || info->index != 0 ||
                info->len != len) {
                return;  // ignore fragmented/binary frames for now
            }
            // 1KB, not 512: get_config with a long screen-client error string
            // and a fingerprint runs to ~650 bytes, and status to ~530. At 512
            // vsnprintf truncated them into JSON with no closing brace, so the
            // browser threw on parse every poll - worst exactly when the device
            // had a TLS error to report. This is on the async_tcp stack, which
            // has headroom for it.
            char response[1024];
            g_processor->handleMessage(client->id(), reinterpret_cast<const char *>(data), len,
                                       response, sizeof(response));
            // Only reply when the processor produced something: key and mouse
            // events are fire-and-forget, and echoing every one of them would
            // add latency to the path we most care about.
            if (response[0] != '\0') client->text(response);
            // Drop the socket after repeated auth failures (H4): a brute-force
            // script then has to reconnect and wait out the cooldown per guess.
            if (g_processor->consumeDisconnectRequest())
                client->close(1008, "auth failed");
            break;
        }

        default:
            break;
    }
}


// ---------------------------------------------------------------------------
// Firmware update over the network
//
// The partition table is already dual-slot (app0/app1 + otadata), so a write
// lands in the *inactive* slot and otadata only switches once the image
// validates. An interrupted upload therefore leaves the running firmware
// untouched - retry and nothing is lost.
// ---------------------------------------------------------------------------

// Per-request OTA state, hung off the request via _tempObject (the async server
// free()s it when the request is destroyed). Keeping this per-request instead of
// in file-scope globals is what stops one POST from corrupting another's upload:
// an unauthenticated or concurrent POST used to set a *shared* "failed" flag,
// which aborted a legitimate in-flight update - an unauthenticated DoS (H5).
struct OtaState {
    bool failed  = false;
    bool begun   = false;
    bool replied = false;
    char error[128] = {};
};

// Update is a single global flash writer - only one upload may run at a time.
// This is the request that currently owns it; a second concurrent POST is
// refused without touching this one's state or the writer.
AsyncWebServerRequest *g_otaOwner = nullptr;

OtaState *otaStateOf(AsyncWebServerRequest *request) {
    if (request->_tempObject == nullptr) {
        void *mem = calloc(1, sizeof(OtaState));
        if (mem) request->_tempObject = new (mem) OtaState();
    }
    return static_cast<OtaState *>(request->_tempObject);
}

// Ends the request from inside the body handler. Without this we would keep
// accepting an 800KB upload we have already decided to reject, and the client
// sees a reset connection rather than the reason. Records the failure on the
// REQUEST, never on shared state.
void otaFail(AsyncWebServerRequest *request, int code, const char *why) {
    OtaState *st = otaStateOf(request);
    if (st) {
        snprintf(st->error, sizeof(st->error), "%s", why);
        st->failed = true;
        if (st->replied) return;
        st->replied = true;
    }
    request->send(code, "application/json",
                  String("{\"type\":\"error\",\"error\":\"") + why + "\"}");
}

bool hostAllowed(AsyncWebServerRequest *r);   // defined below

// Constant-time string compare (no early return) - the HTTP token check must not
// leak the token prefix-by-prefix via timing, same as the WS path's ctEquals.
bool ctEqualsStr(const char *a, const char *b) {
    const size_t la = strlen(a), lb = strlen(b);
    unsigned char d = static_cast<unsigned char>(la ^ lb);
    const size_t n = la > lb ? la : lb;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char ca = i < la ? static_cast<unsigned char>(a[i]) : 0;
        const unsigned char cb = i < lb ? static_cast<unsigned char>(b[i]) : 0;
        d |= static_cast<unsigned char>(ca ^ cb);
    }
    return d == 0;
}

// HTTP token brute-force throttle (H7). The WS path is throttled (H4) but the
// HTTP endpoints were not, and HTTP is concurrent - the real guessing oracle.
// Only a *supplied but wrong* token counts, so ordinary unauthenticated probes
// don't lock out the operator; 5 strikes -> a 30s cooldown (bounded, so it's a
// weak DoS lever), reset on success. Global, which suits a single-user device.
uint8_t  g_httpAuthFails = 0;
uint32_t g_httpCooldownUntil = 0;

// This endpoint installs arbitrary code on a device that types into someone's
// computer. It must never be reachable without the token - and never from a
// rebound DNS name pointing a victim's browser at us (Host cross-check).
bool otaAuthorized(AsyncWebServerRequest *request) {
    if (!hostAllowed(request)) return false;
    const uint32_t now = millis();
    if (g_httpCooldownUntil != 0 &&
        static_cast<int32_t>(g_httpCooldownUntil - now) > 0) {
        return false;   // in cooldown: refuse everything, even the right token
    }
    const char *tok = g_config->authToken();
    if (tok == nullptr || tok[0] == '\0') return true;   // auth disabled

    String given;
    bool have = false;
    if (request->hasHeader("X-GhostHID-Token")) {
        given = request->getHeader("X-GhostHID-Token")->value(); have = true;
    } else if (request->hasParam("token")) {
        given = request->getParam("token")->value(); have = true;
    }
    if (!have) return false;                             // no token: unauth, uncounted

    // The token is shown on the LCD in 4-char blocks; accept it with or without
    // the spaces. Real tokens have none.
    given.replace(" ", "");

    if (ctEqualsStr(given.c_str(), tok)) { g_httpAuthFails = 0; return true; }
    if (++g_httpAuthFails >= 5) { g_httpCooldownUntil = now + 30000; g_httpAuthFails = 0; }
    return false;
}

// True when something else is using enough memory that an update is risky.
bool otaContended() {
    return g_deskflow != nullptr && g_deskflow->connected();
}

// --- Origin / Host validation (H1: no drive-by from another web page) -------
// WebSockets are exempt from the same-origin policy, so without this any page a
// LAN user opens could connect and type. Reject a browser Origin that is not
// our own address, and cross-check the Host header to blunt DNS rebinding.
// A missing Origin means a native (non-browser) client - allowed.
bool hostIsOurs(String h) {
    h.toLowerCase();
    const int c = h.indexOf(':');
    if (c >= 0) h = h.substring(0, c);          // strip :port
    // Deliberately NOT allowing localhost/127.0.0.1: this device is only ever
    // reached over the network, so a "localhost" Origin means a page on the
    // victim's own machine trying to use us - exactly what we're blocking.
    if (h.length() == 0) return false;
    if (g_network) {
        if (h == String(g_network->apAddress())) return true;
        const char *s = g_network->staAddress();
        if (s && s[0] && h == String(s)) return true;
    }
    if (g_network) {
        // The registered mDNS hostname is name-SUFFIX (same string as the AP
        // SSID), not the bare device name - so the .local URL the README and
        // boot log advertise must be matched here or the device's own page is
        // refused (N2). ssid_ holds exactly that string.
        String m = String(g_network->ssid());
        m.toLowerCase();
        if (m.length() && (h == m || h == m + ".local")) return true;
    }
    if (g_config) {
        String n = String(g_config->deviceName());
        n.toLowerCase();
        if (h == n || h == n + ".local") return true;
    }
    return false;
}

bool originAllowed(AsyncWebServerRequest *r) {
    if (!r->hasHeader("Origin")) return true;   // native client, no browser origin
    String o = r->getHeader("Origin")->value();
    const int s = o.indexOf("://");
    if (s >= 0) o = o.substring(s + 3);
    const int sl = o.indexOf('/');
    if (sl >= 0) o = o.substring(0, sl);
    return hostIsOurs(o);
}

bool hostAllowed(AsyncWebServerRequest *r) {
    if (!r->hasHeader("Host")) return true;
    return hostIsOurs(r->getHeader("Host")->value());
}

// Append `s` to `out` as a JSON string body (no surrounding quotes), escaping
// the characters JSON forbids raw - the server certificate is PEM, so it has
// newlines that would otherwise produce invalid JSON.
void appendJsonEscaped(String &out, const char *s) {
    if (s == nullptr) return;
    for (const char *p = s; *p; ++p) {
        const char c = *p;
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char u[7];
                    snprintf(u, sizeof(u), "\\u%04x", static_cast<unsigned>(c));
                    out += u;
                } else {
                    out += c;
                }
        }
    }
}

// The importable settings as a JSON document, for provisioning another board.
// Deliberately excludes secrets (Wi-Fi/AP passwords, auth token) - those stay
// write-only. The server certificate is the server's *public* cert, not a
// secret, so it is included: it is the fiddliest part to move by hand.
String buildExportJson() {
    String j;
    j.reserve(2048);
    j += "{\"ghosthid_export\":1,\"version\":\"";
    appendJsonEscaped(j, GHOSTHID_VERSION);
    j += "\",\"name\":\"";
    appendJsonEscaped(j, g_config->deviceName());
    j += "\",\"ap_always\":";
    j += g_config->apAlways() ? "true" : "false";
    j += ",\"scroll_invert\":";
    j += g_config->scrollInvert() ? "true" : "false";
    j += ",\"sta_ssid\":\"";
    appendJsonEscaped(j, g_config->staSsid());
    j += "\",\"kvm_on\":";
    j += g_config->deskflowEnabled() ? "true" : "false";
    j += ",\"kvm_host\":\"";
    appendJsonEscaped(j, g_config->deskflowHost());
    j += "\",\"kvm_port\":";
    j += g_config->deskflowPort();
    j += ",\"kvm_screen\":\"";
    appendJsonEscaped(j, g_config->deskflowScreen());
    j += "\",\"kvm_w\":";
    j += g_config->deskflowWidth();
    j += ",\"kvm_h\":";
    j += g_config->deskflowHeight();
    j += ",\"kvm_tls\":";
    j += g_config->deskflowTls() ? "true" : "false";
    j += ",\"kvm_ca\":\"";
    // Copy under the config's lock rather than reading the live pointer: the
    // Deskflow task can free/replace the cert buffer mid-read.
    char *cert = static_cast<char *>(malloc(4096));
    if (cert != nullptr) {
        g_config->copyServerCert(cert, 4096);
        appendJsonEscaped(j, cert);
        free(cert);
    }
    j += "\"}";
    return j;
}

void onOtaBody(AsyncWebServerRequest *request, uint8_t *data, size_t len,
               size_t index, size_t total) {
    // Authorise before ANY side effect. The contention prologue - which
    // disconnects the screen client and blocks the async task - used to run
    // before this check, so an unauthenticated peer could drop the session and
    // stall the web server just by POSTing. This endpoint installs code that
    // types into the target; it must do nothing without the token.
    if (!otaAuthorized(request)) {
        otaFail(request, 401, "unauthorized");
        return;
    }
    OtaState *st = otaStateOf(request);

    if (index == 0) {
        // Only one upload may touch the Update writer at a time. A second
        // concurrent POST is refused here WITHOUT disturbing the in-flight one's
        // state or the writer (H5) - the request never becomes the owner.
        if (g_otaOwner != nullptr && g_otaOwner != request) {
            otaFail(request, 409, "another firmware update is already in progress");
            return;
        }
        g_otaOwner = request;

        // Refuse rather than compete. A TLS screen session holds ~34KB and an
        // update needs a large contiguous buffer; attempting both at once took
        // a device down mid-use. `force` accepts the trade explicitly, and then
        // we free the memory ourselves rather than hoping.
        if (otaContended() && !request->hasParam("force")) {
            otaFail(request, 409,
                    "the screen client is connected and an update needs the memory "
                    "it is holding - retry with ?force=1 to disconnect it first");
            g_otaOwner = nullptr;      // never started; free the slot for a retry
            return;
        }
        if (otaContended()) {
            Serial.println("[ota] disconnecting the screen client to free memory");
            g_deskflow->suspend();      // raises a flag; its own task tears down
            // Wait for that task to release the session and return its memory,
            // rather than a blind delay. Bounded so the async task cannot hang.
            const uint32_t until = millis() + 1500;
            while (g_deskflow->busy() && millis() < until) delay(10);
        }
    }

    // Only the owner proceeds past here. A body from a request we refused (its
    // reply is already sent) is ignored, so it can't touch the writer.
    if (request != g_otaOwner) return;
    if (!st || st->failed) return;

    if (index == 0) {

        // An ESP32 application image starts with magic 0xE9. Catching this
        // here gives a useful message instead of a failed flash, and it is the
        // exact mistake people make: ghosthid-merged.bin starts with 0xFF
        // bootloader padding and is for USB flashing only.
        if (len > 0 && data[0] != 0xE9) {
            otaFail(request, 400,
                    "not an ESP32 app image - upload firmware.bin, not ghosthid-merged.bin");
            return;
        }

        // Stop driving the target before rewriting our own flash.
        g_processor->setLocked(true, "firmware update in progress");

        // An upload that died mid-flight - a dropped connection, a client that
        // gave up - leaves Update running, and every later attempt then fails
        // with "could not start" until the device is rebooted. Clear it.
        if (Update.isRunning()) {
            Serial.println("[ota] a previous update was left incomplete; aborting it");
            Update.abort();
        }
        if (!Update.begin(total > 0 ? total : UPDATE_SIZE_UNKNOWN)) {
            char why[96];
            snprintf(why, sizeof(why),
                     "could not start update: %s", Update.errorString());
            otaFail(request, 400, why);
            return;
        }
        st->begun = true;
        Serial.printf("[ota] receiving %u bytes\r\n", static_cast<unsigned>(total));
    }

    if (!st->begun) return;

    if (Update.write(data, len) != len) {
        otaFail(request, 500, "flash write failed");
        return;
    }

    if (total > 0 && index + len >= total) {
        if (!Update.end(true)) {
            otaFail(request, 400, "image failed validation");
            return;
        }
        Serial.println("[ota] image verified, rebooting");
    }
}

void onOtaDone(AsyncWebServerRequest *request) {
    OtaState *st = otaStateOf(request);

    // A request that never became the OTA owner: either we refused it in the
    // body handler (already replied) or it carried no body at all (an empty POST
    // that never reached the body handler). Answer the latter; never touch the
    // owner's state or the Update writer.
    if (request != g_otaOwner) {
        if (st && !st->replied) {
            st->replied = true;
            if (!otaAuthorized(request))
                request->send(401, "application/json",
                              "{\"type\":\"error\",\"error\":\"unauthorized\"}");
            else
                request->send(400, "application/json",
                              "{\"type\":\"error\",\"error\":\"no firmware received\"}");
        }
        return;
    }

    // Owner path: finish this upload and release the writer.
    if (st->failed) {
        if (st->begun) Update.abort();
        g_processor->setLocked(false, nullptr);
        Serial.printf("[ota] failed: %s\r\n", st->error);
        if (!st->replied) {
            st->replied = true;
            request->send(400, "application/json",
                          String("{\"type\":\"error\",\"error\":\"") + st->error + "\"}");
        }
        g_otaOwner = nullptr;
        return;
    }
    // An empty or bodyless POST from the owner never wrote anything; don't reboot.
    if (!st->begun) {
        request->send(400, "application/json",
                      "{\"type\":\"error\",\"error\":\"no firmware received\"}");
        g_otaOwner = nullptr;
        return;
    }
    request->send(200, "application/json",
                  "{\"type\":\"ota_ok\",\"rebooting\":true}");
    g_otaOwner = nullptr;
    g_processor->requestReboot();
}

}  // namespace

void Network::attachDeskflow(DeskflowClient *c) { g_deskflow = c; }

void Network::beginRadio() {
    g_network   = this;
    g_processor = &processor_;
    g_config    = &config_;

    const bool wantStation = config_.stationConfigured();

    // AP_STA keeps our own access point alive while also joining an existing
    // network, so the device never becomes unreachable just because the
    // infrastructure network is down or absent.
    WiFi.mode(wantStation ? WIFI_AP_STA : WIFI_AP);

    // Modem sleep is the single biggest source of input lag. With it enabled
    // the radio only wakes on DTIM beacons, so a keystroke can sit waiting
    // ~100ms for the next wake - measured here as 6ms best case against a
    // 154ms worst case on the same link. An input device cannot afford that.
    // Costs steady-state current, which is acceptable on USB power.
    WiFi.setSleep(false);

    char suffix[5];
    deviceSuffix(suffix);
    // The configured device name drives BOTH the AP SSID and the mDNS
    // hostname. Having `name` change one but not the other was just
    // confusing. The MAC-derived suffix keeps two devices distinguishable.
    snprintf(ssid_, sizeof(ssid_), "%s-%s", config_.deviceName(), suffix);

    startAp();

    if (wantStation) {
        WiFi.begin(config_.staSsid(), config_.staPassword());
        const uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED &&
               (millis() - start) < GHOSTHID_STA_TIMEOUT_MS) {
            delay(100);
        }
        if (WiFi.status() == WL_CONNECTED) {
            snprintf(staIp_, sizeof(staIp_), "%s", WiFi.localIP().toString().c_str());
            Serial.printf("[wifi] STA %s : up (%s)\r\n", config_.staSsid(), staIp_);
            staStableSince_ = millis();
        } else {
            // Not fatal: the AP above is already serving.
            staIp_[0] = '\0';
            Serial.printf("[wifi] STA %s : failed, AP still available\r\n",
                          config_.staSsid());
        }
    }

    char host[32];
    snprintf(host, sizeof(host), "%s-%s", config_.deviceName(), suffix);
    if (MDNS.begin(host)) {
        MDNS.addService("ghosthid", "tcp", 80);
        Serial.printf("[mdns] http://%s.local/\r\n", host);
    }

}

void Network::beginServers() {
    // L4: idempotent. `web off` then `web on` from the serial console used to
    // re-register every route and stack duplicate handlers.
    if (serversUp_) return;
    // Serve the control UI straight out of flash, pre-gzipped. The AP has no
    // route to the internet, so the page cannot reference any external asset.
    g_server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        AsyncWebServerResponse *res = request->beginResponse_P(
            200, "text/html", kIndexHtmlGz, kIndexHtmlGzLen);
        res->addHeader("Content-Encoding", "gzip");
        request->send(res);
    });

    // Firmware update. Raw body, not multipart, so a plain
    //   curl --data-binary @firmware.bin
    // works as well as the browser does.
    g_server.on("/api/ota", HTTP_POST, onOtaDone, nullptr, onOtaBody);

    // Pre-flight: what is currently using memory, so a client can warn before
    // sending 800KB rather than after.
    g_server.on("/api/ota", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (!otaAuthorized(request)) {
            request->send(401, "application/json",
                          "{\"type\":\"error\",\"error\":\"unauthorized\"}");
            return;
        }
        char body[220];
        snprintf(body, sizeof(body),
                 "{\"type\":\"ota_preflight\",\"screen_client\":%s,"
                 "\"free\":%u,\"largest\":%u,\"safe\":%s}",
                 otaContended() ? "true" : "false",
                 (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
                 otaContended() ? "false" : "true");
        request->send(200, "application/json", body);
    });

    // The device's TLS certificate, for inspection or for a server that wants
    // it in advance. Token-gated: it is not secret, but it identifies the
    // device and there is no reason to hand it to anyone who asks.
    g_server.on("/api/identity", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (!otaAuthorized(request)) {
            request->send(401, "application/json",
                          "{\"type\":\"error\",\"error\":\"unauthorized\"}");
            return;
        }
        if (g_deskflow == nullptr ||
            !g_deskflow->ensureIdentity(g_config->deskflowScreen())) {
            request->send(503, "text/plain", "no identity available");
            return;
        }
        request->send(200, "application/x-pem-file", g_deskflow->certificatePem());
    });

    // Config export for provisioning another board. Token-gated (same gate as
    // OTA/identity) because it exposes the SSID and screen-client settings; it
    // never exposes a password or the auth token.
    g_server.on("/api/export", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (!otaAuthorized(request)) {
            request->send(401, "application/json",
                          "{\"type\":\"error\",\"error\":\"unauthorized\"}");
            return;
        }
        AsyncWebServerResponse *res =
            request->beginResponse(200, "application/json", buildExportJson());
        res->addHeader("Content-Disposition",
                       "attachment; filename=\"ghosthid-config.json\"");
        request->send(res);
    });

    g_server.onNotFound([](AsyncWebServerRequest *request) {
        request->redirect("/");
    });

    // Reject the WebSocket handshake from a foreign browser origin (H1). A
    // native client sends no Origin and is allowed; the device's own UI matches.
    g_ws.handleHandshake([](AsyncWebServerRequest *r) {
        return originAllowed(r) && hostAllowed(r);
    });
    g_ws.onEvent(onWsEvent);
    g_server.addHandler(&g_ws);
    g_server.begin();

    serversUp_ = true;
    Serial.printf("[web] http://%s/\r\n", apIp_);
    if (stationConnected()) Serial.printf("[web] http://%s/\r\n", staIp_);
}

void Network::startAp() {
    if (apActive_) return;
    // WPA2, not an open AP. On an open network every keystroke crosses the air
    // in cleartext to anyone in range, and the app-layer token is replayable.
    const bool ok = WiFi.softAP(ssid_, config_.apPassword());
    snprintf(apIp_, sizeof(apIp_), "%s", WiFi.softAPIP().toString().c_str());
    apActive_ = ok;
    Serial.printf("[wifi] AP  %s : %s (%s)\r\n", ssid_, ok ? "up" : "FAILED", apIp_);
}

void Network::stopAp() {
    if (!apActive_) return;
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    apActive_ = false;
    apIp_[0] = '\0';
    Serial.println("[wifi] AP  down - station is up, radio is now dedicated to it");
}

void Network::serviceRadio() {
    // Nothing to manage when there is no station to fall back from, or when
    // the operator has asked for the AP to stay up permanently.
    if (!config_.stationConfigured() || config_.apAlways()) return;

    const bool staUp = (WiFi.status() == WL_CONNECTED);
    const uint32_t now = millis();

    if (staUp) {
        if (staStableSince_ == 0) staStableSince_ = now;
        // Wait before dropping the AP so a flapping station connection does not
        // make the radio thrash between the two.
        if (apActive_ && (now - staStableSince_) > 8000) {
            stopAp();
        }
        if (staIp_[0] == '\0') {
            snprintf(staIp_, sizeof(staIp_), "%s", WiFi.localIP().toString().c_str());
        }
    } else {
        staStableSince_ = 0;
        staIp_[0] = '\0';
        // The station is gone. Bring the AP back so the device stays reachable
        // - this is the whole reason the AP exists.
        if (!apActive_) {
            Serial.println("[wifi] STA lost - raising the AP again");
            WiFi.mode(WIFI_AP_STA);
            startAp();
        }
    }
}

void Network::stopServers() {
    if (!serversUp_) return;
    g_ws.closeAll();
    g_ws.cleanupClients();
    g_server.end();
    // L4: end() stops the listener but does NOT clear the handler list, so a
    // later beginServers() would re-register every route on top of the old ones
    // (web off -> web on stacked duplicates). reset() clears them so the next
    // begin re-registers cleanly.
    g_server.reset();
    serversUp_ = false;
    for (auto &c : controllers_) c = Controller{};
    controllerCount_ = 0;
    Serial.printf("[web] stopped; heap now %u free, %u largest\r\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
}

bool Network::acquireClientSlot(uint32_t clientId) {
    if (isClientConnected(clientId)) return true;      // already holding a slot
    for (auto &c : controllers_) {
        if (c.id == 0) {
            c.id = clientId;
            c.since = millis();
            c.authTimedOut = false;
            ++controllerCount_;
            return true;
        }
    }
    return false;   // every slot full
}

void Network::releaseClientSlot(uint32_t clientId) {
    for (auto &c : controllers_) {
        if (c.id == clientId && clientId != 0) {
            c = Controller{};
            if (controllerCount_ > 0) --controllerCount_;
            return;
        }
    }
}

bool Network::isClientConnected(uint32_t clientId) const {
    if (clientId == 0) return false;
    for (const auto &c : controllers_) {
        if (c.id == clientId) return true;
    }
    return false;
}

void Network::loop() {
    g_ws.cleanupClients();

    // Drop any controller that has not authenticated within 5s (H4), so an
    // attacker cannot squat a slot without the token - each is timed out on its
    // own so one silent client never denies the others.
    for (auto &c : controllers_) {
        if (c.id != 0 && !c.authTimedOut && !processor_.authenticated(c.id) &&
            millis() - c.since > 5000) {
            AsyncWebSocketClient *sock = g_ws.client(c.id);
            if (sock) sock->close(1008, "auth timeout");
            c.authTimedOut = true;   // issue the close once, not every loop
        }
    }

    // Cheap, but there is no reason to re-evaluate the radio every few ms.
    static uint32_t last = 0;
    if (millis() - last > 1000) { last = millis(); serviceRadio(); }
}

}  // namespace ghosthid
