// Deskflow wire-parsing tests — the code that turns bytes straight off the
// network into HID keycodes and field values. keyIdToHid in particular is an
// untrusted-input mapper (a hostile/spoofed server picks the KeyID), so it gets
// a full-range fuzz over every possible value.
#include "test_common.h"
#include "net/DeskflowWire.h"

// ---- big-endian readers ----------------------------------------------------

void test_wire_readers() {
    const uint8_t b[] = {0x12, 0x34, 0x56, 0x78};
    TEST_ASSERT_EQUAL_UINT16(0x1234, rd16(b));
    TEST_ASSERT_EQUAL_UINT32(0x12345678u, rd32(b));
    const uint8_t neg[] = {0xFF, 0xFF};
    TEST_ASSERT_EQUAL_INT16(-1, rdS16(neg));
    const uint8_t pos[] = {0x00, 0x7B};
    TEST_ASSERT_EQUAL_INT16(123, rdS16(pos));
}

void test_is_matches_code() {
    const uint8_t m[] = {'D','K','D','N', 0,0};
    TEST_ASSERT_TRUE(is(m, sizeof(m), "DKDN"));
    TEST_ASSERT_FALSE(is(m, sizeof(m), "DMMV"));
    TEST_ASSERT_FALSE(is(m, 3, "DKDN"));     // too short to hold a 4-char code
}

// ---- keyIdToHid: the untrusted KeyID -> HID map ----------------------------

void test_keyid_printable_ascii() {
    TEST_ASSERT_EQUAL_UINT8('A', keyIdToHid('A'));
    TEST_ASSERT_EQUAL_UINT8('z', keyIdToHid('z'));
    TEST_ASSERT_EQUAL_UINT8(' ', keyIdToHid(0x20));
    TEST_ASSERT_EQUAL_UINT8('~', keyIdToHid(0x7E));
}

void test_keyid_special_keys() {
    TEST_ASSERT_EQUAL_UINT8(key::Return,   keyIdToHid(0xEF0D));
    TEST_ASSERT_EQUAL_UINT8(key::Escape,   keyIdToHid(0xEF1B));
    TEST_ASSERT_EQUAL_UINT8(key::LeftCtrl, keyIdToHid(0xEFE3));
    TEST_ASSERT_EQUAL_UINT8(key::F1,       keyIdToHid(0xEFBE));
    TEST_ASSERT_EQUAL_UINT8(key::F12,      keyIdToHid(0xEFC9));
}

void test_keyid_unmapped_is_zero() {
    TEST_ASSERT_EQUAL_UINT8(0, keyIdToHid(0x0000));
    TEST_ASSERT_EQUAL_UINT8(0, keyIdToHid(0xEF00));   // 0xEF00 page, not in table
    TEST_ASSERT_EQUAL_UINT8(0, keyIdToHid(0xEF41));   // page excluded from fallback
    TEST_ASSERT_EQUAL_UINT8(0, keyIdToHid(0x001F));   // below printable range
}

void test_keyid_high_bit_fallback() {
    // A server that sets a non-0xEF page on a plain char -> we still type it.
    TEST_ASSERT_EQUAL_UINT8('A', keyIdToHid(0x0141));   // low byte 0x41, page 0x0100
}

// Full-range fuzz: every possible KeyID must return without crashing, and the
// printable-ASCII invariant must always hold.
void test_keyid_fuzz_full_range() {
    for (uint32_t id = 0; id <= 0xFFFF; ++id) {
        const uint8_t c = keyIdToHid((uint16_t)id);
        if (id >= 0x20 && id <= 0x7E) TEST_ASSERT_EQUAL_UINT8((uint8_t)id, c);
        else (void)c;   // any other value: just must not crash / must terminate
    }
}
