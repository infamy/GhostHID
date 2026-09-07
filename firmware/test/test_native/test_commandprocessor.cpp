// CommandProcessor tests — the auth gate, per-client sessions, input dispatch,
// clamping, gating (USB-not-ready, OTA lock), and the protocol replies.
#include "test_common.h"

// ---- auth & sessions -------------------------------------------------------

void test_auth_wrong_then_right() {
    SETUP_PROC(); p.beginSession(1);
    send(p, 1, "{\"type\":\"auth\",\"token\":\"WRONG\"}");
    TEST_ASSERT_TRUE(replied("\"ok\":false"));
    send(p, 1, "{\"type\":\"auth\",\"token\":\"TESTTOKEN\"}");
    TEST_ASSERT_TRUE(replied("\"ok\":true"));
}

void test_auth_case_insensitive() {
    SETUP_PROC(); p.beginSession(1);
    send(p, 1, "{\"type\":\"auth\",\"token\":\"testtoken\"}");   // lower-case
    TEST_ASSERT_TRUE(replied("\"ok\":true"));
}

void test_auth_ignores_spaces() {
    SETUP_PROC(); p.beginSession(1);
    send(p, 1, "{\"type\":\"auth\",\"token\":\"TEST TOKEN\"}");  // display spaces
    TEST_ASSERT_TRUE(replied("\"ok\":true"));
}

void test_no_token_means_open() {
    HidDevice hid; Config cfg; cfg.begin(); cfg.setAuthToken("");   // auth disabled
    CommandProcessor p(hid, cfg);
    p.beginSession(1);
    send(p, 1, "{\"type\":\"key\",\"key\":\"ENTER\",\"pressed\":true}");
    TEST_ASSERT_EQUAL_INT(1, hidhook::keyDownCalls);   // works with no auth step
}

void test_unauthenticated_input_blocked() {
    SETUP_PROC(); p.beginSession(1);
    send(p, 1, "{\"type\":\"key\",\"key\":\"ENTER\",\"pressed\":true}");
    TEST_ASSERT_TRUE(replied("unauthenticated"));
    TEST_ASSERT_EQUAL_INT(0, hidhook::keyDownCalls);
}

void test_per_client_auth_isolation() {
    SETUP_PROC(); p.beginSession(1); p.beginSession(2);
    send(p, 1, "{\"type\":\"auth\",\"token\":\"TESTTOKEN\"}");
    send(p, 2, "{\"type\":\"key\",\"key\":\"ENTER\",\"pressed\":true}");
    TEST_ASSERT_TRUE(replied("unauthenticated"));
    const int before = hidhook::keyDownCalls;
    send(p, 1, "{\"type\":\"key\",\"key\":\"ENTER\",\"pressed\":true}");
    TEST_ASSERT_TRUE(hidhook::keyDownCalls > before);
}

void test_rate_limit_lockout_and_decay() {
    SETUP_PROC(); p.beginSession(1);
    for (int i = 0; i < 3; ++i) send(p, 1, "{\"type\":\"auth\",\"token\":\"WRONG\"}");
    send(p, 1, "{\"type\":\"auth\",\"token\":\"TESTTOKEN\"}");
    TEST_ASSERT_TRUE(replied("\"locked\":true"));
    g_test_millis += 130000;                                   // quiet > 2 min
    send(p, 1, "{\"type\":\"auth\",\"token\":\"TESTTOKEN\"}");
    TEST_ASSERT_TRUE(replied("\"ok\":true"));
}

void test_lockout_requests_disconnect() {
    SETUP_PROC(); p.beginSession(1);
    for (int i = 0; i < 3; ++i) send(p, 1, "{\"type\":\"auth\",\"token\":\"WRONG\"}");
    TEST_ASSERT_TRUE(p.consumeDisconnectRequest());   // set on lockout
}

// ---- disconnect safety (H8) ------------------------------------------------

void test_h8_release_held_input_on_disconnect() {
    SETUP_PROC(); AUTHED(1);
    send(p, 1, "{\"type\":\"key\",\"key\":\"ENTER\",\"pressed\":true}");
    TEST_ASSERT_TRUE(hid.anythingHeld());
    const int before = hidhook::releaseAllCalls;
    p.endSession(1);
    TEST_ASSERT_TRUE(hidhook::releaseAllCalls > before);
    TEST_ASSERT_FALSE(hid.anythingHeld());
}

void test_disconnect_of_last_controller_releases() {
    SETUP_PROC(); AUTHED(1); AUTHED(2);
    send(p, 1, "{\"type\":\"mouse_button\",\"button\":\"left\",\"pressed\":true}");
    TEST_ASSERT_TRUE(hid.anythingHeld());
    p.endSession(1);                       // one leaves while holding -> released
    TEST_ASSERT_FALSE(hid.anythingHeld());
}

// ---- keyboard --------------------------------------------------------------

void test_key_down_then_up() {
    SETUP_PROC(); AUTHED(1);
    send(p, 1, "{\"type\":\"key\",\"key\":\"a\",\"pressed\":true}");
    TEST_ASSERT_EQUAL_INT(1, hidhook::keyDownCalls);
    TEST_ASSERT_TRUE(hid.anythingHeld());
    send(p, 1, "{\"type\":\"key\",\"key\":\"a\",\"pressed\":false}");
    TEST_ASSERT_EQUAL_INT(1, hidhook::keyUpCalls);
    TEST_ASSERT_FALSE(hid.anythingHeld());
}

void test_unknown_key_rejected() {
    SETUP_PROC(); AUTHED(1);
    send(p, 1, "{\"type\":\"key\",\"key\":\"NOPE\",\"pressed\":true}");
    TEST_ASSERT_TRUE(replied("unknown key"));
    TEST_ASSERT_EQUAL_INT(0, hidhook::keyDownCalls);
}

void test_text_types() {
    SETUP_PROC(); AUTHED(1);
    send(p, 1, "{\"type\":\"text\",\"text\":\"hello world\"}");
    TEST_ASSERT_EQUAL_INT(1, hidhook::typeTextCalls);
}

// ---- mouse -----------------------------------------------------------------

void test_mouse_move() {
    SETUP_PROC(); AUTHED(1);
    send(p, 1, "{\"type\":\"mouse_move\",\"dx\":10,\"dy\":-5}");
    TEST_ASSERT_EQUAL_INT(1, hidhook::mouseMoveCalls);
}

void test_mouse_button_press_release() {
    SETUP_PROC(); AUTHED(1);
    send(p, 1, "{\"type\":\"mouse_button\",\"button\":\"right\",\"pressed\":true}");
    TEST_ASSERT_EQUAL_INT(1, hidhook::mouseBtnDownCalls);
    TEST_ASSERT_TRUE(hid.anythingHeld());
    send(p, 1, "{\"type\":\"mouse_button\",\"button\":\"right\",\"pressed\":false}");
    TEST_ASSERT_FALSE(hid.anythingHeld());
}

void test_mouse_button_unknown_rejected() {
    SETUP_PROC(); AUTHED(1);
    send(p, 1, "{\"type\":\"mouse_button\",\"button\":\"scroll\",\"pressed\":true}");
    TEST_ASSERT_TRUE(replied("unknown button"));
    TEST_ASSERT_EQUAL_INT(0, hidhook::mouseBtnDownCalls);
}

void test_mouse_wheel() {
    SETUP_PROC(); AUTHED(1);
    send(p, 1, "{\"type\":\"mouse_wheel\",\"delta\":3}");
    TEST_ASSERT_EQUAL_INT(1, hidhook::mouseWheelCalls);
}

void test_mouse_abs_rejects_non_finite() {
    SETUP_PROC(); AUTHED(1);
    send(p, 1, "{\"type\":\"mouse_abs\",\"x\":1e400,\"y\":0.5}");
    TEST_ASSERT_TRUE(replied("finite"));
    TEST_ASSERT_EQUAL_INT(0, hidhook::mouseAbsCalls);
    send(p, 1, "{\"type\":\"mouse_abs\",\"x\":0.5,\"y\":0.5}");
    TEST_ASSERT_EQUAL_INT(1, hidhook::mouseAbsCalls);
}

void test_mouse_abs_clamps_out_of_range() {
    SETUP_PROC(); AUTHED(1);
    send(p, 1, "{\"type\":\"mouse_abs\",\"x\":5.0,\"y\":-2.0}");   // out of 0..1
    TEST_ASSERT_EQUAL_FLOAT(1.0f, hid.absoluteX());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, hid.absoluteY());
}

// ---- media / system --------------------------------------------------------

void test_media_key() {
    SETUP_PROC(); AUTHED(1);
    send(p, 1, "{\"type\":\"media\",\"key\":\"vol_up\"}");
    TEST_ASSERT_EQUAL_INT(1, hidhook::mediaCalls);
}
void test_media_unknown_rejected() {
    SETUP_PROC(); AUTHED(1);
    send(p, 1, "{\"type\":\"media\",\"key\":\"eject\"}");
    TEST_ASSERT_TRUE(replied("unknown media key"));
}
void test_system_key() {
    SETUP_PROC(); AUTHED(1);
    send(p, 1, "{\"type\":\"system\",\"key\":\"sleep\"}");
    TEST_ASSERT_EQUAL_INT(1, hidhook::systemCalls);
}
void test_system_unknown_rejected() {
    SETUP_PROC(); AUTHED(1);
    send(p, 1, "{\"type\":\"system\",\"key\":\"selfdestruct\"}");
    TEST_ASSERT_TRUE(replied("unknown system key"));
}

// ---- gating ----------------------------------------------------------------

void test_usb_not_ready_blocks_input() {
    SETUP_PROC(); AUTHED(1);
    hidhook::ready = false;
    send(p, 1, "{\"type\":\"key\",\"key\":\"ENTER\",\"pressed\":true}");
    TEST_ASSERT_TRUE(replied("usb not ready"));
    TEST_ASSERT_EQUAL_INT(0, hidhook::keyDownCalls);
}

void test_ota_lock_blocks_input() {
    SETUP_PROC(); AUTHED(1);
    p.setLocked(true, "firmware update in progress");
    send(p, 1, "{\"type\":\"key\",\"key\":\"ENTER\",\"pressed\":true}");
    TEST_ASSERT_EQUAL_INT(0, hidhook::keyDownCalls);
    p.setLocked(false, nullptr);
    send(p, 1, "{\"type\":\"key\",\"key\":\"ENTER\",\"pressed\":true}");
    TEST_ASSERT_EQUAL_INT(1, hidhook::keyDownCalls);
}

// ---- protocol replies ------------------------------------------------------

void test_bad_json_rejected() {
    SETUP_PROC(); p.beginSession(1);
    send(p, 1, "{not json");
    TEST_ASSERT_TRUE(replied("bad json"));
}

void test_ping_pong() {
    SETUP_PROC(); AUTHED(1);
    send(p, 1, "{\"type\":\"ping\"}");
    TEST_ASSERT_TRUE(replied("\"type\":\"pong\""));
    TEST_ASSERT_TRUE(replied("\"ctrls\":"));       // controller count is reported
}

void test_get_config_shape() {
    SETUP_PROC(); AUTHED(1);
    send(p, 1, "{\"type\":\"get_config\"}");
    TEST_ASSERT_TRUE(replied("\"type\":\"config\""));
    TEST_ASSERT_TRUE(replied("kvm_cert_pending"));
}

void test_status_shape() {
    SETUP_PROC(); AUTHED(1);
    send(p, 1, "{\"type\":\"status\"}");
    TEST_ASSERT_TRUE(replied("\"type\":\"status\""));
}

void test_kvm_trust_cert_nothing_pending() {
    SETUP_PROC(); AUTHED(1);
    send(p, 1, "{\"type\":\"kvm_trust_cert\"}");
    TEST_ASSERT_TRUE(replied("awaiting confirmation"));   // deskflow not attached
}

void test_set_config_scroll_invert_is_live() {
    SETUP_PROC(); AUTHED(1);
    send(p, 1, "{\"type\":\"set_config\",\"scroll_invert\":true}");
    TEST_ASSERT_TRUE(hid.invertScroll());     // applied to HID immediately
}

void test_set_config_rejects_short_token() {
    SETUP_PROC(); AUTHED(1);
    send(p, 1, "{\"type\":\"set_config\",\"token\":\"abc\"}");   // < 6 chars
    TEST_ASSERT_TRUE(replied("error"));
    TEST_ASSERT_EQUAL_STRING("TESTTOKEN", cfg.authToken());     // unchanged
}

// M5: a reply that won't fit must be a short, well-formed error, not truncated
// JSON the browser can't parse.
void test_response_too_large_is_wellformed() {
    SETUP_PROC(); AUTHED(1);
    char tiny[80];
    const char *msg = "{\"type\":\"get_config\"}";   // get_config is large
    p.handleMessage(1, msg, strlen(msg), tiny, sizeof(tiny));
    TEST_ASSERT_TRUE(strstr(tiny, "too large") != nullptr);
    TEST_ASSERT_EQUAL_CHAR('}', tiny[strlen(tiny) - 1]);   // closed brace = valid JSON
}
