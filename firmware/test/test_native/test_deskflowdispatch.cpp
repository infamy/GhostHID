// DeskflowClient::dispatch() tests — the message router for the untrusted screen
// protocol. Runs against the recording HID stub and no-op transport stubs, via
// the GHOSTHID_NATIVE_TEST-only test_dispatch() hook. Includes a fuzz over random
// buffers (a hostile/spoofed server controls these bytes).
#include "test_common.h"
#include "net/DeskflowClient.h"

#define SETUP_DF() \
    HidDevice hid; Config cfg; cfg.begin(); DeskflowClient df(hid, cfg)

void test_dispatch_key_down_up() {
    SETUP_DF();
    // DKDN: keyId=0x0041 ('A'), button=0x0005
    const uint8_t dn[] = {'D','K','D','N', 0x00,0x41, 0x00,0x00, 0x00,0x05};
    df.test_dispatch(dn, sizeof(dn));
    TEST_ASSERT_EQUAL_INT(1, hidhook::keyDownCalls);
    TEST_ASSERT_EQUAL_UINT8('A', hidhook::lastKeyDown);
    TEST_ASSERT_EQUAL_UINT32(1, df.countKey());
    // DKUP: keyId 0, identified by button 5 -> forgetKey -> 'A' -> keyUp
    const uint8_t up[] = {'D','K','U','P', 0x00,0x00, 0x00,0x00, 0x00,0x05};
    df.test_dispatch(up, sizeof(up));
    TEST_ASSERT_EQUAL_INT(1, hidhook::keyUpCalls);
}

void test_dispatch_unknown_keyid_ignored() {
    SETUP_DF();
    const uint8_t dn[] = {'D','K','D','N', 0x00,0x00, 0x00,0x00, 0x00,0x01};
    df.test_dispatch(dn, sizeof(dn));
    TEST_ASSERT_EQUAL_INT(0, hidhook::keyDownCalls);   // keyId 0 -> nothing
}

void test_dispatch_short_key_msg_ignored() {
    SETUP_DF();
    const uint8_t dn[] = {'D','K','D','N', 0x00};      // len 5 < 10
    df.test_dispatch(dn, sizeof(dn));
    TEST_ASSERT_EQUAL_INT(0, hidhook::keyDownCalls);
}

void test_dispatch_mouse_button() {
    SETUP_DF();
    const uint8_t dn[] = {'D','M','D','N', 0x01};      // left down
    df.test_dispatch(dn, sizeof(dn));
    TEST_ASSERT_EQUAL_INT(1, hidhook::mouseBtnDownCalls);
    TEST_ASSERT_TRUE(hid.anythingHeld());
    const uint8_t up[] = {'D','M','U','P', 0x01};
    df.test_dispatch(up, sizeof(up));
    TEST_ASSERT_FALSE(hid.anythingHeld());
}

void test_dispatch_mouse_button_invalid() {
    SETUP_DF();
    const uint8_t dn[] = {'D','M','D','N', 0x09};      // no such button
    df.test_dispatch(dn, sizeof(dn));
    TEST_ASSERT_EQUAL_INT(0, hidhook::mouseBtnDownCalls);
}

void test_dispatch_moves_counted() {
    SETUP_DF();
    const uint8_t mv[] = {'D','M','M','V', 0x00,0x64, 0x00,0x32};   // absolute
    df.test_dispatch(mv, sizeof(mv));
    const uint8_t rm[] = {'D','M','R','M', 0x00,0x05, 0x00,0x05};   // relative
    df.test_dispatch(rm, sizeof(rm));
    TEST_ASSERT_EQUAL_UINT32(2, df.countMove());
}

void test_dispatch_wheel() {
    SETUP_DF();
    const uint8_t w[] = {'D','M','W','M', 0x00,0x00, 0x00,0x78};    // yd=120 -> 1 detent
    df.test_dispatch(w, sizeof(w));
    TEST_ASSERT_EQUAL_INT(1, hidhook::mouseWheelCalls);
}

void test_dispatch_focus_enter_leave_releases() {
    SETUP_DF();
    const uint8_t cinn[] = {'C','I','N','N', 0x00,0x0A, 0x00,0x0A, 0,0,0,0};
    df.test_dispatch(cinn, sizeof(cinn));
    TEST_ASSERT_TRUE(df.hasFocus());
    const uint8_t dn[] = {'D','M','D','N', 0x01};
    df.test_dispatch(dn, sizeof(dn));
    TEST_ASSERT_TRUE(hid.anythingHeld());
    const uint8_t cout[] = {'C','O','U','T'};
    df.test_dispatch(cout, sizeof(cout));   // pointer left -> release everything held
    TEST_ASSERT_FALSE(df.hasFocus());
    TEST_ASSERT_FALSE(hid.anythingHeld());
}

void test_dispatch_unknown_code_counted() {
    SETUP_DF();
    const uint8_t z[] = {'Z','Z','Z','Z', 0,0,0,0};
    df.test_dispatch(z, sizeof(z));
    TEST_ASSERT_EQUAL_UINT32(1, df.countOther());
}

// The bytes here are chosen by whoever runs the screen server. Feed 20k random,
// mostly-malformed messages and require that the interpreter never crashes or
// reads out of bounds (ASan/UBSan in the native build would trip otherwise).
void test_dispatch_fuzz_no_crash() {
    SETUP_DF();
    uint8_t b[40];
    uint32_t seed = 0x1234abcd;
    for (int iter = 0; iter < 20000; ++iter) {
        seed = seed * 1664525u + 1013904223u;
        const size_t n = seed % (sizeof(b) + 1);
        for (size_t i = 0; i < n; ++i) {
            seed = seed * 1664525u + 1013904223u;
            b[i] = (uint8_t)(seed >> 16);
        }
        df.test_dispatch(b, n);
    }
    TEST_ASSERT_TRUE(true);   // survived 20k arbitrary messages
}

// Endpoint busy: the position is held, not sent and not lost; once free, only
// the NEWEST position goes out (stale ones are never replayed).
void test_dispatch_pointer_pending_while_busy() {
    SETUP_DF();
    hidhook::endpointBusy = true;
    const uint8_t a[] = {'D','M','M','V', 0x00,0x64, 0x00,0x32};   // x=100
    const uint8_t b[] = {'D','M','M','V', 0x00,0xC8, 0x00,0x32};   // x=200
    df.test_dispatch(a, sizeof(a));
    df.test_flushPointer();
    TEST_ASSERT_EQUAL_INT(0, hidhook::mouseAbsCalls);
    TEST_ASSERT_EQUAL_UINT32(1, hid.pointerBusyCount());
    df.test_dispatch(b, sizeof(b));
    hidhook::endpointBusy = false;
    df.test_flushPointer();
    TEST_ASSERT_EQUAL_INT(1, hidhook::mouseAbsCalls);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 200.0f / cfg.deskflowWidth(), hidhook::lastAbsX);
    df.test_flushPointer();                                        // nothing pending now
    TEST_ASSERT_EQUAL_INT(1, hidhook::mouseAbsCalls);
}

// Relative motion goes out one clamped report per pass; the rest is kept.
void test_dispatch_relative_one_step_per_pass() {
    SETUP_DF();
    const uint8_t m[] = {'D','M','R','M', 0x01,0x2C, 0x00,0x00};   // dx=300
    df.test_dispatch(m, sizeof(m));
    df.test_flushPointer();
    TEST_ASSERT_EQUAL_INT(1, hidhook::mouseMoveCalls);
    TEST_ASSERT_EQUAL_INT32(127, hidhook::lastRelDx);
    df.test_flushPointer();
    df.test_flushPointer();
    TEST_ASSERT_EQUAL_INT(3, hidhook::mouseMoveCalls);
    TEST_ASSERT_EQUAL_INT32(46, hidhook::lastRelDx);               // 300-127-127
    df.test_flushPointer();
    TEST_ASSERT_EQUAL_INT(3, hidhook::mouseMoveCalls);
}

// A click forces pending motion out even while the endpoint is busy, so it lands
// where the pointer is.
void test_dispatch_click_flushes_pending_motion() {
    SETUP_DF();
    hidhook::endpointBusy = true;
    const uint8_t mv[] = {'D','M','M','V', 0x00,0x64, 0x00,0x32};
    df.test_dispatch(mv, sizeof(mv));
    df.test_flushPointer();
    TEST_ASSERT_EQUAL_INT(0, hidhook::mouseAbsCalls);
    const uint8_t dn[] = {'D','M','D','N', 0x01};
    df.test_dispatch(dn, sizeof(dn));
    TEST_ASSERT_EQUAL_INT(1, hidhook::mouseAbsCalls);
    TEST_ASSERT_EQUAL_INT(1, hidhook::mouseBtnDownCalls);
}
