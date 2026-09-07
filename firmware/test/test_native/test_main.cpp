// Host unit tests for CommandProcessor — the safety-critical logic layer.
// Runs on the dev machine (platform = native), no board, via `pio test -e native`.
// HidDevice is a recording stub; Config/Keymap are the real code compiled against
// a small shim layer. No shipped firmware code is modified to make this build.
#include <unity.h>
#include <Arduino.h>
#include <string.h>

#include "protocol/CommandProcessor.h"
#include "hid/HidDevice.h"
#include "config/Config.h"
#include "test_hooks.h"

using namespace ghosthid;

// Definition of the shim's test-controllable clock.
uint32_t g_test_millis = 1000;

static char buf[1200];

static void send(CommandProcessor &p, uint32_t cid, const char *json) {
    p.handleMessage(cid, json, strlen(json), buf, sizeof(buf));
}
static bool replied(const char *needle) { return strstr(buf, needle) != nullptr; }

// Build a processor with a known token set. Objects are per-test (fresh auth
// counters); the caller keeps them alive for the test body.
#define SETUP_PROC()                              \
    HidDevice hid;                                \
    Config cfg;                                   \
    cfg.begin();                                  \
    cfg.setAuthToken("TESTTOKEN");                \
    CommandProcessor p(hid, cfg)

void setUp() { hidhook::reset(); g_test_millis = 1000; }
void tearDown() {}

// 1. Auth: a wrong token is refused, the right token is accepted.
void test_auth_wrong_then_right() {
    SETUP_PROC();
    p.beginSession(1);
    send(p, 1, "{\"type\":\"auth\",\"token\":\"WRONG\"}");
    TEST_ASSERT_TRUE(replied("\"ok\":false"));
    send(p, 1, "{\"type\":\"auth\",\"token\":\"TESTTOKEN\"}");
    TEST_ASSERT_TRUE(replied("\"ok\":true"));
}

// 2. The auth gate: input before authenticating never reaches the HID layer.
void test_unauthenticated_input_blocked() {
    SETUP_PROC();
    p.beginSession(1);
    send(p, 1, "{\"type\":\"key\",\"key\":\"ENTER\",\"pressed\":true}");
    TEST_ASSERT_TRUE(replied("unauthenticated"));
    TEST_ASSERT_EQUAL_INT(0, hidhook::keyDownCalls);
}

// 3. Once authenticated, a key reaches the HID layer.
void test_authenticated_key_reaches_hid() {
    SETUP_PROC();
    p.beginSession(1);
    send(p, 1, "{\"type\":\"auth\",\"token\":\"TESTTOKEN\"}");
    send(p, 1, "{\"type\":\"key\",\"key\":\"ENTER\",\"pressed\":true}");
    TEST_ASSERT_EQUAL_INT(1, hidhook::keyDownCalls);
    TEST_ASSERT_TRUE(hid.anythingHeld());
}

// 4. H8 regression: a controller that disconnects while holding a key must not
//    leave it stuck — endSession releases held input.
void test_h8_release_held_input_on_disconnect() {
    SETUP_PROC();
    p.beginSession(1);
    send(p, 1, "{\"type\":\"auth\",\"token\":\"TESTTOKEN\"}");
    send(p, 1, "{\"type\":\"key\",\"key\":\"ENTER\",\"pressed\":true}");
    TEST_ASSERT_TRUE(hid.anythingHeld());
    const int before = hidhook::releaseAllCalls;
    p.endSession(1);
    TEST_ASSERT_TRUE(hidhook::releaseAllCalls > before);
    TEST_ASSERT_FALSE(hid.anythingHeld());
}

// 5. Multi-controller: auth is per client — one client's login must not
//    authorise another.
void test_per_client_auth_isolation() {
    SETUP_PROC();
    p.beginSession(1);
    p.beginSession(2);
    send(p, 1, "{\"type\":\"auth\",\"token\":\"TESTTOKEN\"}");   // client 1 in
    send(p, 2, "{\"type\":\"key\",\"key\":\"ENTER\",\"pressed\":true}");  // client 2 not
    TEST_ASSERT_TRUE(replied("unauthenticated"));
    const int before = hidhook::keyDownCalls;
    send(p, 1, "{\"type\":\"key\",\"key\":\"ENTER\",\"pressed\":true}");  // client 1 ok
    TEST_ASSERT_TRUE(hidhook::keyDownCalls > before);
}

// 6. Rate-limit: 3 wrong tokens lock out even the correct token; the lockout is
//    reported distinctly (locked); and it decays after a quiet period.
void test_rate_limit_lockout_and_decay() {
    SETUP_PROC();
    p.beginSession(1);
    for (int i = 0; i < 3; ++i) send(p, 1, "{\"type\":\"auth\",\"token\":\"WRONG\"}");
    // Correct token now, but inside the cooldown -> refused as locked, not "bad token".
    send(p, 1, "{\"type\":\"auth\",\"token\":\"TESTTOKEN\"}");
    TEST_ASSERT_TRUE(replied("\"locked\":true"));
    // Quiet for >2 min -> counters decay -> the correct token works again.
    g_test_millis += 130000;
    send(p, 1, "{\"type\":\"auth\",\"token\":\"TESTTOKEN\"}");
    TEST_ASSERT_TRUE(replied("\"ok\":true"));
}

// 7. M4/M5: mouse_abs rejects non-finite coordinates instead of casting UB.
void test_mouse_abs_rejects_non_finite() {
    SETUP_PROC();
    p.beginSession(1);
    send(p, 1, "{\"type\":\"auth\",\"token\":\"TESTTOKEN\"}");
    send(p, 1, "{\"type\":\"mouse_abs\",\"x\":1e400,\"y\":0.5}");   // 1e400 -> inf
    TEST_ASSERT_TRUE(replied("finite"));
    TEST_ASSERT_EQUAL_INT(0, hidhook::mouseAbsCalls);
    send(p, 1, "{\"type\":\"mouse_abs\",\"x\":0.5,\"y\":0.5}");     // valid
    TEST_ASSERT_EQUAL_INT(1, hidhook::mouseAbsCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_auth_wrong_then_right);
    RUN_TEST(test_unauthenticated_input_blocked);
    RUN_TEST(test_authenticated_key_reaches_hid);
    RUN_TEST(test_h8_release_held_input_on_disconnect);
    RUN_TEST(test_per_client_auth_isolation);
    RUN_TEST(test_rate_limit_lockout_and_decay);
    RUN_TEST(test_mouse_abs_rejects_non_finite);
    return UNITY_END();
}
