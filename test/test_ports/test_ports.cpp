/**
 * Native unit tests for the ports layer (pio test -e native).
 *
 * Covers the Arduino-free logic (key validation, reset-reason projection, RTC
 * state validity) and the IotStore *contract*, written as
 * runStoreContract(IotStore&) so the same assertions can later run against
 * IotNvsStore on target: one contract, several adapters.
 */

#include <unity.h>

#include "../iot_fakes.h"
#include "iot_ports.h"
#include "iot_rtc_state.h"

// *****************************************************************************

void setUp() {}
void tearDown() {}


// *****************************************************************************
// IotStore: the contract every adapter must satisfy
// *****************************************************************************

static void runStoreContract(IotStore& store)
{
    store.clear();

    // absent keys report absent and yield the caller's default
    TEST_ASSERT_FALSE(store.has("missing"));
    TEST_ASSERT_EQUAL_STRING("fallback", store.getString("missing", "fallback").c_str());
    TEST_ASSERT_EQUAL_INT64(42, store.getInt("missing", 42));
    TEST_ASSERT_TRUE(store.getBool("missing", true));

    // round trip
    store.putString("token", "bearer abc");
    store.putInt("expiry", 1700000000LL);
    store.putBool("flag", true);
    TEST_ASSERT_TRUE(store.has("token"));
    TEST_ASSERT_EQUAL_STRING("bearer abc", store.getString("token").c_str());
    TEST_ASSERT_EQUAL_INT64(1700000000LL, store.getInt("expiry"));
    TEST_ASSERT_TRUE(store.getBool("flag"));

    // overwriting changes the value
    store.putString("token", "bearer xyz");
    TEST_ASSERT_EQUAL_STRING("bearer xyz", store.getString("token").c_str());

    // an empty string is a value, not an absence
    store.putString("empty", "");
    TEST_ASSERT_TRUE(store.has("empty"));
    TEST_ASSERT_EQUAL_STRING("", store.getString("empty", "default").c_str());

    // reading with the wrong type yields the default rather than garbage
    TEST_ASSERT_EQUAL_INT64(7, store.getInt("token", 7));
    TEST_ASSERT_EQUAL_STRING("d", store.getString("expiry", "d").c_str());

    // remove
    store.remove("token");
    TEST_ASSERT_FALSE(store.has("token"));
    store.remove("token");  // removing an absent key is not an error

    // clear wipes the namespace
    store.clear();
    TEST_ASSERT_FALSE(store.has("expiry"));
    TEST_ASSERT_FALSE(store.has("flag"));
}

void test_store_contract_on_fake()
{
    IotFakeStore store;
    runStoreContract(store);
}

void test_store_does_not_write_unchanged_values()
{
    // the port's central promise; this library writes from paths that run on
    // every single wakeup
    IotFakeStore store;
    store.putString("apiUrl", "https://example.org");
    store.putInt("boot", 17);
    store.putBool("flag", false);
    store.resetWriteCount();

    store.putString("apiUrl", "https://example.org");
    store.putInt("boot", 17);
    store.putBool("flag", false);
    TEST_ASSERT_EQUAL_UINT(0, store.writeCount());

    store.putInt("boot", 18);
    TEST_ASSERT_EQUAL_UINT(1, store.writeCount());
}

void test_store_key_validation()
{
    TEST_ASSERT_TRUE(IotStore::isValidKey("deviceToken"));
    TEST_ASSERT_TRUE(IotStore::isValidKey("123456789012345"));  // exactly 15
    TEST_ASSERT_FALSE(IotStore::isValidKey("1234567890123456")); // 16 exceeds NVS
    TEST_ASSERT_FALSE(IotStore::isValidKey(""));
    TEST_ASSERT_FALSE(IotStore::isValidKey(nullptr));
    TEST_ASSERT_FALSE(IotStore::isValidKey("with\tab"));
}

void test_store_namespaces_are_independent()
{
    IotFakeStore seedStore;
    IotFakeStore configStore;
    seedStore.putString("project", "seeded");
    configStore.putString("project", "mirrored");
    TEST_ASSERT_EQUAL_STRING("seeded", seedStore.getString("project").c_str());
    TEST_ASSERT_EQUAL_STRING("mirrored", configStore.getString("project").c_str());
}


// *****************************************************************************
// IotClock
// *****************************************************************************

void test_clock_plausibility_threshold()
{
    IotFakeClock clock;
    TEST_ASSERT_FALSE(clock.isPlausible());   // never set

    clock.setNow(1000);
    TEST_ASSERT_FALSE(clock.isPlausible());   // 1970, still implausible

    clock.setNow(1700000000LL);               // 2023
    TEST_ASSERT_TRUE(clock.isPlausible());
}

void test_clock_sync_records_last_sync_time()
{
    IotFakeClock clock;
    clock.setNow(1700000000LL);
    TEST_ASSERT_TRUE(clock.sync(5000));
    TEST_ASSERT_EQUAL_INT64(1700000000LL, clock.lastSyncTime());
    TEST_ASSERT_EQUAL_UINT(1, clock.syncCount());

    clock.syncResult = false;
    clock.advance(100);
    TEST_ASSERT_FALSE(clock.sync(5000));
    TEST_ASSERT_EQUAL_INT64(1700000000LL, clock.lastSyncTime());  // unchanged
}


// *****************************************************************************
// IotBattery: optional by construction
// *****************************************************************************

void test_absent_battery_is_a_null_pointer()
{
    // a device without a battery has no port - normal, not an error
    IotBattery* battery = nullptr;
    int milliVolts = (battery != nullptr) ? battery->readMilliVolts() : -1;
    TEST_ASSERT_EQUAL_INT(-1, milliVolts);
}

void test_callback_battery_wraps_a_lambda()
{
    int source_mV = 3812;
    IotCallbackBattery battery([&source_mV] { return source_mV; });
    TEST_ASSERT_EQUAL_INT(3812, battery.readMilliVolts());

    source_mV = 3650;
    TEST_ASSERT_EQUAL_INT(3650, battery.readMilliVolts());
}

void test_callback_battery_without_callable_reports_unavailable()
{
    IotCallbackBattery battery{ std::function<int()>() };
    TEST_ASSERT_EQUAL_INT(-1, battery.readMilliVolts());
}

void test_fake_battery_defaults_to_unavailable()
{
    IotFakeBattery battery;
    TEST_ASSERT_EQUAL_INT(-1, battery.readMilliVolts());
    battery.milliVolts = 4100;
    TEST_ASSERT_EQUAL_INT(4100, battery.readMilliVolts());
    TEST_ASSERT_EQUAL_UINT(2, battery.readCount());
}


// *****************************************************************************
// IotNetwork
// *****************************************************************************

void test_network_mac_is_available_without_a_connection()
{
    // the device id, and hence every API URL, derives from the MAC, so it must
    // be readable on the path where the connect failed
    IotFakeNetwork network;
    network.connectResult = false;
    TEST_ASSERT_FALSE(network.connect(1000));
    TEST_ASSERT_FALSE(network.isConnected());
    TEST_ASSERT_EQUAL_STRING("aabbccddeeff", network.macAddress().c_str());
}

void test_network_reports_metrics_only_while_connected()
{
    IotFakeNetwork network;
    TEST_ASSERT_EQUAL_INT(0, network.rssi());
    TEST_ASSERT_EQUAL_UINT32(0, network.lastConnectDuration_ms());

    TEST_ASSERT_TRUE(network.connect(1000));
    TEST_ASSERT_EQUAL_INT(-55, network.rssi());
    TEST_ASSERT_EQUAL_UINT32(350, network.lastConnectDuration_ms());
    TEST_ASSERT_EQUAL_STRING("192.168.1.42", network.localIp().c_str());
}


// *****************************************************************************
// IotPower
// *****************************************************************************

void test_power_records_the_exit_instead_of_taking_it()
{
    // the reason IotPower is a port: a host test reaches a failure path and
    // keeps running
    IotFakePower power;
    TEST_ASSERT_FALSE(power.wasCalled());

    power.deepSleep(300);
    TEST_ASSERT_TRUE(power.wasCalled());
    TEST_ASSERT_TRUE(IotFakePower::Action::DeepSleep == power.lastCall().action);
    TEST_ASSERT_EQUAL_UINT32(300, power.lastCall().duration_s);

    power.restart();
    TEST_ASSERT_TRUE(IotFakePower::Action::Restart == power.lastCall().action);
    TEST_ASSERT_EQUAL_size_t(2, power.calls().size());
}

void test_abnormal_reset_detection()
{
    TEST_ASSERT_TRUE(iotIsAbnormalReset(IotResetReason::Panic));
    TEST_ASSERT_TRUE(iotIsAbnormalReset(IotResetReason::Watchdog));
    TEST_ASSERT_TRUE(iotIsAbnormalReset(IotResetReason::Brownout));

    TEST_ASSERT_FALSE(iotIsAbnormalReset(IotResetReason::PowerOn));
    TEST_ASSERT_FALSE(iotIsAbnormalReset(IotResetReason::DeepSleep));
    TEST_ASSERT_FALSE(iotIsAbnormalReset(IotResetReason::Software));
    TEST_ASSERT_FALSE(iotIsAbnormalReset(IotResetReason::Unknown));
}

void test_reset_reason_strings()
{
    TEST_ASSERT_EQUAL_STRING("BROWNOUT", iotResetReasonToString(IotResetReason::Brownout));
    TEST_ASSERT_EQUAL_STRING("DEEP_SLEEP", iotResetReasonToString(IotResetReason::DeepSleep));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", iotResetReasonToString(IotResetReason::Unknown));
    TEST_ASSERT_EQUAL_STRING("TIMER", iotWakeupCauseToString(IotWakeupCause::Timer));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", iotWakeupCauseToString(IotWakeupCause::Unknown));
}


// *****************************************************************************
// IotRtcState
// *****************************************************************************

void test_rtc_state_starts_invalid_and_resets_to_a_defined_state()
{
    // simulate the memory as it is after a power-on reset: arbitrary bytes
    IotRtcState state;
    memset(&state, 0xa5, sizeof(state));
    TEST_ASSERT_FALSE(state.isValid());

    state.reset();
    TEST_ASSERT_TRUE(state.isValid());
    TEST_ASSERT_EQUAL_UINT32(0, state.bootCount);
    TEST_ASSERT_EQUAL_INT64(0, state.activeDuration_ms);
    TEST_ASSERT_EQUAL_INT32(0, state.wifiChannel);
    TEST_ASSERT_EQUAL_UINT32(0, state.dhcpIp);
    TEST_ASSERT_EQUAL_INT64(0, state.ntpLastSyncTime);

    // -1 means "ended in an orderly way": a fresh device is not in backoff
    TEST_ASSERT_EQUAL_INT32(-1, state.panicSleepDuration_s);
}

void test_rtc_state_survives_as_plain_data()
{
    IotRtcState state;
    state.reset();
    state.bootCount = 5;
    state.panicSleepDuration_s = 120;

    // what a deep sleep does to it: nothing
    IotRtcState afterSleep = state;
    TEST_ASSERT_TRUE(afterSleep.isValid());
    TEST_ASSERT_EQUAL_UINT32(5, afterSleep.bootCount);
    TEST_ASSERT_EQUAL_INT32(120, afterSleep.panicSleepDuration_s);
}

void test_rtc_state_magic_rejects_a_foreign_layout()
{
    IotRtcState state;
    state.reset();
    state.magic = IotRtcState::MAGIC + 1;  // a firmware with a changed layout
    TEST_ASSERT_FALSE(state.isValid());
}


// *****************************************************************************

int main(int argc, char **argv)
{
    UNITY_BEGIN();

    RUN_TEST(test_store_contract_on_fake);
    RUN_TEST(test_store_does_not_write_unchanged_values);
    RUN_TEST(test_store_key_validation);
    RUN_TEST(test_store_namespaces_are_independent);

    RUN_TEST(test_clock_plausibility_threshold);
    RUN_TEST(test_clock_sync_records_last_sync_time);

    RUN_TEST(test_absent_battery_is_a_null_pointer);
    RUN_TEST(test_callback_battery_wraps_a_lambda);
    RUN_TEST(test_callback_battery_without_callable_reports_unavailable);
    RUN_TEST(test_fake_battery_defaults_to_unavailable);

    RUN_TEST(test_network_mac_is_available_without_a_connection);
    RUN_TEST(test_network_reports_metrics_only_while_connected);

    RUN_TEST(test_power_records_the_exit_instead_of_taking_it);
    RUN_TEST(test_abnormal_reset_detection);
    RUN_TEST(test_reset_reason_strings);

    RUN_TEST(test_rtc_state_starts_invalid_and_resets_to_a_defined_state);
    RUN_TEST(test_rtc_state_survives_as_plain_data);
    RUN_TEST(test_rtc_state_magic_rejects_a_foreign_layout);

    return UNITY_END();
}
