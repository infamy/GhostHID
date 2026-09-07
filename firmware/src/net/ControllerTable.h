// Fixed-capacity table of connected web controllers.
//
// Pure bookkeeping — no sockets, no I/O — so it can be unit-tested off-device.
// Network owns one; the WebSocket connect/disconnect callbacks and the
// auth-timeout sweep all go through it. Pulling it out of Network (which drags
// the whole AsyncWebServer/Wi-Fi stack) is what makes the slot logic testable.

#pragma once

#include <stdint.h>
#include <stddef.h>

#include "board_config.h"   // GHOSTHID_MAX_CONTROLLERS

namespace ghosthid {

class ControllerTable {
public:
    struct Entry {
        uint32_t id = 0;            // 0 = free slot
        uint32_t since = 0;         // millis() at connect (for the auth timeout)
        bool     authTimedOut = false;  // auth-timeout close already issued
    };
    static constexpr size_t kCapacity = GHOSTHID_MAX_CONTROLLERS;

    // Record a controller, stamping `now` as its connect time. Returns false
    // only when every slot is full; a client already present is a no-op that
    // returns true (idempotent — a duplicate WS_EVT_CONNECT can't double-count).
    bool acquire(uint32_t clientId, uint32_t now) {
        if (clientId == 0) return false;
        if (contains(clientId)) return true;
        for (Entry &e : slots_) {
            if (e.id == 0) { e = Entry{clientId, now, false}; ++count_; return true; }
        }
        return false;
    }

    void release(uint32_t clientId) {
        if (clientId == 0) return;
        for (Entry &e : slots_) {
            if (e.id == clientId) { e = Entry{}; if (count_ > 0) --count_; return; }
        }
    }

    bool contains(uint32_t clientId) const {
        if (clientId == 0) return false;
        for (const Entry &e : slots_) if (e.id == clientId) return true;
        return false;
    }

    size_t count() const { return count_; }

    void clear() {
        for (Entry &e : slots_) e = Entry{};
        count_ = 0;
    }

    // Iterate all slots (including free ones, id==0) — used by the auth-timeout
    // sweep, which needs each entry's `since` / `authTimedOut`.
    Entry *begin() { return slots_; }
    Entry *end()   { return slots_ + kCapacity; }

private:
    Entry  slots_[kCapacity] = {};
    size_t count_ = 0;
};

}  // namespace ghosthid
