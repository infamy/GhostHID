#include "DeviceIdentity.h"

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/pem.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/x509_csr.h>
#include <mbedtls/version.h>   // MBEDTLS_VERSION_MAJOR (2.x on S2, 3.x on S3/pioarduino)

namespace ghosthid {
namespace {

constexpr char kNamespace[] = "ghostid";
constexpr char kKeyCert[]   = "cert";
constexpr char kKeyKey[]    = "key";
constexpr char kKeyVer[]    = "ver";

// Bumped when the certificate we generate changes shape. A stored identity from
// an older version is regenerated rather than kept, which matters because
// version 1 omitted the X.509 extensions below and was rejected outright.
constexpr uint32_t kIdentityVersion = 3;

Preferences g_store;

char *dupString(const String &s) {
    char *out = static_cast<char *>(malloc(s.length() + 1));
    if (out != nullptr) memcpy(out, s.c_str(), s.length() + 1);
    return out;
}

}  // namespace

bool DeviceIdentity::begin(const char *commonName) {
    if (ready()) return true;

    g_store.begin(kNamespace, /*readOnly=*/false);
    const uint32_t storedVersion = g_store.getUInt(kKeyVer, 1);
    String cert = g_store.getString(kKeyCert, "");
    String key  = g_store.getString(kKeyKey, "");

    if (storedVersion != kIdentityVersion && cert.length() > 0) {
        Serial.printf("[identity] stored certificate is version %u, regenerating as %u\r\n",
                      (unsigned)storedVersion, (unsigned)kIdentityVersion);
        cert = ""; key = "";
    }

    if (cert.length() > 0 && key.length() > 0) {
        cert_ = dupString(cert);
        key_  = dupString(key);
        computeFingerprint();
        Serial.printf("[identity] loaded, fingerprint %s\r\n", fingerprint_);
        return ready();
    }

    Serial.println("[identity] no certificate stored, generating one...");
    const uint32_t t0 = millis();
    if (!generate(commonName)) {
        Serial.println("[identity] generation FAILED");
        return false;
    }
    Serial.printf("[identity] generated in %lums, fingerprint %s\r\n",
                  (unsigned long)(millis() - t0), fingerprint_);
    return true;
}

void DeviceIdentity::regenerate(const char *commonName) {
    g_store.begin(kNamespace, false);
    g_store.remove(kKeyCert);
    g_store.remove(kKeyKey);
    free(cert_); free(key_);
    cert_ = key_ = nullptr;
    fingerprint_[0] = '\0';
    begin(commonName);
}

bool DeviceIdentity::generate(const char *commonName) {
    mbedtls_pk_context key;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509write_cert crt;
    mbedtls_mpi serial;

    mbedtls_pk_init(&key);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_x509write_crt_init(&crt);
    mbedtls_mpi_init(&serial);

    bool ok = false;
    unsigned char *pem = static_cast<unsigned char *>(malloc(4096));
    char subject[96];
    snprintf(subject, sizeof(subject), "CN=%s", commonName);

    do {
        if (pem == nullptr) break;
        const char *pers = "ghosthid-identity";
        if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                  (const unsigned char *)pers, strlen(pers)) != 0) break;

        if (mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0) break;
        if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key),
                                mbedtls_ctr_drbg_random, &drbg) != 0) break;

        memset(pem, 0, 4096);
        if (mbedtls_pk_write_key_pem(&key, pem, 4096) != 0) break;
        key_ = strdup((char *)pem);

        // Self-signed: subject and issuer are the same, which is what a
        // fingerprint-based trust model expects.
        mbedtls_x509write_crt_set_subject_key(&crt, &key);
        mbedtls_x509write_crt_set_issuer_key(&crt, &key);
        if (mbedtls_x509write_crt_set_subject_name(&crt, subject) != 0) break;
        if (mbedtls_x509write_crt_set_issuer_name(&crt, subject) != 0) break;
        mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);

        // These extensions are not decoration. A self-signed certificate has to
        // validate as its own root, and OpenSSL - which is what Deskflow uses -
        // will not accept one without CA:TRUE. Omitting them produced a fatal
        // TLS alert with no explanation from the server.
        if (mbedtls_x509write_crt_set_basic_constraints(&crt, 1, -1) != 0) break;
        if (mbedtls_x509write_crt_set_subject_key_identifier(&crt) != 0) break;
        if (mbedtls_x509write_crt_set_authority_key_identifier(&crt) != 0) break;
        // Deliberately NO keyUsage extension. mbedtls marks it critical, and a
        // critical keyUsage constrains what a peer will accept the certificate
        // for; the certificates these servers generate for themselves carry
        // none, so carrying one only creates a way to be rejected.
        // mbedTLS 3.x (pioarduino / arduino-esp32 3.x) dropped the mpi-based
        // set_serial for a raw-bytes variant; 2.x (espressif32 / S2) has only the
        // mpi one. Support both so the S2 and S3 builds share this file.
#if MBEDTLS_VERSION_MAJOR >= 3
        {
            unsigned char serialRaw[] = { 0x01 };
            if (mbedtls_x509write_crt_set_serial_raw(&crt, serialRaw, sizeof(serialRaw)) != 0) break;
        }
#else
        if (mbedtls_mpi_read_string(&serial, 10, "1") != 0) break;
        if (mbedtls_x509write_crt_set_serial(&crt, &serial) != 0) break;
#endif
        // The device has no clock at generation time, so use a fixed window
        // that is comfortably valid. Trust here rests on the fingerprint, not
        // on validity dates.
        if (mbedtls_x509write_crt_set_validity(&crt, "20240101000000",
                                               "20440101000000") != 0) break;

        memset(pem, 0, 4096);
        if (mbedtls_x509write_crt_pem(&crt, pem, 4096,
                                      mbedtls_ctr_drbg_random, &drbg) != 0) break;
        cert_ = strdup((char *)pem);

        if (cert_ == nullptr || key_ == nullptr) break;

        g_store.putString(kKeyCert, cert_);
        g_store.putString(kKeyKey, key_);
        g_store.putUInt(kKeyVer, kIdentityVersion);
        computeFingerprint();
        ok = true;
    } while (false);

    free(pem);
    mbedtls_mpi_free(&serial);
    mbedtls_x509write_crt_free(&crt);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    mbedtls_pk_free(&key);
    return ok && ready();
}

bool DeviceIdentity::computeFingerprint() {
    if (cert_ == nullptr) return false;

    // Deskflow fingerprints the DER form, so parse the PEM back and hash the raw
    // certificate bytes rather than the base64 text.
    mbedtls_x509_crt parsed;
    mbedtls_x509_crt_init(&parsed);
    bool ok = false;
    if (mbedtls_x509_crt_parse(&parsed, (const unsigned char *)cert_,
                               strlen(cert_) + 1) == 0) {
        // This mbedtls build returns void from mbedtls_sha256; the _ret
        // variant that reports failure is not present in every version, so
        // call the plain one.
        uint8_t digest[32];
        mbedtls_sha256(parsed.raw.p, parsed.raw.len, digest, 0);
        for (int i = 0; i < 32; ++i) {
            snprintf(fingerprint_ + i * 2, 3, "%02x", digest[i]);
        }
        ok = true;
    }
    mbedtls_x509_crt_free(&parsed);
    return ok;
}

}  // namespace ghosthid
