// SHA-256 / HMAC-SHA-256 against published test vectors, so the challenge-response
// auth built on it is verifiably correct (and matches the browser-side JS).
#include "test_common.h"
#include "crypto/Hmac.h"

static void hex(const uint8_t *b, int n, char *out) {
    static const char *h = "0123456789abcdef";
    for (int i = 0; i < n; ++i) { out[i*2]=h[b[i]>>4]; out[i*2+1]=h[b[i]&0xf]; }
    out[n*2] = '\0';
}

void test_sha256_vectors() {
    uint8_t d[32]; char s[65];
    ghosthid::sha256((const uint8_t*)"abc", 3, d); hex(d, 32, s);
    TEST_ASSERT_EQUAL_STRING(
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", s);
    ghosthid::sha256((const uint8_t*)"", 0, d); hex(d, 32, s);
    TEST_ASSERT_EQUAL_STRING(
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", s);
}

void test_hmac_sha256_vector() {
    // RFC-style vector: HMAC-SHA256("key", "The quick brown fox...").
    char s[65];
    ghosthid::hmacSha256Hex("key", "The quick brown fox jumps over the lazy dog",
                            s, sizeof(s));
    TEST_ASSERT_EQUAL_STRING(
        "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8", s);
}

void test_hmac_long_key() {
    // Key longer than the 64-byte block must be hashed first; just exercise the
    // path and confirm determinism + 64 hex chars out.
    char a[65], b[65];
    const char *k = "0123456789012345678901234567890123456789012345678901234567890123456789";
    ghosthid::hmacSha256Hex(k, "nonce123", a, sizeof(a));
    ghosthid::hmacSha256Hex(k, "nonce123", b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING(a, b);
    TEST_ASSERT_EQUAL_INT(64, (int)strlen(a));
}
