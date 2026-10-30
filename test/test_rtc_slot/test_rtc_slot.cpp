/**
 * Native unit tests for IotRtcSlot (pio test -e native).
 *
 * On target the slot lives in RTC_NOINIT_ATTR memory; here a memset stands in
 * for what that memory holds after a power-on reset or an OTA update.
 */

#include <unity.h>

#include <cstring>

#include "iot_rtc_slot.h"

// *****************************************************************************

void setUp() {}
void tearDown() {}

struct AppState
{
    char etag[16];
    uint32_t deferredCycles;
};

struct BiggerState
{
    char etag[16];
    uint32_t deferredCycles;
    uint32_t added;
};


// *****************************************************************************

void test_garbage_is_detected_and_reset()
{
    IotRtcSlot<AppState> slot;
    std::memset(&slot, 0xA5, sizeof(slot));   // power-on contents
    TEST_ASSERT_FALSE(slot.isValid());

    AppState& state = slot.get();
    TEST_ASSERT_TRUE(slot.isValid());
    TEST_ASSERT_EQUAL_UINT32(0, state.deferredCycles);
    TEST_ASSERT_EQUAL_STRING("", state.etag);
}

void test_a_valid_value_survives()
{
    IotRtcSlot<AppState> slot;
    std::memset(&slot, 0, sizeof(slot));
    slot->deferredCycles = 3;
    std::strcpy(slot->etag, "\"abc\"");

    // a deep sleep: the same bytes, seen again by the same firmware
    IotRtcSlot<AppState> after;
    std::memcpy(&after, &slot, sizeof(slot));
    TEST_ASSERT_TRUE(after.isValid());
    TEST_ASSERT_EQUAL_UINT32(3, after->deferredCycles);
    TEST_ASSERT_EQUAL_STRING("\"abc\"", after->etag);
}

void test_a_changed_version_resets()
{
    IotRtcSlot<AppState, 1> slot;
    std::memset(&slot, 0, sizeof(slot));
    slot->deferredCycles = 3;

    // an OTA update that changed the meaning but not the size
    IotRtcSlot<AppState, 2> updated;
    static_assert(sizeof(updated) == sizeof(slot), "same size, different version");
    std::memcpy(&updated, &slot, sizeof(slot));
    TEST_ASSERT_FALSE(updated.isValid());
    TEST_ASSERT_EQUAL_UINT32(0, updated->deferredCycles);
}

void test_a_changed_size_resets()
{
    IotRtcSlot<AppState> slot;
    std::memset(&slot, 0, sizeof(slot));
    slot->deferredCycles = 3;

    IotRtcSlot<BiggerState> updated;
    std::memset(&updated, 0, sizeof(updated));
    std::memcpy(&updated, &slot, sizeof(slot));   // the old bytes at the same address
    TEST_ASSERT_FALSE(updated.isValid());
    TEST_ASSERT_EQUAL_UINT32(0, updated->added);
}

void test_reset_discards_the_value()
{
    IotRtcSlot<AppState> slot;
    std::memset(&slot, 0, sizeof(slot));
    slot->deferredCycles = 7;
    slot.reset();
    TEST_ASSERT_TRUE(slot.isValid());
    TEST_ASSERT_EQUAL_UINT32(0, (*slot).deferredCycles);
}


// *****************************************************************************

int main(int argc, char **argv)
{
    UNITY_BEGIN();

    RUN_TEST(test_garbage_is_detected_and_reset);
    RUN_TEST(test_a_valid_value_survives);
    RUN_TEST(test_a_changed_version_resets);
    RUN_TEST(test_a_changed_size_resets);
    RUN_TEST(test_reset_discards_the_value);

    return UNITY_END();
}
