/**
 * Native unit tests for IotApi (pio test -e native).
 *
 * The transport used to own an HTTPClient and a WiFiClientSecure, so none of
 * this could run without hardware. Against IotFakeHttp the decisions are plain
 * assertions: which URL a path becomes, which headers a request carries, and
 * what happens when the server answers 401.
 */

#include <unity.h>

#include "../iot_fakes.h"
#include "iot_api.h"
#include "iot_seed.h"

// *****************************************************************************

void setUp() {}
void tearDown() {}

/// A provisioned device: endpoint, project and a device token in the store.
struct Fixture
{
    IotFakeStore store;
    IotFakeHttp http;
    IotFakeClock clock;
    IotApi api;

    Fixture()
    {
        clock.setNow(1700000000LL);   // plausible, so token expiry arithmetic applies
        store.putString(iot_nvs_keys::API_URL, "https://api.example.org/api");
        store.putString(iot_nvs_keys::PROJECT, "myproject");
        store.putString(IotTokenManager::KEY_PROVISIONING_TOKEN, "P-102-secret");
        store.putString(IotTokenManager::KEY_DEVICE_TOKEN, "bearer old");
    }

    void begin()
    {
        api.begin(store, http, clock);
        api.setDeviceName("e32_aabbccddeeff");
    }
};

static std::string provisionBody(const char* token)
{
    return std::string("{\"tokenType\":\"bearer\",\"accessToken\":\"") + token
        + "\",\"expiresIn\":604800}";
}


// *****************************************************************************
// URL building
// *****************************************************************************

void test_placeholders_are_substituted()
{
    Fixture f;
    f.begin();
    TEST_ASSERT_EQUAL_STRING("https://api.example.org/api/telemetry/myproject/e32_aabbccddeeff/system",
        f.api.getApiUrlForPath("telemetry/{project}/{device}/system").c_str());
}

void test_a_missing_trailing_slash_in_the_base_url_is_added()
{
    Fixture f;
    f.store.putString(iot_nvs_keys::API_URL, "https://api.example.org/api");
    f.begin();
    TEST_ASSERT_EQUAL_STRING("https://api.example.org/api/provision",
        f.api.getApiUrlForPath("provision").c_str());

    Fixture g;
    g.store.putString(iot_nvs_keys::API_URL, "https://api.example.org/api/");
    g.begin();
    TEST_ASSERT_EQUAL_STRING("https://api.example.org/api/provision",
        g.api.getApiUrlForPath("provision").c_str());
}


// *****************************************************************************
// Headers
// *****************************************************************************

void test_a_request_carries_the_device_token()
{
    Fixture f;
    f.begin();
    std::string response;
    f.api.apiGet(response, "file/x");

    TEST_ASSERT_EQUAL_size_t(1, f.http.requestCount());
    TEST_ASSERT_EQUAL_STRING("bearer old", f.http.header(0, "Authorization").c_str());
    TEST_ASSERT_EQUAL_STRING("application/json", f.http.header(0, "Accept").c_str());
    TEST_ASSERT_EQUAL_STRING("GET", f.http.lastRequest().method);
}

void test_header_precedence_request_beats_default_beats_base()
{
    Fixture f;
    f.begin();
    f.api.setApiHeader({ { "Host", "proxy.example.org" }, { "Accept", "text/plain" } });

    std::string response;
    f.api.apiGet(response, "file/x", "", { { "Accept", "application/octet-stream" } });

    TEST_ASSERT_EQUAL_STRING("proxy.example.org", f.http.header(0, "Host").c_str());
    // the per-request header wins over setApiHeader(), which wins over the base
    TEST_ASSERT_EQUAL_STRING("application/octet-stream", f.http.header(0, "Accept").c_str());
}

void test_an_empty_header_value_suppresses_the_library_default()
{
    Fixture f;
    f.begin();
    std::string response;
    f.api.apiGet(response, "file/x", "", { { "Authorization", "" } });

    // the transport skips empty values, so nothing is sent for it
    TEST_ASSERT_EQUAL_STRING("", f.http.header(0, "Authorization").c_str());
}


// *****************************************************************************
// The 401 retry
// *****************************************************************************

void test_a_401_re_provisions_and_retries_once()
{
    Fixture f;
    f.begin();
    f.http.reply(401);                                    // the original request
    f.http.reply(200, provisionBody("fresh"));            // /provision
    f.http.reply(200, "payload");                         // the retry

    std::string response;
    int status = f.api.apiGet(response, "file/x");

    TEST_ASSERT_EQUAL_INT(200, status);
    TEST_ASSERT_EQUAL_STRING("payload", response.c_str());
    TEST_ASSERT_EQUAL_size_t(3, f.http.requestCount());

    // the retry carries the freshly issued token, not the rejected one
    TEST_ASSERT_EQUAL_STRING("bearer old", f.http.header(0, "Authorization").c_str());
    TEST_ASSERT_EQUAL_STRING("bearer fresh", f.http.header(2, "Authorization").c_str());
    TEST_ASSERT_EQUAL_STRING("bearer fresh", f.store.getString(IotTokenManager::KEY_DEVICE_TOKEN).c_str());
}

void test_the_provisioning_request_is_unauthenticated()
{
    Fixture f;
    f.begin();
    f.http.reply(401);
    f.http.reply(200, provisionBody("fresh"));
    f.http.reply(200);

    std::string response;
    f.api.apiGet(response, "file/x");

    // /provision authenticates with the provisioning token in the body
    TEST_ASSERT_EQUAL_STRING("", f.http.header(1, "Authorization").c_str());
    TEST_ASSERT_TRUE(f.http.request(1).url.find("/provision") != std::string::npos);
    TEST_ASSERT_TRUE(f.http.request(1).body.find("P-102-secret") != std::string::npos);
}

void test_a_second_401_is_not_retried_again()
{
    // one retry, not a loop: the request path cannot re-enter itself
    Fixture f;
    f.begin();
    f.http.defaultStatus = 401;
    f.http.reply(401);
    f.http.reply(200, provisionBody("fresh"));

    std::string response;
    int status = f.api.apiGet(response, "file/x");

    TEST_ASSERT_EQUAL_INT(401, status);
    TEST_ASSERT_EQUAL_size_t(3, f.http.requestCount());
}

void test_a_401_without_a_provisioning_token_is_passed_through()
{
    Fixture f;
    f.store.remove(IotTokenManager::KEY_PROVISIONING_TOKEN);
    f.begin();
    f.http.reply(401);

    std::string response;
    TEST_ASSERT_EQUAL_INT(401, f.api.apiGet(response, "file/x"));
    TEST_ASSERT_EQUAL_size_t(1, f.http.requestCount());   // no provisioning attempt
}

void test_a_403_keeps_the_token_and_does_not_retry()
{
    // 403 means the server config is wrong; re-provisioning would not help
    Fixture f;
    f.begin();
    f.http.reply(403);

    std::string response;
    TEST_ASSERT_EQUAL_INT(403, f.api.apiGet(response, "file/x"));
    TEST_ASSERT_EQUAL_size_t(1, f.http.requestCount());
    TEST_ASSERT_EQUAL_STRING("bearer old", f.store.getString(IotTokenManager::KEY_DEVICE_TOKEN).c_str());
}

void test_a_transport_error_is_passed_through()
{
    Fixture f;
    f.begin();
    f.http.reply(-11);

    std::string response;
    IotResult result = f.api.apiGet(response, "file/x");
    TEST_ASSERT_TRUE(result.isTransportError());
    TEST_ASSERT_EQUAL_INT(-11, result.transportError);
}


// *****************************************************************************
// Provisioning
// *****************************************************************************

void test_a_valid_token_is_not_renewed()
{
    Fixture f;
    f.store.putInt(IotTokenManager::KEY_DEVICE_TOKEN_EXPIRY, 4102444800LL);  // 2100
    f.begin();

    TEST_ASSERT_TRUE(f.api.updateProvisioning().isOk());
    TEST_ASSERT_EQUAL_size_t(0, f.http.requestCount());   // no request at all
}

void test_provisioning_without_a_token_reports_the_synthetic_status()
{
    Fixture f;
    f.store.remove(IotTokenManager::KEY_PROVISIONING_TOKEN);
    f.store.remove(IotTokenManager::KEY_DEVICE_TOKEN);
    f.begin();

    IotResult result = f.api.updateProvisioning();
    TEST_ASSERT_EQUAL_INT(IotResult::STATUS_NO_PROVISIONING_TOKEN, result.httpStatus);
    TEST_ASSERT_EQUAL_size_t(0, f.http.requestCount());
}

void test_a_2xx_with_an_unusable_body_is_malformed_not_success()
{
    Fixture f;
    f.store.remove(IotTokenManager::KEY_DEVICE_TOKEN);
    f.begin();
    f.http.reply(200, "{\"nothing\":\"useful\"}");

    IotResult result = f.api.updateProvisioning();
    TEST_ASSERT_EQUAL_INT(IotResult::STATUS_MALFORMED_RESPONSE, result.httpStatus);
    TEST_ASSERT_FALSE(f.api.deviceToken() == "bearer old");
}

void test_a_403_from_provision_is_reported_as_is()
{
    Fixture f;
    f.store.remove(IotTokenManager::KEY_DEVICE_TOKEN);
    f.begin();
    f.http.reply(403);

    IotResult result = f.api.updateProvisioning();
    TEST_ASSERT_EQUAL_INT(403, result.httpStatus);
    TEST_ASSERT_FALSE(result.isOk());
}


// *****************************************************************************
// Requests and responses
// *****************************************************************************

void test_post_sends_the_body_and_returns_the_response()
{
    Fixture f;
    f.begin();
    f.http.reply(201, "created");

    std::string response;
    int status = f.api.apiPost(response, "telemetry/{project}/{device}/system", "{\"x\":1}");

    TEST_ASSERT_EQUAL_INT(201, status);
    TEST_ASSERT_EQUAL_STRING("created", response.c_str());
    TEST_ASSERT_EQUAL_STRING("POST", f.http.lastRequest().method);
    TEST_ASSERT_EQUAL_STRING("{\"x\":1}", f.http.lastRequest().body.c_str());
}

void test_requested_response_headers_are_collected()
{
    Fixture f;
    f.begin();
    f.http.reply(200, "{}", { { "ETag", "\"abc\"" } });

    std::string response;
    std::map<std::string, std::string> responseHeader;
    f.api.apiGet(response, responseHeader, "file/config.json", { "ETag", "Last-Modified" });

    TEST_ASSERT_EQUAL_STRING("\"abc\"", responseHeader["ETag"].c_str());
    TEST_ASSERT_EQUAL_size_t(2, f.http.lastRequest().collectResponseHeaders.size());
}

void test_closing_the_connection_reaches_the_transport()
{
    Fixture f;
    f.begin();
    f.api.closeConnection();
    TEST_ASSERT_EQUAL_UINT(1, f.http.closeCount);
}


// *****************************************************************************

int main(int argc, char **argv)
{
    UNITY_BEGIN();

    RUN_TEST(test_placeholders_are_substituted);
    RUN_TEST(test_a_missing_trailing_slash_in_the_base_url_is_added);

    RUN_TEST(test_a_request_carries_the_device_token);
    RUN_TEST(test_header_precedence_request_beats_default_beats_base);
    RUN_TEST(test_an_empty_header_value_suppresses_the_library_default);

    RUN_TEST(test_a_401_re_provisions_and_retries_once);
    RUN_TEST(test_the_provisioning_request_is_unauthenticated);
    RUN_TEST(test_a_second_401_is_not_retried_again);
    RUN_TEST(test_a_401_without_a_provisioning_token_is_passed_through);
    RUN_TEST(test_a_403_keeps_the_token_and_does_not_retry);
    RUN_TEST(test_a_transport_error_is_passed_through);

    RUN_TEST(test_a_valid_token_is_not_renewed);
    RUN_TEST(test_provisioning_without_a_token_reports_the_synthetic_status);
    RUN_TEST(test_a_2xx_with_an_unusable_body_is_malformed_not_success);
    RUN_TEST(test_a_403_from_provision_is_reported_as_is);

    RUN_TEST(test_post_sends_the_body_and_returns_the_response);
    RUN_TEST(test_requested_response_headers_are_collected);
    RUN_TEST(test_closing_the_connection_reaches_the_transport);

    return UNITY_END();
}
