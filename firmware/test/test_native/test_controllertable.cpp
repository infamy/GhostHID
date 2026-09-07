// ControllerTable tests — the multi-controller slot bookkeeping, extracted from
// Network so it can be tested without the AsyncWebServer/Wi-Fi stack.
#include "test_common.h"
#include "net/ControllerTable.h"

void test_ct_acquire_release() {
    ControllerTable t;
    TEST_ASSERT_EQUAL_UINT(0, t.count());
    TEST_ASSERT_TRUE(t.acquire(10, 1000));
    TEST_ASSERT_TRUE(t.contains(10));
    TEST_ASSERT_EQUAL_UINT(1, t.count());
    t.release(10);
    TEST_ASSERT_FALSE(t.contains(10));
    TEST_ASSERT_EQUAL_UINT(0, t.count());
}

void test_ct_idempotent_acquire() {
    ControllerTable t;
    TEST_ASSERT_TRUE(t.acquire(5, 1));
    TEST_ASSERT_TRUE(t.acquire(5, 2));   // duplicate connect must not double-count
    TEST_ASSERT_EQUAL_UINT(1, t.count());
}

void test_ct_full_then_room() {
    ControllerTable t;
    for (unsigned i = 1; i <= ControllerTable::kCapacity; ++i)
        TEST_ASSERT_TRUE(t.acquire(i, 0));
    TEST_ASSERT_EQUAL_UINT(ControllerTable::kCapacity, t.count());
    TEST_ASSERT_FALSE(t.acquire(999, 0));   // full -> refused
    t.release(1);                           // freeing one makes room
    TEST_ASSERT_TRUE(t.acquire(999, 0));
}

void test_ct_release_unknown_is_noop() {
    ControllerTable t; t.acquire(7, 0);
    t.release(123);                         // not present
    TEST_ASSERT_EQUAL_UINT(1, t.count());
    TEST_ASSERT_TRUE(t.contains(7));
}

void test_ct_rejects_zero_id() {
    ControllerTable t;
    TEST_ASSERT_FALSE(t.acquire(0, 0));     // 0 == "free slot" sentinel
    TEST_ASSERT_FALSE(t.contains(0));
    TEST_ASSERT_EQUAL_UINT(0, t.count());
}

void test_ct_clear() {
    ControllerTable t; t.acquire(1, 0); t.acquire(2, 0);
    t.clear();
    TEST_ASSERT_EQUAL_UINT(0, t.count());
    TEST_ASSERT_FALSE(t.contains(1));
}

void test_ct_entry_fields_for_timeout_sweep() {
    ControllerTable t; t.acquire(42, 5000);
    bool found = false;
    for (auto &e : t) {
        if (e.id == 42) { found = true; TEST_ASSERT_EQUAL_UINT32(5000, e.since);
                          TEST_ASSERT_FALSE(e.authTimedOut); }
    }
    TEST_ASSERT_TRUE(found);
}
