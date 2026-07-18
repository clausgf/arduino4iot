/**
 * Native unit tests for IotTelemetry (pio test -e native).
 *
 * IotTelemetry only depends on ArduinoJson, so it can be tested
 * without an ESP32.
 */

#include <string>
#include <unity.h>

#include "iot_telemetry.h"

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

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_empty);
    RUN_TEST(test_add_numeric_values);
    RUN_TEST(test_add_overwrites_existing_key);
    RUN_TEST(test_add_string_value);
    RUN_TEST(test_measure_matches_serialized_size);
    RUN_TEST(test_clear);
    RUN_TEST(test_negative_and_large_values);
    return UNITY_END();
}
