#include "Network.h"

#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <Update.h>

#include "board_config.h"
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
            g_processor->beginSession();
            Serial.printf("[ws] client %u connected from %s\n",
                          client->id(), client->remoteIP().toString().c_str());
            break;

        case WS_EVT_DISCONNECT:
        case WS_EVT_ERROR:
            g_network->releaseClientSlot();
            // Immediate release on a clean close; the watchdog only has to
            // cover abrupt link loss, where no event ever arrives.
            g_processor->endSession();
            Serial.printf("[ws] client %u disconnected - all input released\n",
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

void onOtaBody(AsyncWebServerRequest *request, uint8_t *data, size_t len,
               size_t index, size_t total) {
    if (!otaAuthorized(request)) return;    // the completion handler 401s

    if (index == 0) {
        g_otaError = nullptr;
        g_otaBegun = false;

        // An ESP32 application image starts with magic 0xE9. Catching this
        // here gives a useful message instead of a failed flash, and it is the
        // exact mistake people make: ghosthid-merged.bin starts with 0xFF
        // bootloader padding and is for USB flashing only.
        if (len > 0 && data[0] != 0xE9) {
            g_otaError = "not an ESP32 app image - upload firmware.bin, not ghosthid-merged.bin";
            return;
        }

        // Stop driving the target before rewriting our own flash.
        g_processor->setLocked(true, "firmware update in progress");

        if (!Update.begin(total > 0 ? total : UPDATE_SIZE_UNKNOWN)) {
            g_otaError = "could not start update (image too large?)";
            return;
        }
        g_otaBegun = true;
        Serial.printf("[ota] receiving %u bytes\n", static_cast<unsigned>(total));
    }

    if (g_otaError != nullptr || !g_otaBegun) return;

    if (Update.write(data, len) != len) {
        g_otaError = "write failed";
        return;
    }

    if (total > 0 && index + len >= total) {
        if (!Update.end(true)) {
            g_otaError = "image failed validation";
            return;
        }
        Serial.println("[ota] image verified, rebooting");
    }
}

void onOtaDone(AsyncWebServerRequest *request) {
    if (!otaAuthorized(request)) {
        request->send(401, "application/json",
                      "{\"type\":\"error\",\"error\":\"unauthorized\"}");
        return;
    }
    if (g_otaError != nullptr) {
        if (g_otaBegun) Update.abort();
        g_processor->setLocked(false, nullptr);
        Serial.printf("[ota] failed: %s\n", g_otaError);
        AsyncWebServerResponse *r = request->beginResponse(
            400, "application/json",
            String("{\"type\":\"error\",\"error\":\"") + g_otaError + "\"}");
        request->send(r);
        g_otaError = nullptr;
        g_otaBegun = false;
        return;
    }
    request->send(200, "application/json",
                  "{\"type\":\"ota_ok\",\"rebooting\":true}");
    g_processor->requestReboot();
}

}  // namespace

void Network::begin() {
    g_network   = this;
    g_processor = &processor_;
    g_config    = &config_;

    const bool wantStation = config_.stationConfigured();

    // AP_STA keeps our own access point alive while also joining an existing
    // network, so the device never becomes unreachable just because the
    // infrastructure network is down or absent.
    WiFi.mode(wantStation ? WIFI_AP_STA : WIFI_AP);

    char suffix[5];
    deviceSuffix(suffix);
    snprintf(ssid_, sizeof(ssid_), "%s-%s", GHOSTHID_AP_SSID_PREFIX, suffix);

    // WPA2, not an open AP. On an open network every keystroke crosses the air
    // in cleartext to anyone in range, and the app-layer token is replayable.
    const bool apOk = WiFi.softAP(ssid_, config_.apPassword());
    snprintf(apIp_, sizeof(apIp_), "%s", WiFi.softAPIP().toString().c_str());
    Serial.printf("[wifi] AP  %s : %s (%s)\n", ssid_, apOk ? "up" : "FAILED", apIp_);

    if (wantStation) {
        WiFi.begin(config_.staSsid(), config_.staPassword());
        const uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED &&
               (millis() - start) < GHOSTHID_STA_TIMEOUT_MS) {
            delay(100);
        }
        if (WiFi.status() == WL_CONNECTED) {
            snprintf(staIp_, sizeof(staIp_), "%s", WiFi.localIP().toString().c_str());
            Serial.printf("[wifi] STA %s : up (%s)\n", config_.staSsid(), staIp_);
        } else {
            // Not fatal: the AP above is already serving.
            staIp_[0] = '\0';
            Serial.printf("[wifi] STA %s : failed, AP still available\n",
                          config_.staSsid());
        }
    }

    char host[32];
    snprintf(host, sizeof(host), "%s-%s", config_.deviceName(), suffix);
    if (MDNS.begin(host)) {
        MDNS.addService("ghosthid", "tcp", 80);
        Serial.printf("[mdns] http://%s.local/\n", host);
    }

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

    g_server.onNotFound([](AsyncWebServerRequest *request) {
        request->redirect("/");
    });

    g_ws.onEvent(onWsEvent);
    g_server.addHandler(&g_ws);
    g_server.begin();

    Serial.printf("[web] http://%s/\n", apIp_);
    if (stationConnected()) Serial.printf("[web] http://%s/\n", staIp_);
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
}

}  // namespace ghosthid
