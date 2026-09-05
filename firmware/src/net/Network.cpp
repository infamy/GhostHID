#include "Network.h"

#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <Update.h>

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
    uint8_t mac[6] = {};
    WiFi.macAddress(mac);
    snprintf(out, 5, "%02X%02X", mac[4], mac[5]);
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
               AwsEventType type, void *arg, uint8_t *data, size_t len) {
    switch (type) {
        case WS_EVT_CONNECT:
            // One controller at a time. A second connection would let two
            // peers fight over the same held-key state.
            if (!g_network->acquireClientSlot()) {
                client->close(1013, "busy");
                return;
            }
            // Nagle batches small writes, which is exactly wrong for a stream
            // of tiny input events; without this each report can wait for an
            // ACK or a 40ms coalescing timer.
            client->client()->setNoDelay(true);
            g_processor->beginSession();
            Serial.printf("[ws] client %u connected from %s\r\n",
                          client->id(), client->remoteIP().toString().c_str());
            break;

        case WS_EVT_DISCONNECT:
        case WS_EVT_ERROR:
            g_network->releaseClientSlot();
            // Immediate release on a clean close; the watchdog only has to
            // cover abrupt link loss, where no event ever arrives.
            g_processor->endSession();
            Serial.printf("[ws] client %u disconnected - all input released\r\n",
                          client->id());
            break;

        case WS_EVT_DATA: {
            AwsFrameInfo *info = static_cast<AwsFrameInfo *>(arg);
            if (info->opcode != WS_TEXT || !info->final || info->index != 0 ||
                info->len != len) {
                return;  // ignore fragmented/binary frames for now
            }
            char response[512];
            g_processor->handleMessage(reinterpret_cast<const char *>(data), len,
                                       response, sizeof(response));
            // Only reply when the processor produced something: key and mouse
            // events are fire-and-forget, and echoing every one of them would
            // add latency to the path we most care about.
            if (response[0] != '\0') client->text(response);
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

const char *g_otaError = nullptr;
bool        g_otaBegun = false;
bool        g_otaReplied = false;   // a response was already sent from onOtaBody

// Ends the request from inside the body handler. Without this we would keep
// accepting an 800KB upload we have already decided to reject, and the client
// sees a reset connection rather than the reason.
void otaFail(AsyncWebServerRequest *request, int code, const char *why) {
    g_otaError = why;
    if (g_otaReplied) return;
    g_otaReplied = true;
    request->send(code, "application/json",
                  String("{\"type\":\"error\",\"error\":\"") + why + "\"}");
}

// This endpoint installs arbitrary code on a device that types into someone's
// computer. It must never be reachable without the token.
bool otaAuthorized(AsyncWebServerRequest *request) {
    const char *tok = g_config->authToken();
    if (tok == nullptr || tok[0] == '\0') return true;   // auth disabled
    if (request->hasHeader("X-GhostHID-Token")) {
        return request->getHeader("X-GhostHID-Token")->value() == tok;
    }
    if (request->hasParam("token")) {
        return request->getParam("token")->value() == tok;
    }
    return false;
}

// True when something else is using enough memory that an update is risky.
bool otaContended() {
    return g_deskflow != nullptr && g_deskflow->connected();
}

void onOtaBody(AsyncWebServerRequest *request, uint8_t *data, size_t len,
               size_t index, size_t total) {
    if (index == 0) {
        g_otaError = nullptr;
        g_otaBegun = false;
        g_otaReplied = false;

        // Refuse rather than compete. A TLS screen session holds ~34KB and an
        // update needs a large contiguous buffer; attempting both at once took
        // a device down mid-use. `force` accepts the trade explicitly, and then
        // we free the memory ourselves rather than hoping.
        if (otaContended() && !request->hasParam("force")) {
            otaFail(request, 409,
                    "the screen client is connected and an update needs the memory "
                    "it is holding - retry with ?force=1 to disconnect it first");
            return;
        }
        if (otaContended()) {
            Serial.println("[ota] disconnecting the screen client to free memory");
            g_deskflow->suspend();
            delay(150);                 // let the socket close and buffers return
        }
    }

    if (!otaAuthorized(request)) {
        otaFail(request, 401, "unauthorized");
        return;
    }
    if (g_otaError != nullptr) return;   // already rejected; ignore the rest

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
        g_otaBegun = true;
        Serial.printf("[ota] receiving %u bytes\r\n", static_cast<unsigned>(total));
    }

    if (!g_otaBegun) return;

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
    if (g_otaError != nullptr) {
        if (g_otaBegun) Update.abort();
        g_processor->setLocked(false, nullptr);
        Serial.printf("[ota] failed: %s\r\n", g_otaError);
        if (!g_otaReplied) {
            g_otaReplied = true;
            request->send(400, "application/json",
                          String("{\"type\":\"error\",\"error\":\"") + g_otaError + "\"}");
        }
        g_otaError = nullptr;
        g_otaBegun = false;
        return;
    }
    if (!otaAuthorized(request)) {
        request->send(401, "application/json",
                      "{\"type\":\"error\",\"error\":\"unauthorized\"}");
        return;
    }
    request->send(200, "application/json",
                  "{\"type\":\"ota_ok\",\"rebooting\":true}");
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

    g_server.onNotFound([](AsyncWebServerRequest *request) {
        request->redirect("/");
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
    serversUp_ = false;
    clientCount_ = 0;
    Serial.printf("[web] stopped; heap now %u free, %u largest\r\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
}

bool Network::acquireClientSlot() {
    if (clientCount_ > 0) return false;
    clientCount_++;
    return true;
}

void Network::releaseClientSlot() {
    if (clientCount_ > 0) clientCount_--;
}

void Network::loop() {
    g_ws.cleanupClients();

    // Cheap, but there is no reason to re-evaluate the radio every few ms.
    static uint32_t last = 0;
    if (millis() - last > 1000) { last = millis(); serviceRadio(); }
}

}  // namespace ghosthid
