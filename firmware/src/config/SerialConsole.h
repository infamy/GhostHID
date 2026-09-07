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

    // Prints the banner. Call once after Serial is up. No-op while sealed (the
    // console is not part of the USB descriptor then).
    void begin();

    // Call from loop(). Reads any pending input and executes complete lines.
    void feed();

    // Called by main() when the BOOT button is held long enough (~5s) on a
    // SEALED device. That hold is the physical factor: it re-enables the USB
    // serial console (main brings Serial back), and this arms the one command -
    // `unseal` - that a sealed device will otherwise refuse. Unsealing still
    // needs the token (`unlock`), so the two factors are physical + token.
    void armUnseal();
    bool unsealArmed() const { return unsealArmed_; }

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
    // Per-boot flag: `unlock <token>` sets it, gating the commands that mutate
    // an already-set security setting so a compromised target host can't
    // reconfigure the device across the USB console (M1). Reset every boot.
    bool   consoleUnlocked_ = false;
    // Set by armUnseal() after a physical BOOT hold on a sealed device. Gates the
    // `unseal` command so the token alone (over a re-enabled console) can't undo
    // the seal without someone physically at the board. Reset every boot.
    bool   unsealArmed_ = false;
};

}  // namespace ghosthid
