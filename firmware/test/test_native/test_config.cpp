// Config tests — the real validation logic (compiled against the in-memory
// Preferences shim), which guards what a controller is allowed to set.
#include "test_common.h"

void test_config_token_validation() {
    Config cfg; cfg.begin();
    TEST_ASSERT_TRUE(cfg.setAuthToken(""));          // empty = auth disabled, allowed
    TEST_ASSERT_FALSE(cfg.setAuthToken("12345"));    // 5 < 6
    TEST_ASSERT_TRUE(cfg.setAuthToken("123456"));    // 6 ok
    char big[64]; memset(big, 'A', 49); big[49] = '\0';
    TEST_ASSERT_FALSE(cfg.setAuthToken(big));        // 49 > 48
}

void test_config_ap_password_validation() {
    Config cfg; cfg.begin();
    TEST_ASSERT_FALSE(cfg.setApPassword("short7!")); // 7 chars
    TEST_ASSERT_TRUE(cfg.setApPassword("eightchr"));  // 8 chars
}

void test_config_device_name_validation() {
    Config cfg; cfg.begin();
    TEST_ASSERT_TRUE(cfg.setDeviceName("ghost-hid1"));
    TEST_ASSERT_FALSE(cfg.setDeviceName("bad name"));  // space not DNS-safe
    TEST_ASSERT_FALSE(cfg.setDeviceName("nope!"));     // punctuation
    TEST_ASSERT_FALSE(cfg.setDeviceName(""));          // empty
}

void test_config_screen_size_validation() {
    Config cfg; cfg.begin();
    TEST_ASSERT_TRUE(cfg.setDeskflowScreenSize(1920, 1080));
    TEST_ASSERT_FALSE(cfg.setDeskflowScreenSize(10, 10));       // below min
    TEST_ASSERT_FALSE(cfg.setDeskflowScreenSize(20000, 20000)); // above max
}

void test_config_server_validation() {
    Config cfg; cfg.begin();
    TEST_ASSERT_TRUE(cfg.setDeskflowServer("192.168.1.10", 24800));
    TEST_ASSERT_EQUAL_UINT16(24800, cfg.deskflowPort());
    TEST_ASSERT_EQUAL_STRING("192.168.1.10", cfg.deskflowHost());
}

void test_config_provisions_random_token() {
    // setUp() cleared NVS, so this is a genuine first boot.
    Config cfg; cfg.begin();
    TEST_ASSERT_TRUE(cfg.justProvisioned());
    TEST_ASSERT_TRUE(strcmp(cfg.authToken(), "ghosthid") != 0);  // not the default
    TEST_ASSERT_TRUE(strlen(cfg.authToken()) >= 6);
    TEST_ASSERT_TRUE(cfg.apPassword()[0] != '\0');
}
