// Sealed mode: the network-facing enforcement. A sealed device is read-only over
// the wire - config writes, cert-pinning and OTA are refused - and it advertises
// the state so the UI can show it. The physical unseal path (BOOT hold + serial
// console) lives in main()/SerialConsole and is exercised on hardware; here we
// test the parts that run through CommandProcessor and Config.
#include "test_common.h"

// --- Config: the flag persists and round-trips through NVS -------------------

void test_config_sealed_defaults_off() {
    Config c; c.begin();
    TEST_ASSERT_FALSE(c.sealed());
}

void test_config_sealed_roundtrip() {
    Config a; a.begin();
    a.setSealed(true);
    TEST_ASSERT_TRUE(a.sealed());
    // A fresh Config reads the same shared NVS store - proves it persisted, not
    // just cached in the instance.
    Config b; b.begin();
    TEST_ASSERT_TRUE(b.sealed());
    b.setSealed(false);
    Config c; c.begin();
    TEST_ASSERT_FALSE(c.sealed());
}

void test_config_factory_reset_clears_sealed() {
    Config a; a.begin();
    a.setSealed(true);
    a.factoryReset();
    Config b; b.begin();
    TEST_ASSERT_FALSE(b.sealed());   // reset wipes the namespace, seal included
}

// --- CommandProcessor: sealed refuses config-mutating commands --------------

void test_sealed_blocks_set_config() {
    SETUP_PROC(); AUTHED(1);
    cfg.setSealed(true);
    // ap_always defaults true; try to turn it off over the wire.
    send(p, 1, "{\"type\":\"set_config\",\"ap_always\":false}");
    TEST_ASSERT_TRUE(replied("\"sealed\":true"));
    TEST_ASSERT_TRUE(cfg.apAlways());     // refused -> unchanged
}

void test_unsealed_allows_set_config() {
    SETUP_PROC(); AUTHED(1);
    send(p, 1, "{\"type\":\"set_config\",\"ap_always\":false}");
    TEST_ASSERT_FALSE(cfg.apAlways());    // applied when not sealed (control)
}

void test_sealed_blocks_kvm_trust_cert() {
    SETUP_PROC(); AUTHED(1);
    cfg.setSealed(true);
    // The sealed check precedes the "nothing pending" check, so the reason is
    // the seal, not the missing certificate.
    send(p, 1, "{\"type\":\"kvm_trust_cert\"}");
    TEST_ASSERT_TRUE(replied("\"sealed\":true"));
}

// --- CommandProcessor: sealed is advertised ---------------------------------

void test_status_reports_sealed() {
    SETUP_PROC(); AUTHED(1);
    cfg.setSealed(true);
    send(p, 1, "{\"type\":\"status\"}");
    TEST_ASSERT_TRUE(replied("\"sealed\":true"));
}

void test_get_config_reports_sealed() {
    SETUP_PROC(); AUTHED(1);
    cfg.setSealed(true);
    send(p, 1, "{\"type\":\"get_config\"}");
    TEST_ASSERT_TRUE(replied("\"sealed\":true"));
}

// A sealed device must still take input and answer pings - sealing locks down
// configuration, not the device's actual job of typing into the host.
void test_sealed_still_accepts_input() {
    SETUP_PROC(); AUTHED(1);
    cfg.setSealed(true);
    // hidhook::ready defaults true after reset(), so USB is "enumerated".
    send(p, 1, "{\"type\":\"key\",\"key\":\"ENTER\",\"pressed\":true}");
    TEST_ASSERT_EQUAL_INT(1, hidhook::keyDownCalls);
}
