// A tiny line-based console on the USB serial port.
//
// Exists so a fresh device can be configured over the cable you already used
// to flash it, without the chicken-and-egg of "join the access point in order
// to configure the access point".
//
// Non-blocking: feed() consumes whatever bytes have arrived and returns.

#pragma once

#include <stddef.h>

namespace ghosthid {

class Config;
class CommandProcessor;
class Network;
class DeskflowClient;

class SerialConsole {
public:
    SerialConsole(Config &config, CommandProcessor &processor, Network &network,
                  DeskflowClient &deskflow)
        : config_(config), processor_(processor), network_(network), deskflow_(deskflow) {}

    // Prints the banner. Call once after Serial is up.
    void begin();

    // Call from loop(). Reads any pending input and executes complete lines.
    void feed();

private:
    void execute(char *line);
    void printHelp() const;
    void printStatus() const;

    static constexpr size_t kLineMax = 160;

    Config           &config_;
    CommandProcessor &processor_;
    Network          &network_;
    DeskflowClient   &deskflow_;
    char   line_[kLineMax] = {};
    size_t len_ = 0;
};

}  // namespace ghosthid
