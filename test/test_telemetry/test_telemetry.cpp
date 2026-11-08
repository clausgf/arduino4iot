/**
 * Native unit tests for IotTelemetry (pio test -e native).
 *
 * IotTelemetry only depends on ArduinoJson, so it can be tested
 * without an ESP32.
 */

#include <string>
#include <unity.h>

#include "iot_telemetry.h"
#include "iot_rtc_slot.h"

#include <cstring>

// *****************************************************************************

void setUp() {}
void tearDown() {}

// *****************************************************************************

static std::string serialize(const IotTelemetry& telemetry)
{
    std::string out;
    telemetry.serializeTo(out);
    return out;
}

void test_empty()
{
    IotTelemetry telemetry;
    TEST_ASSERT_TRUE(telemetry.isEmpty());
}

void test_add_numeric_values()
{
    IotTelemetry telemetry;
    telemetry.add("temperature", 22.5).add("boot_count", 12);
    TEST_ASSERT_FALSE(telemetry.isEmpty());
    TEST_ASSERT_EQUAL_STRING("{\"temperature\":22.5,\"boot_count\":12}",
        serialize(telemetry).c_str());
}

void test_add_overwrites_existing_key()
{
    IotTelemetry telemetry;
    telemetry.add("value", 1);
    telemetry.add("value", 2);
    TEST_ASSERT_EQUAL_STRING("{\"value\":2}", serialize(telemetry).c_str());
}

void test_add_string_value()
{
    // strings are allowed, although the nice4iot backend ignores them
    IotTelemetry telemetry;
    telemetry.add("firmware_version", "1.2.3");
    TEST_ASSERT_EQUAL_STRING("{\"firmware_version\":\"1.2.3\"}",
        serialize(telemetry).c_str());
}

void test_measure_matches_serialized_size()
{
    IotTelemetry telemetry;
    telemetry.add("battery_V", 3.71).add("wifi_rssi", -67);
    TEST_ASSERT_EQUAL_UINT(serialize(telemetry).size(), telemetry.measure());
}

void test_clear()
{
    IotTelemetry telemetry;
    telemetry.add("temperature", 22.5);
    telemetry.clear();
    TEST_ASSERT_TRUE(telemetry.isEmpty());
}

void test_negative_and_large_values()
{
    IotTelemetry telemetry;
    telemetry.add("wifi_rssi", -67);
    telemetry.add("active_ms", (int64_t)8589934592ll); // > 32 bit
    TEST_ASSERT_EQUAL_STRING("{\"wifi_rssi\":-67,\"active_ms\":8589934592}",
        serialize(telemetry).c_str());
}

// *****************************************************************************

// *****************************************************************************
// IotRtcTelemetry
// *****************************************************************************

void test_rtc_telemetry_moves_into_the_next_post_as_last()
{
    struct AppState { IotRtcTelemetry<4> next; };
    IotRtcSlot<AppState> slot;
    std::memset(&slot, 0xA5, sizeof(slot));   // power-on contents
    TEST_ASSERT_TRUE(slot->next.isEmpty());

    TEST_ASSERT_TRUE(slot->next.add("refresh_ms", 15234u));
    TEST_ASSERT_TRUE(slot->next.add("battery_V", 3.71));

    IotTelemetry t;
    t.add("temperature", 21);
    slot->next.moveInto(t);
    TEST_ASSERT_TRUE(slot->next.isEmpty());

    std::string json;
    t.serializeTo(json);
    TEST_ASSERT_EQUAL_STRING("{\"temperature\":21,\"last_refresh_ms\":15234,\"last_battery_V\":3.71}", json.c_str());
}

void test_rtc_telemetry_overwrites_a_key_and_rejects_overflow()
{
    IotRtcTelemetry<2> next;
    std::memset(&next, 0, sizeof(next));
    TEST_ASSERT_TRUE(next.add("a", 1));
    TEST_ASSERT_TRUE(next.add("a", 2));       // same key: overwritten, no new place
    TEST_ASSERT_TRUE(next.add("b", 3));
    TEST_ASSERT_FALSE(next.add("c", 4));      // full
    TEST_ASSERT_FALSE(next.add("a_key_that_is_far_too_long", 5));

    IotTelemetry t;
    next.moveInto(t);
    std::string json;
    t.serializeTo(json);
    TEST_ASSERT_EQUAL_STRING("{\"last_a\":2,\"last_b\":3}", json.c_str());
}

void test_moving_an_empty_rtc_telemetry_adds_nothing()
{
    IotRtcTelemetry<2> next;
    std::memset(&next, 0, sizeof(next));
    IotTelemetry t;
    next.moveInto(t);
    TEST_ASSERT_TRUE(t.isEmpty());
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_empty);
    RUN_TEST(test_rtc_telemetry_moves_into_the_next_post_as_last);
    RUN_TEST(test_rtc_telemetry_overwrites_a_key_and_rejects_overflow);
    RUN_TEST(test_moving_an_empty_rtc_telemetry_adds_nothing);
    RUN_TEST(test_add_numeric_values);
    RUN_TEST(test_add_overwrites_existing_key);
    RUN_TEST(test_add_string_value);
    RUN_TEST(test_measure_matches_serialized_size);
    RUN_TEST(test_clear);
    RUN_TEST(test_negative_and_large_values);
    return UNITY_END();
}
