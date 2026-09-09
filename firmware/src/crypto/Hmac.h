// Self-contained SHA-256 + HMAC-SHA-256.
//
// Deliberately NOT mbedtls: this is used by CommandProcessor's challenge-response
// auth, and CommandProcessor is compiled in the native (host) test env, which
// shims out TLS/mbedtls entirely. A small standalone implementation lets the same
// code run - and be unit-tested against known vectors - on both the device and the
// host, with no crypto-library dependency. It is used only for authentication
// proofs (short, infrequent), so raw throughput is irrelevant.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ghosthid {

// Raw SHA-256 of `len` bytes -> 32-byte digest.
void sha256(const uint8_t *data, size_t len, uint8_t out[32]);

// HMAC-SHA-256(key, msg) -> 32-byte MAC.
void hmacSha256(const uint8_t *key, size_t keyLen,
                const uint8_t *msg, size_t msgLen, uint8_t out[32]);

// Convenience: HMAC-SHA-256 over C-string key/msg, result written as 64 lower-case
// hex chars + NUL into `outHex` (must be >= 65 bytes).
void hmacSha256Hex(const char *key, const char *msg, char *outHex, size_t cap);

// Same, over the concatenation msg1 || msg2 - so a MAC over "counter:message" can
// be computed without allocating a buffer to hold the whole thing.
void hmacSha256Hex2(const char *key, const char *msg1, const char *msg2,
                    char *outHex, size_t cap);

}  // namespace ghosthid
