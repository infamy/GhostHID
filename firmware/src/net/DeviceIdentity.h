// A per-device TLS identity: an EC key pair and a self-signed certificate.
//
// Deskflow, Barrier and Synergy authenticate peers with mutual TLS and identify
// them by certificate fingerprint - a server records a client's fingerprint the
// first time it connects and serves it thereafter. So GhostHID needs a
// certificate of its own, and it must be stable: regenerating it on every boot
// would make the device a stranger to the server every time.
//
// Generated once, on first use, and kept in NVS. P-256 rather than RSA because
// RSA-2048 key generation on a 240MHz single-core S2 takes minutes, where an
// EC key takes well under a second.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ghosthid {

class DeviceIdentity {
public:
    // Loads the stored identity, generating one if there is none. Safe to call
    // repeatedly; only the first call can be slow.
    bool begin(const char *commonName);

    bool ready() const { return cert_ != nullptr && key_ != nullptr; }

    const char *certificatePem() const { return cert_; }
    const char *privateKeyPem()  const { return key_; }

    // Lower-case hex SHA-256 of the DER certificate - the same value Deskflow
    // writes to its trusted-clients file, so a user can compare them.
    const char *fingerprint() const { return fingerprint_; }

    // Discards the stored identity; a new one is generated on the next begin().
    void regenerate(const char *commonName);

private:
    bool generate(const char *commonName);
    bool computeFingerprint();

    char *cert_ = nullptr;      // PEM, heap-allocated and kept for the socket's life
    char *key_  = nullptr;      // PEM
    char  fingerprint_[65] = {};
};

}  // namespace ghosthid
