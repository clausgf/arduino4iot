/**
 * Native unit tests for IotTokenManager and the /provision response parser
 * (pio test -e native).
 *
 * The token lifetime rules used to be a handful of conditions inside
 * IotApi::updateProvisioning(), reachable only with a real server and a real
 * clock. They are plain decisions now, so the awkward cases - an unknown
 * expiry, an implausible clock, a token that expires during the next sleep -
 * can simply be asserted.
 */

#include <unity.h>

#include "iot_fakes.h"
#include "iot_token.h"

// *****************************************************************************

void setUp() {}
void tearDown() {}

static const int64_t NOW = 1700000000LL;  // 2023-11-14, comfortably plausible


// *****************************************************************************
// Parsing a /provision response
// *****************************************************************************

void test_parse_full_response()
{
    IotProvisioningResponse r;
    TEST_ASSERT_TRUE(iotParseProvisioningResponse(
        "{\"tokenType\":\"bearer\",\"accessToken\":\"abc123\",\"expiresIn\":604800}", r));
    TEST_ASSERT_TRUE(r.isValid());
    TEST_ASSERT_EQUAL_STRING("bearer", r.tokenType.c_str());
    TEST_ASSERT_EQUAL_STRING("abc123", r.accessToken.c_str());
    TEST_ASSERT_EQUAL_INT64(604800, r.expiresIn_s);
    TEST_ASSERT_EQUAL_STRING("bearer abc123", r.authorizationValue().c_str());
}

void test_parse_response_without_expiry()
{
    // a server that does not say: the token is usable, its lifetime unknown
    IotProvisioningResponse r;
    TEST_ASSERT_TRUE(iotParseProvisioningResponse(
        "{\"tokenType\":\"bearer\",\"accessToken\":\"abc123\"}", r));
    TEST_ASSERT_TRUE(r.isValid());
    TEST_ASSERT_EQUAL_INT64(-1, r.expiresIn_s);
}

void test_parse_rejects_malformed_bodies()
{
    IotProvisioningResponse r;
    TEST_ASSERT_FALSE(iotParseProvisioningResponse("", r));
    TEST_ASSERT_FALSE(iotParseProvisioningResponse(nullptr, r));
    TEST_ASSERT_FALSE(iotParseProvisioningResponse("not json", r));
    TEST_ASSERT_FALSE(iotParseProvisioningResponse("{}", r));
    TEST_ASSERT_FALSE(iotParseProvisioningResponse("{\"accessToken\":\"a\"}", r));
    TEST_ASSERT_FALSE(iotParseProvisioningResponse("{\"tokenType\":\"bearer\"}", r));
    TEST_ASSERT_FALSE(r.isValid());
}


// *****************************************************************************
// When a new token is needed
// *****************************************************************************

void test_unprovisioned_device_needs_a_token()
{
    IotFakeStore store;
    IotTokenManager tokens;
    tokens.begin(store);
    TEST_ASSERT_FALSE(tokens.hasDeviceToken());
    TEST_ASSERT_TRUE(tokens.needsNewToken(NOW, true));
}

void test_a_token_valid_beyond_the_margin_is_kept()
{
    IotFakeStore store;
    IotTokenManager tokens;
    tokens.begin(store);
    tokens.setDeviceToken("bearer abc", NOW + 7 * 24 * 3600);
    TEST_ASSERT_FALSE(tokens.needsNewToken(NOW, true));
}

void test_a_token_expiring_within_the_margin_is_renewed()
{
    // the margin exists so a device never wakes up with an expired token:
    // keep it larger than the sleep interval
    IotFakeStore store;
    IotTokenManager tokens;
    tokens.begin(store);
    tokens.setExpiryMargin_s(3600);

    tokens.setDeviceToken("bearer abc", NOW + 3601);
    TEST_ASSERT_FALSE(tokens.needsNewToken(NOW, true));

    tokens.setDeviceToken("bearer abc", NOW + 3599);
    TEST_ASSERT_TRUE(tokens.needsNewToken(NOW, true));
}

void test_an_expired_token_is_renewed()
{
    IotFakeStore store;
    IotTokenManager tokens;
    tokens.begin(store);
    tokens.setDeviceToken("bearer abc", NOW - 1);
    TEST_ASSERT_TRUE(tokens.needsNewToken(NOW, true));
}

void test_an_unknown_expiry_counts_as_still_good()
{
    // e.g. issued by an older library version; guessing "stale" would
    // re-provision on every single wakeup
    IotFakeStore store;
    IotTokenManager tokens;
    tokens.begin(store);
    tokens.setDeviceToken("bearer abc", 0);
    TEST_ASSERT_FALSE(tokens.needsNewToken(NOW, true));
}

void test_an_implausible_clock_counts_as_still_good()
{
    // the arithmetic would be meaningless before the NTP sync; an actually
    // expired token then surfaces as HTTP 401, which re-provisions
    IotFakeStore store;
    IotTokenManager tokens;
    tokens.begin(store);
    tokens.setDeviceToken("bearer abc", NOW + 60);
    TEST_ASSERT_FALSE(tokens.needsNewToken(1000, /*isTimePlausible=*/false));
    TEST_ASSERT_TRUE(tokens.needsNewToken(NOW, /*isTimePlausible=*/true));
}


// *****************************************************************************
// Persistence
// *****************************************************************************

void test_the_device_token_survives_a_restart()
{
    IotFakeStore store;
    {
        IotTokenManager tokens;
        tokens.begin(store);
        tokens.setDeviceToken("bearer abc", NOW + 600);
    }

    IotTokenManager afterRestart;
    afterRestart.begin(store);
    TEST_ASSERT_EQUAL_STRING("bearer abc", afterRestart.deviceToken().c_str());
    TEST_ASSERT_EQUAL_INT64(NOW + 600, afterRestart.deviceTokenExpiresAt());
}

void test_the_seeded_provisioning_token_is_loaded()
{
    IotFakeStore store;
    store.putString(IotTokenManager::KEY_PROVISIONING_TOKEN, "P-102-secret");

    IotTokenManager tokens;
    tokens.begin(store);
    TEST_ASSERT_TRUE(tokens.hasProvisioningToken());
    TEST_ASSERT_EQUAL_STRING("P-102-secret", tokens.provisioningToken().c_str());
}

void test_storing_the_same_token_again_writes_nothing()
{
    // this runs on every wakeup that re-provisions; NVS write cycles are finite
    IotFakeStore store;
    IotTokenManager tokens;
    tokens.begin(store);
    tokens.setDeviceToken("bearer abc", NOW + 600);
    store.resetWriteCount();

    tokens.setDeviceToken("bearer abc", NOW + 600);
    TEST_ASSERT_EQUAL_UINT(0, store.writeCount());

    tokens.setDeviceToken("bearer xyz", NOW + 600);
    TEST_ASSERT_TRUE(store.writeCount() > 0);
}

void test_clearing_the_token_forces_re_provisioning()
{
    IotFakeStore store;
    IotTokenManager tokens;
    tokens.begin(store);
    tokens.setDeviceToken("bearer abc", NOW + 7 * 24 * 3600);

    tokens.clearDeviceToken();

    TEST_ASSERT_FALSE(tokens.hasDeviceToken());
    TEST_ASSERT_TRUE(tokens.needsNewToken(NOW, true));
    TEST_ASSERT_EQUAL_STRING("", store.getString(IotTokenManager::KEY_DEVICE_TOKEN, "x").c_str());
}


// *****************************************************************************
// Adopting a response
// *****************************************************************************

void test_applying_a_response_computes_the_absolute_expiry()
{
    IotFakeStore store;
    IotTokenManager tokens;
    tokens.begin(store);

    IotProvisioningResponse r;
    TEST_ASSERT_TRUE(iotParseProvisioningResponse(
        "{\"tokenType\":\"bearer\",\"accessToken\":\"abc\",\"expiresIn\":604800}", r));

    int64_t expiresAt = tokens.applyProvisioningResponse(r, NOW, true);

    TEST_ASSERT_EQUAL_INT64(NOW + 604800, expiresAt);
    TEST_ASSERT_EQUAL_STRING("bearer abc", tokens.deviceToken().c_str());
    TEST_ASSERT_FALSE(tokens.needsNewToken(NOW, true));
}

void test_a_token_shorter_lived_than_the_margin_is_always_stale()
{
    // a deployment worth catching: if the server issues tokens whose lifetime
    // is below the renewal margin, every wakeup re-provisions. Not a bug in the
    // rule - the margin is a promise that the token outlives the next sleep -
    // but it means margin and server-side lifetime have to be chosen together.
    IotFakeStore store;
    IotTokenManager tokens;
    tokens.begin(store);
    tokens.setExpiryMargin_s(3600);

    IotProvisioningResponse r;
    iotParseProvisioningResponse(
        "{\"tokenType\":\"bearer\",\"accessToken\":\"abc\",\"expiresIn\":600}", r);
    tokens.applyProvisioningResponse(r, NOW, true);

    TEST_ASSERT_TRUE(tokens.needsNewToken(NOW, true));
}

void test_applying_a_response_without_a_plausible_clock_stores_no_expiry()
{
    // better an unknown expiry than a wrong one computed from a 1970 clock
    IotFakeStore store;
    IotTokenManager tokens;
    tokens.begin(store);

    IotProvisioningResponse r;
    iotParseProvisioningResponse(
        "{\"tokenType\":\"bearer\",\"accessToken\":\"abc\",\"expiresIn\":600}", r);

    int64_t expiresAt = tokens.applyProvisioningResponse(r, 1000, /*isTimePlausible=*/false);

    TEST_ASSERT_EQUAL_INT64(0, expiresAt);
    TEST_ASSERT_EQUAL_STRING("bearer abc", tokens.deviceToken().c_str());
}

void test_applying_a_response_without_expiry_stores_no_expiry()
{
    IotFakeStore store;
    IotTokenManager tokens;
    tokens.begin(store);

    IotProvisioningResponse r;
    iotParseProvisioningResponse("{\"tokenType\":\"bearer\",\"accessToken\":\"abc\"}", r);

    TEST_ASSERT_EQUAL_INT64(0, tokens.applyProvisioningResponse(r, NOW, true));
    TEST_ASSERT_FALSE(tokens.needsNewToken(NOW, true));
}


// *****************************************************************************

int main(int argc, char **argv)
{
    UNITY_BEGIN();

    RUN_TEST(test_parse_full_response);
    RUN_TEST(test_parse_response_without_expiry);
    RUN_TEST(test_parse_rejects_malformed_bodies);

    RUN_TEST(test_unprovisioned_device_needs_a_token);
    RUN_TEST(test_a_token_valid_beyond_the_margin_is_kept);
    RUN_TEST(test_a_token_expiring_within_the_margin_is_renewed);
    RUN_TEST(test_an_expired_token_is_renewed);
    RUN_TEST(test_an_unknown_expiry_counts_as_still_good);
    RUN_TEST(test_an_implausible_clock_counts_as_still_good);

    RUN_TEST(test_the_device_token_survives_a_restart);
    RUN_TEST(test_the_seeded_provisioning_token_is_loaded);
    RUN_TEST(test_storing_the_same_token_again_writes_nothing);
    RUN_TEST(test_clearing_the_token_forces_re_provisioning);

    RUN_TEST(test_applying_a_response_computes_the_absolute_expiry);
    RUN_TEST(test_a_token_shorter_lived_than_the_margin_is_always_stale);
    RUN_TEST(test_applying_a_response_without_a_plausible_clock_stores_no_expiry);
    RUN_TEST(test_applying_a_response_without_expiry_stores_no_expiry);

    return UNITY_END();
}
