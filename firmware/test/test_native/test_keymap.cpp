// Keymap tests — lookupKey turns wire names into HID keycodes; it's the parser
// behind every "key"/combo command.
#include "test_common.h"

void test_keymap_named_keys() {
    TEST_ASSERT_EQUAL_UINT8(key::Return, lookupKey("ENTER"));
    TEST_ASSERT_EQUAL_UINT8(key::Return, lookupKey("enter"));   // case-insensitive
    TEST_ASSERT_EQUAL_UINT8(key::Return, lookupKey("return"));  // alias
    TEST_ASSERT_EQUAL_UINT8(key::F1,     lookupKey("F1"));
    TEST_ASSERT_EQUAL_UINT8(key::F12,    lookupKey("f12"));
}

void test_keymap_single_char_is_itself() {
    TEST_ASSERT_EQUAL_UINT8('a', lookupKey("a"));
    TEST_ASSERT_EQUAL_UINT8('Z', lookupKey("Z"));
    TEST_ASSERT_EQUAL_UINT8('1', lookupKey("1"));
}

void test_keymap_unknown_is_zero() {
    TEST_ASSERT_EQUAL_UINT8(0, lookupKey(""));
    TEST_ASSERT_EQUAL_UINT8(0, lookupKey("NOPE"));
    TEST_ASSERT_EQUAL_UINT8(0, lookupKey("this-name-is-far-too-long-to-match"));
}
