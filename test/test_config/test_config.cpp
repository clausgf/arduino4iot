/**
 * Native unit tests for IotConfig and IotConfigValue (pio test -e native).
 *
 * The interesting part is the precedence between a compile-time default, the
 * NVS mirror, config.json and an explicit set() - the rule that decides whether
 * an application's own setting survives the next config download.
 */

#include <unity.h>

#include "iot_config.h"
#include "iot_fakes.h"

// *****************************************************************************

void setUp() {}
void tearDown() {}

/// One IotConfig plus its values, wired to an in-memory store.
///
/// Values register themselves globally from their constructors, filtered by
/// owner, so each fixture must be a distinct IotConfig instance - which is also
/// what the multi-instance support means in practice.
struct Fixture
{
    IotFakeStore store;
    IotConfig config;
    IotConfigValue<int32_t> sleep_s{ config, 300, "sleep_s" };
    IotConfigValue<int32_t> ledPin{ config, -1, "led_pin" };
    IotConfigValue<bool> verbose{ config, false, "verbose" };
    IotConfigValue<std::string> server{ config, "pool.ntp.org", "ntp_server1" };

    void begin() { config.begin(store); }
};


// *****************************************************************************
// Precedence
// *****************************************************************************

void test_compile_time_default_applies_without_anything_stored()
{
    Fixture f;
    f.begin();
    TEST_ASSERT_EQUAL_INT32(300, f.sleep_s.get());
    TEST_ASSERT_FALSE(f.verbose.get());
    TEST_ASSERT_EQUAL_STRING("pool.ntp.org", f.server.get().c_str());
}

void test_the_nvs_mirror_beats_the_compile_time_default()
{
    Fixture f;
    f.store.putInt("sleep_s", 900);
    f.store.putString("ntp_server1", "192.168.1.1");

    f.begin();

    TEST_ASSERT_EQUAL_INT32(900, f.sleep_s.get());
    TEST_ASSERT_EQUAL_STRING("192.168.1.1", f.server.get().c_str());
    TEST_ASSERT_EQUAL_INT32(-1, f.ledPin.get());  // untouched key keeps its default
}

void test_config_json_beats_the_nvs_mirror()
{
    Fixture f;
    f.store.putInt("sleep_s", 900);
    f.begin();

    TEST_ASSERT_EQUAL_INT(1, f.config.applyJsonBody("{\"sleep_s\":120}"));

    TEST_ASSERT_EQUAL_INT32(120, f.sleep_s.get());
    TEST_ASSERT_EQUAL_INT64(120, f.store.getInt("sleep_s"));  // mirrored
}

void test_an_explicit_set_beats_config_json()
{
    // the application owns this value for the rest of the cycle
    Fixture f;
    f.begin();
    f.sleep_s.set(60);
    TEST_ASSERT_TRUE(f.sleep_s.isOverridden());

    f.config.applyJsonBody("{\"sleep_s\":900}");

    TEST_ASSERT_EQUAL_INT32(60, f.sleep_s.get());
}

void test_an_overridden_value_is_still_mirrored_for_the_next_boot()
{
    // the override is per cycle, not a veto: the server value reaches NVS and
    // applies from the next boot
    Fixture f;
    f.begin();
    f.sleep_s.set(60);

    f.config.applyJsonBody("{\"sleep_s\":900}");
    TEST_ASSERT_EQUAL_INT64(900, f.store.getInt("sleep_s"));

    Fixture nextBoot;
    nextBoot.store.putInt("sleep_s", 900);
    nextBoot.begin();
    TEST_ASSERT_EQUAL_INT32(900, nextBoot.sleep_s.get());
}

void test_an_explicit_set_before_begin_is_not_overwritten()
{
    // the trap this rule exists for: configuring a value in setup() and having
    // begin() silently replace it from the NVS mirror
    Fixture f;
    f.store.putInt("led_pin", 2);
    f.ledPin.set(13);

    f.begin();

    TEST_ASSERT_EQUAL_INT32(13, f.ledPin.get());
}

void test_assignment_counts_as_an_explicit_set()
{
    Fixture f;
    f.begin();
    f.sleep_s = 42;
    TEST_ASSERT_TRUE(f.sleep_s.isOverridden());
    f.config.applyJsonBody("{\"sleep_s\":900}");
    TEST_ASSERT_EQUAL_INT32(42, f.sleep_s.get());
}

void test_clearing_the_override_hands_the_value_back_to_the_server()
{
    Fixture f;
    f.begin();
    f.sleep_s.set(60);
    f.sleep_s.clearOverride();

    f.config.applyJsonBody("{\"sleep_s\":900}");

    TEST_ASSERT_EQUAL_INT32(900, f.sleep_s.get());
}


// *****************************************************************************
// Applying a configuration body
// *****************************************************************************

void test_apply_reports_unknown_keys_and_type_mismatches()
{
    // one bad entry must not discard the rest of the file
    Fixture f;
    f.begin();

    int unknown = 0, mismatched = 0;
    int applied = f.config.applyJsonBody(
        "{\"sleep_s\":120,\"nonesuch\":1,\"led_pin\":\"two\",\"verbose\":true}",
        &unknown, &mismatched);

    TEST_ASSERT_EQUAL_INT(2, applied);      // sleep_s and verbose
    TEST_ASSERT_EQUAL_INT(1, unknown);      // nonesuch
    TEST_ASSERT_EQUAL_INT(1, mismatched);   // led_pin is not a string
    TEST_ASSERT_EQUAL_INT32(120, f.sleep_s.get());
    TEST_ASSERT_TRUE(f.verbose.get());
    TEST_ASSERT_EQUAL_INT32(-1, f.ledPin.get());
}

void test_apply_rejects_a_body_that_is_not_a_json_object()
{
    Fixture f;
    f.begin();
    TEST_ASSERT_EQUAL_INT(-1, f.config.applyJsonBody("not json"));
    TEST_ASSERT_EQUAL_INT(-1, f.config.applyJsonBody("[1,2,3]"));
    TEST_ASSERT_EQUAL_INT(-1, f.config.applyJsonBody(""));
}

void test_a_bool_is_not_accepted_as_an_int()
{
    Fixture f;
    f.begin();
    int mismatched = 0;
    f.config.applyJsonBody("{\"sleep_s\":true}", nullptr, &mismatched);
    TEST_ASSERT_EQUAL_INT(1, mismatched);
    TEST_ASSERT_EQUAL_INT32(300, f.sleep_s.get());
}

void test_applying_the_same_value_again_writes_nothing()
{
    Fixture f;
    f.begin();
    f.config.applyJsonBody("{\"sleep_s\":120}");
    f.store.resetWriteCount();

    f.config.applyJsonBody("{\"sleep_s\":120}");

    TEST_ASSERT_EQUAL_UINT(0, f.store.writeCount());
}


// *****************************************************************************
// Keys and instances
// *****************************************************************************

void test_every_key_fits_the_store()
{
    // one key names the value in config.json, in NVS and in the free-form
    // accessors, so it has to fit the NVS limit
    const char* keys[] = {
        // Iot
        "log_level", "sleep_s", "watchdog_s", "ntp_timeout_ms", "battery_min_mv",
        "panic_init_s", "panic_factor", "panic_max_s",
        // IotNtpClock
        "ntp_resync_s", "ntp_server1", "ntp_server2", "ntp_server3",
        // IotConfig itself
        "iotCfgEtag", "iotCfgDate",
    };
    for (const char* key : keys)
    {
        TEST_ASSERT_TRUE_MESSAGE(IotStore::isValidKey(key), key);
    }
}

void test_values_are_bound_to_their_own_config_instance()
{
    Fixture a;
    Fixture b;
    a.begin();
    b.begin();

    a.config.applyJsonBody("{\"sleep_s\":111}");

    TEST_ASSERT_EQUAL_INT32(111, a.sleep_s.get());
    TEST_ASSERT_EQUAL_INT32(300, b.sleep_s.get());   // untouched
    TEST_ASSERT_EQUAL_INT64(0, b.store.getInt("sleep_s", 0));
}

void test_free_form_access_uses_the_same_key_space()
{
    Fixture f;
    f.begin();
    f.config.applyJsonBody("{\"sleep_s\":120}");

    // no second naming scheme: the config.json key is the storage key
    TEST_ASSERT_EQUAL_INT64(120, f.config.getInt("sleep_s"));

    f.config.setString("app_note", "hello");
    TEST_ASSERT_EQUAL_STRING("hello", f.config.getString("app_note").c_str());
}

void test_an_unbound_config_returns_defaults_instead_of_crashing()
{
    IotConfig unbound;
    TEST_ASSERT_EQUAL_INT64(7, unbound.getInt("anything", 7));
    TEST_ASSERT_EQUAL_STRING("d", unbound.getString("anything", "d").c_str());
    TEST_ASSERT_EQUAL_INT(-1, unbound.applyJsonBody("{\"sleep_s\":1}"));
    TEST_ASSERT_EQUAL_STRING("", unbound.etag().c_str());
}


// *****************************************************************************

int main(int argc, char **argv)
{
    UNITY_BEGIN();

    RUN_TEST(test_compile_time_default_applies_without_anything_stored);
    RUN_TEST(test_the_nvs_mirror_beats_the_compile_time_default);
    RUN_TEST(test_config_json_beats_the_nvs_mirror);
    RUN_TEST(test_an_explicit_set_beats_config_json);
    RUN_TEST(test_an_overridden_value_is_still_mirrored_for_the_next_boot);
    RUN_TEST(test_an_explicit_set_before_begin_is_not_overwritten);
    RUN_TEST(test_assignment_counts_as_an_explicit_set);
    RUN_TEST(test_clearing_the_override_hands_the_value_back_to_the_server);

    RUN_TEST(test_apply_reports_unknown_keys_and_type_mismatches);
    RUN_TEST(test_apply_rejects_a_body_that_is_not_a_json_object);
    RUN_TEST(test_a_bool_is_not_accepted_as_an_int);
    RUN_TEST(test_applying_the_same_value_again_writes_nothing);

    RUN_TEST(test_every_key_fits_the_store);
    RUN_TEST(test_values_are_bound_to_their_own_config_instance);
    RUN_TEST(test_free_form_access_uses_the_same_key_space);
    RUN_TEST(test_an_unbound_config_returns_defaults_instead_of_crashing);

    return UNITY_END();
}
