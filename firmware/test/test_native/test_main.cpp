// Host unit-test runner. Compiles the real CommandProcessor / Config / Keymap
// on the dev machine (platform = native) against a shim layer + a recording HID
// stub. No board, no shipped-code changes. Run with `make test`.
#include "test_common.h"

// Shared state referenced by test_common.h helpers.
uint32_t g_test_millis = 1000;
char     g_buf[1200];

// Config keeps its NVS in a single file-scope Preferences, so it persists across
// tests in one process. Clear it (and the recording HID) before every test for
// isolation, and reset the shim clock.
void setUp() {
    hidhook::reset();
    g_test_millis = 1000;
    Config c; c.factoryReset();     // wipes the in-memory Preferences shim
}
void tearDown() {}

// --- CommandProcessor ---
void test_auth_wrong_then_right();
void test_auth_case_insensitive();
void test_auth_ignores_spaces();
void test_no_token_means_open();
void test_unauthenticated_input_blocked();
void test_per_client_auth_isolation();
void test_rate_limit_lockout_and_decay();
void test_lockout_requests_disconnect();
void test_h8_release_held_input_on_disconnect();
void test_disconnect_of_last_controller_releases();
void test_key_down_then_up();
void test_unknown_key_rejected();
void test_text_types();
void test_mouse_move();
void test_mouse_button_press_release();
void test_mouse_button_unknown_rejected();
void test_mouse_wheel();
void test_mouse_abs_rejects_non_finite();
void test_mouse_abs_clamps_out_of_range();
void test_media_key();
void test_media_unknown_rejected();
void test_system_key();
void test_system_unknown_rejected();
void test_usb_not_ready_blocks_input();
void test_ota_lock_blocks_input();
void test_bad_json_rejected();
void test_ping_pong();
void test_get_config_shape();
void test_status_shape();
void test_kvm_trust_cert_nothing_pending();
void test_set_config_scroll_invert_is_live();
void test_set_config_rejects_short_token();
void test_response_too_large_is_wellformed();
// --- Config ---
void test_config_token_validation();
void test_config_ap_password_validation();
void test_config_device_name_validation();
void test_config_screen_size_validation();
void test_config_server_validation();
void test_config_provisions_random_token();
// --- Keymap ---
void test_keymap_named_keys();
void test_keymap_single_char_is_itself();
void test_keymap_unknown_is_zero();
// --- ControllerTable ---
void test_ct_acquire_release();
void test_ct_idempotent_acquire();
void test_ct_full_then_room();
void test_ct_release_unknown_is_noop();
void test_ct_rejects_zero_id();
void test_ct_clear();
void test_ct_entry_fields_for_timeout_sweep();
// --- DeskflowWire ---
void test_wire_readers();
void test_is_matches_code();
void test_keyid_printable_ascii();
void test_keyid_special_keys();
void test_keyid_unmapped_is_zero();
void test_keyid_high_bit_fallback();
void test_keyid_fuzz_full_range();

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_auth_wrong_then_right);
    RUN_TEST(test_auth_case_insensitive);
    RUN_TEST(test_auth_ignores_spaces);
    RUN_TEST(test_no_token_means_open);
    RUN_TEST(test_unauthenticated_input_blocked);
    RUN_TEST(test_per_client_auth_isolation);
    RUN_TEST(test_rate_limit_lockout_and_decay);
    RUN_TEST(test_lockout_requests_disconnect);
    RUN_TEST(test_h8_release_held_input_on_disconnect);
    RUN_TEST(test_disconnect_of_last_controller_releases);
    RUN_TEST(test_key_down_then_up);
    RUN_TEST(test_unknown_key_rejected);
    RUN_TEST(test_text_types);
    RUN_TEST(test_mouse_move);
    RUN_TEST(test_mouse_button_press_release);
    RUN_TEST(test_mouse_button_unknown_rejected);
    RUN_TEST(test_mouse_wheel);
    RUN_TEST(test_mouse_abs_rejects_non_finite);
    RUN_TEST(test_mouse_abs_clamps_out_of_range);
    RUN_TEST(test_media_key);
    RUN_TEST(test_media_unknown_rejected);
    RUN_TEST(test_system_key);
    RUN_TEST(test_system_unknown_rejected);
    RUN_TEST(test_usb_not_ready_blocks_input);
    RUN_TEST(test_ota_lock_blocks_input);
    RUN_TEST(test_bad_json_rejected);
    RUN_TEST(test_ping_pong);
    RUN_TEST(test_get_config_shape);
    RUN_TEST(test_status_shape);
    RUN_TEST(test_kvm_trust_cert_nothing_pending);
    RUN_TEST(test_set_config_scroll_invert_is_live);
    RUN_TEST(test_set_config_rejects_short_token);
    RUN_TEST(test_response_too_large_is_wellformed);

    RUN_TEST(test_config_token_validation);
    RUN_TEST(test_config_ap_password_validation);
    RUN_TEST(test_config_device_name_validation);
    RUN_TEST(test_config_screen_size_validation);
    RUN_TEST(test_config_server_validation);
    RUN_TEST(test_config_provisions_random_token);

    RUN_TEST(test_keymap_named_keys);
    RUN_TEST(test_keymap_single_char_is_itself);
    RUN_TEST(test_keymap_unknown_is_zero);

    RUN_TEST(test_ct_acquire_release);
    RUN_TEST(test_ct_idempotent_acquire);
    RUN_TEST(test_ct_full_then_room);
    RUN_TEST(test_ct_release_unknown_is_noop);
    RUN_TEST(test_ct_rejects_zero_id);
    RUN_TEST(test_ct_clear);
    RUN_TEST(test_ct_entry_fields_for_timeout_sweep);

    RUN_TEST(test_wire_readers);
    RUN_TEST(test_is_matches_code);
    RUN_TEST(test_keyid_printable_ascii);
    RUN_TEST(test_keyid_special_keys);
    RUN_TEST(test_keyid_unmapped_is_zero);
    RUN_TEST(test_keyid_high_bit_fallback);
    RUN_TEST(test_keyid_fuzz_full_range);

    return UNITY_END();
}
