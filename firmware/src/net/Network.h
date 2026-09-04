// Wi-Fi access point + WebSocket transport.
//
// This is the only networking code. It owns no HID state: every message is
// handed to CommandProcessor, which is what actually talks to the HID layer.

#pragma once

#include <stdint.h>

namespace ghosthid {

class CommandProcessor;

class Network {
public:
    explicit Network(CommandProcessor &processor) : processor_(processor) {}

    // Brings up the AP (always) plus the station connection (if credentials
    // were compiled in), then starts the WebSocket server.
    void begin();

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

private:
    CommandProcessor &processor_;
    char ssid_[33]  = {};
    char apIp_[16]  = {};
    char staIp_[16] = {};
    uint32_t clientCount_ = 0;
};

}  // namespace ghosthid
