// Authenticated-input mode (#1): once a session opts in at auth, every input
// command must arrive in a "secure" envelope with a monotonic counter and
// HMAC(macKey, "<c>:"+message). Forged, tampered, replayed, or plain input is
// refused. macKey = HMAC(token, nonce+"|mac") - the device's derivation.
#include "test_common.h"
#include "crypto/Hmac.h"

static bool grabNonce(char *out, size_t cap) {
    const char *k = strstr(g_buf, "\"nonce\":\"");
    if (!k) return false;
    k += 9; size_t i = 0;
    while (k[i] && k[i] != '"' && i + 1 < cap) { out[i] = k[i]; ++i; }
    out[i] = '\0'; return i > 0;
}

// Authenticate client 1 in secure mode; leaves macKey in `macKey` (>=65).
static void authSecure(CommandProcessor &p, char *macKey) {
    send(p, 1, "{\"type\":\"challenge\"}");
    char nonce[33]; grabNonce(nonce, sizeof(nonce));
    char proof[65]; ghosthid::hmacSha256Hex("TESTTOKEN", nonce, proof, sizeof(proof));
    char am[160];
    snprintf(am, sizeof(am), "{\"type\":\"auth\",\"proof\":\"%s\",\"secure\":true}", proof);
    send(p, 1, am);
    char msg[40]; snprintf(msg, sizeof(msg), "%s|mac", nonce);
    ghosthid::hmacSha256Hex("TESTTOKEN", msg, macKey, 65);
}

// Build a secure envelope for `inner` at counter `c`, MAC'd with `macKey`.
static void envelope(const char *macKey, unsigned c, const char *inner,
                     char *out, size_t cap) {
    char mi[256]; snprintf(mi, sizeof(mi), "%u:%s", c, inner);
    char mac[65]; ghosthid::hmacSha256Hex(macKey, mi, mac, sizeof(mac));
    char esc[256]; size_t o = 0;
    for (const char *q = inner; *q && o + 2 < sizeof(esc); ++q) {
        if (*q == '"' || *q == '\\') esc[o++] = '\\';
        esc[o++] = *q;
    }
    esc[o] = '\0';
    snprintf(out, cap, "{\"type\":\"secure\",\"c\":%u,\"m\":\"%s\",\"mac\":\"%s\"}",
             c, esc, mac);
}

static const char *KEY_CMD = "{\"type\":\"key\",\"key\":\"ENTER\",\"pressed\":true}";

void test_secure_negotiated() {
    SETUP_PROC(); p.beginSession(1);
    char macKey[65]; authSecure(p, macKey);
    TEST_ASSERT_TRUE(replied("\"secure\":true"));
}

void test_secure_input_accepted() {
    SETUP_PROC(); p.beginSession(1);
    char macKey[65]; authSecure(p, macKey);
    char env[400]; envelope(macKey, 1, KEY_CMD, env, sizeof(env));
    send(p, 1, env);
    TEST_ASSERT_EQUAL_INT(1, hidhook::keyDownCalls);
}

void test_secure_bad_mac_rejected() {
    SETUP_PROC(); p.beginSession(1);
    char macKey[65]; authSecure(p, macKey);
    char env[400]; envelope(macKey, 1, KEY_CMD, env, sizeof(env));
    // Corrupt the last mac hex digit.
    char *q = strrchr(env, '"'); q[-1] = (q[-1] == 'a') ? 'b' : 'a';
    send(p, 1, env);
    TEST_ASSERT_TRUE(replied("bad mac"));
    TEST_ASSERT_EQUAL_INT(0, hidhook::keyDownCalls);
}

void test_secure_replay_rejected() {
    SETUP_PROC(); p.beginSession(1);
    char macKey[65]; authSecure(p, macKey);
    char env[400]; envelope(macKey, 1, KEY_CMD, env, sizeof(env));
    send(p, 1, env);                       // counter 1 accepted
    TEST_ASSERT_EQUAL_INT(1, hidhook::keyDownCalls);
    send(p, 1, env);                       // same counter -> stale -> rejected
    TEST_ASSERT_TRUE(replied("stale counter"));
    TEST_ASSERT_EQUAL_INT(1, hidhook::keyDownCalls);   // no second keypress
}

void test_secure_counter_advances() {
    SETUP_PROC(); p.beginSession(1);
    char macKey[65]; authSecure(p, macKey);
    char e1[400]; envelope(macKey, 1, KEY_CMD, e1, sizeof(e1)); send(p, 1, e1);
    char e2[400]; envelope(macKey, 2, KEY_CMD, e2, sizeof(e2)); send(p, 1, e2);
    TEST_ASSERT_EQUAL_INT(2, hidhook::keyDownCalls);   // 1 then 2 both accepted
}

void test_plain_input_rejected_in_secure_mode() {
    SETUP_PROC(); p.beginSession(1);
    char macKey[65]; authSecure(p, macKey);
    send(p, 1, KEY_CMD);                   // plain input, no envelope
    TEST_ASSERT_TRUE(replied("authenticated channel required"));
    TEST_ASSERT_EQUAL_INT(0, hidhook::keyDownCalls);
}
