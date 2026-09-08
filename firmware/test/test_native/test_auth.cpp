// Challenge-response auth: the client proves it knows the token via
// HMAC-SHA256(token, nonce) without ever sending the token (H3). Legacy cleartext
// token auth is covered by test_commandprocessor.cpp; this covers the proof path.
#include "test_common.h"
#include "crypto/Hmac.h"

// Pull the "nonce":"...." value out of the last reply into `out` (>=33 bytes).
static bool extractNonce(char *out, size_t cap) {
    const char *k = strstr(g_buf, "\"nonce\":\"");
    if (!k) return false;
    k += 9;
    size_t i = 0;
    while (k[i] && k[i] != '"' && i + 1 < cap) { out[i] = k[i]; ++i; }
    out[i] = '\0';
    return i > 0;
}

void test_challenge_response_ok() {
    SETUP_PROC(); p.beginSession(1);
    send(p, 1, "{\"type\":\"challenge\"}");
    TEST_ASSERT_TRUE(replied("\"auth_required\":true"));
    char nonce[33];
    TEST_ASSERT_TRUE(extractNonce(nonce, sizeof(nonce)));
    TEST_ASSERT_EQUAL_INT(32, (int)strlen(nonce));

    char proof[65];
    ghosthid::hmacSha256Hex("TESTTOKEN", nonce, proof, sizeof(proof));  // token stays local
    char msg[160];
    snprintf(msg, sizeof(msg), "{\"type\":\"auth\",\"proof\":\"%s\"}", proof);
    send(p, 1, msg);
    TEST_ASSERT_TRUE(replied("\"ok\":true"));

    // Authenticated: input now flows.
    send(p, 1, "{\"type\":\"key\",\"key\":\"ENTER\",\"pressed\":true}");
    TEST_ASSERT_EQUAL_INT(1, hidhook::keyDownCalls);
}

void test_challenge_response_wrong_proof() {
    SETUP_PROC(); p.beginSession(1);
    send(p, 1, "{\"type\":\"challenge\"}");
    char nonce[33]; extractNonce(nonce, sizeof(nonce));
    // Proof computed with the wrong token must be rejected.
    char proof[65];
    ghosthid::hmacSha256Hex("WRONGTOKEN", nonce, proof, sizeof(proof));
    char msg[160];
    snprintf(msg, sizeof(msg), "{\"type\":\"auth\",\"proof\":\"%s\"}", proof);
    send(p, 1, msg);
    TEST_ASSERT_TRUE(replied("\"ok\":false"));
}

void test_proof_without_challenge_fails() {
    SETUP_PROC(); p.beginSession(1);
    // No challenge requested -> no outstanding nonce -> any proof is rejected.
    send(p, 1, "{\"type\":\"auth\",\"proof\":\""
        "0000000000000000000000000000000000000000000000000000000000000000\"}");
    TEST_ASSERT_TRUE(replied("\"ok\":false"));
}

void test_challenge_nonce_is_one_shot() {
    SETUP_PROC(); p.beginSession(1);
    send(p, 1, "{\"type\":\"challenge\"}");
    char nonce[33]; extractNonce(nonce, sizeof(nonce));
    char proof[65];
    ghosthid::hmacSha256Hex("TESTTOKEN", nonce, proof, sizeof(proof));
    char msg[160];
    snprintf(msg, sizeof(msg), "{\"type\":\"auth\",\"proof\":\"%s\"}", proof);
    send(p, 1, msg);
    TEST_ASSERT_TRUE(replied("\"ok\":true"));
    // Replaying the SAME proof must fail: the nonce is consumed on use.
    send(p, 1, msg);
    TEST_ASSERT_TRUE(replied("\"ok\":false"));
}

void test_challenge_no_token_says_not_required() {
    HidDevice hid; Config cfg; cfg.begin();
    cfg.setAuthToken("");                         // disable auth (begin() provisions one)
    CommandProcessor p(hid, cfg); p.beginSession(1);
    send(p, 1, "{\"type\":\"challenge\"}");
    TEST_ASSERT_TRUE(replied("\"auth_required\":false"));
}
