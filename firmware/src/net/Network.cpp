#include "Network.h"

#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>

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

}  // namespace

void Network::begin() {
    g_network   = this;
    g_processor = &processor_;

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
