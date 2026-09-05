// Wi-Fi access point + WebSocket transport.
//
// This is the only networking code. It owns no HID state: every message is
// handed to CommandProcessor, which is what actually talks to the HID layer.

#pragma once

#include <stdint.h>

namespace ghosthid {

class CommandProcessor;
class Config;
class DeskflowClient;

class Network {
public:
    Network(CommandProcessor &processor, Config &config)
        : processor_(processor), config_(config) {}

    // Brings up the AP (always) plus the station connection (if credentials
    // were compiled in), then starts the WebSocket server.
    // Split deliberately. Starting the web server drops the largest
    // contiguous heap block from ~135K to ~47K, and a TLS handshake needs 16K
    // in one piece plus more after it. So the radio comes up first, the screen
    // client gets its handshake in while the heap is still whole, and only then
    // does the web server start.
    void beginRadio();
    void beginServers();
    void begin() { beginRadio(); beginServers(); }
    void attachDeskflow(DeskflowClient *c);

    // Must be called from loop(): drives cleanup of dead WebSocket clients.
    void loop();

    bool clientConnected() const { return clientCount_ > 0; }
    const char *ssid() const { return ssid_; }
    const char *apAddress() const { return apIp_; }

    // Empty string when station mode is disabled or the join failed.
    const char *staAddress() const { return staIp_; }
    bool stationConnected() const { return staIp_[0] != '\0'; }

    // Called by the WebSocket event callback. Returns false if the connection
    // should be refused because a controller is already attached -- two peers
    // sharing one held-key state would fight over it.
    bool acquireClientSlot();
    void releaseClientSlot();

    bool apActive() const { return apActive_; }

private:
    // Raises or drops the access point as the station connection comes and
    // goes. Only used when the AP is in fallback mode.
    void serviceRadio();
    void startAp();
    void stopAp();

    CommandProcessor &processor_;
    Config           &config_;
    char ssid_[33]  = {};
    char apIp_[16]  = {};
    char staIp_[16] = {};
    uint32_t clientCount_ = 0;
    bool     apActive_ = false;
    uint32_t staStableSince_ = 0;
};

}  // namespace ghosthid
