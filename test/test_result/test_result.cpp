/**
 * Native unit tests for IotResult (pio test -e native).
 */

#include <unity.h>

#include "iot_result.h"

// *****************************************************************************

void setUp() {}
void tearDown() {}

// *****************************************************************************

void test_2xx_is_ok()
{
    IotResult r(200);
    TEST_ASSERT_TRUE(r.isOk());
    TEST_ASSERT_TRUE((bool)r);
    TEST_ASSERT_EQUAL_INT(200, r.httpStatus);
    TEST_ASSERT_EQUAL_INT(0, r.transportError);
}

void test_299_is_ok()
{
    IotResult r(299);
    TEST_ASSERT_TRUE(r.isOk());
}

void test_4xx_is_http_error()
{
    IotResult r(404);
    TEST_ASSERT_FALSE((bool)r);
    TEST_ASSERT_TRUE(r.isHttpError());
    TEST_ASSERT_FALSE(r.isTransportError());
    TEST_ASSERT_EQUAL_INT(404, r.httpStatus);
}

void test_304_is_http_error_not_ok()
{
    // 304 Not Modified is a valid HTTP response but not a 2xx success
    IotResult r(304);
    TEST_ASSERT_FALSE(r.isOk());
    TEST_ASSERT_TRUE(r.isHttpError());
    TEST_ASSERT_EQUAL_INT(304, r.httpStatus);
}

void test_negative_is_transport_error()
{
    IotResult r(-11); // e.g. HTTPC_ERROR_READ_TIMEOUT
    TEST_ASSERT_FALSE((bool)r);
    TEST_ASSERT_TRUE(r.isTransportError());
    TEST_ASSERT_EQUAL_INT(0, r.httpStatus);
    TEST_ASSERT_EQUAL_INT(-11, r.transportError);
}

void test_default_constructed_is_not_ok()
{
    IotResult r;
    TEST_ASSERT_FALSE((bool)r);
    TEST_ASSERT_TRUE(r.isTransportError());
}

void test_implicit_conversion_from_int()
{
    // a function returning IotResult can `return <int status>;`
    IotResult r = 201;
    TEST_ASSERT_TRUE(r.isOk());
}

void test_no_provisioning_token_sentinel()
{
    // client-side "no token" signal: not Ok, not a transport error, and
    // carried in httpStatus so a caller can identify it specifically
    IotResult r(IotResult::STATUS_NO_PROVISIONING_TOKEN);
    TEST_ASSERT_FALSE((bool)r);
    TEST_ASSERT_TRUE(r.isHttpError());
    TEST_ASSERT_FALSE(r.isTransportError());
    TEST_ASSERT_EQUAL_INT(IotResult::STATUS_NO_PROVISIONING_TOKEN, r.httpStatus);
}

void test_malformed_response_sentinel_distinct_from_ok()
{
    // a 2xx with a broken body must not read as success
    IotResult r(IotResult::STATUS_MALFORMED_RESPONSE);
    TEST_ASSERT_FALSE(r.isOk());
    TEST_ASSERT_TRUE(r.isHttpError());
    TEST_ASSERT_EQUAL_INT(IotResult::STATUS_MALFORMED_RESPONSE, r.httpStatus);
}

void test_sentinels_outside_real_http_range()
{
    // sentinels live above the real HTTP range so they never collide with a
    // server status code
    TEST_ASSERT_TRUE(IotResult::STATUS_NO_PROVISIONING_TOKEN >= 600);
    TEST_ASSERT_TRUE(IotResult::STATUS_MALFORMED_RESPONSE >= 600);
    TEST_ASSERT_TRUE(IotResult::STATUS_NO_PROVISIONING_TOKEN != IotResult::STATUS_MALFORMED_RESPONSE);
}

// *****************************************************************************

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_2xx_is_ok);
    RUN_TEST(test_299_is_ok);
    RUN_TEST(test_4xx_is_http_error);
    RUN_TEST(test_304_is_http_error_not_ok);
    RUN_TEST(test_negative_is_transport_error);
    RUN_TEST(test_default_constructed_is_not_ok);
    RUN_TEST(test_implicit_conversion_from_int);
    RUN_TEST(test_no_provisioning_token_sentinel);
    RUN_TEST(test_malformed_response_sentinel_distinct_from_ok);
    RUN_TEST(test_sentinels_outside_real_http_range);
    return UNITY_END();
}
