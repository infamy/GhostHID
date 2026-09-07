// Shared helpers for the host unit tests.
#pragma once
#include <unity.h>
#include <Arduino.h>
#include <string.h>

#include "protocol/CommandProcessor.h"
#include "protocol/Keymap.h"
#include "hid/HidDevice.h"
#include "config/Config.h"
#include "test_hooks.h"

using namespace ghosthid;

extern uint32_t g_test_millis;   // shim clock (defined in test_main.cpp)
extern char     g_buf[1200];     // last reply

inline void send(CommandProcessor &p, uint32_t cid, const char *json) {
    p.handleMessage(cid, json, strlen(json), g_buf, sizeof(g_buf));
}
inline bool replied(const char *needle) { return strstr(g_buf, needle) != nullptr; }

// A processor with a known token ("TESTTOKEN"); objects live for the test body.
#define SETUP_PROC()                 \
    HidDevice hid;                   \
    Config cfg;                      \
    cfg.begin();                     \
    cfg.setAuthToken("TESTTOKEN");   \
    CommandProcessor p(hid, cfg)

// Open an authenticated session for client `cid`.
#define AUTHED(cid) do {                                              \
    p.beginSession(cid);                                              \
    send(p, cid, "{\"type\":\"auth\",\"token\":\"TESTTOKEN\"}");      \
} while (0)
