/**
 * Native unit tests for the conditional GET (pio test -e native).
 *
 * The decisions of api.getCached() against IotFakeHttp: which validators a
 * request carries, which ones come back for the caller to commit, and how a
 * Cache-Control header becomes a max-age. Also IotConfig::updateConfig(), which
 * is built on it, and IotConfig::publishSchema() - both over the global api.
 */

#include <unity.h>

#include "../iot_fakes.h"
#include "iot_api.h"
#include "iot_cache.h"
#include "iot_config.h"
#include "iot_seed.h"

// *****************************************************************************

void setUp() {}
void tearDown() {}

/// A provisioned device, as in test_api.
struct Fixture
{
    IotFakeStore store;
    IotFakeHttp http;
    IotFakeClock clock;
    IotApi api;

    Fixture()
    {
        clock.setNow(1700000000LL);
        store.putString(iot_nvs_keys::API_URL, "https://api.example.org/api");
        store.putString(iot_nvs_keys::PROJECT, "myproject");
        store.putString(iot_nvs_keys::PROVISIONING_TOKEN, "P-102-secret");
        store.putString(iot_nvs_keys::DEVICE_TOKEN, "bearer old");
        api.begin(store, http, clock);
        api.setDeviceName("e32_aabbccddeeff");
    }

    IotCachedResponse get(const IotCacheValidators& current,
        const std::map<std::string, std::string>& header = {})
    {
        return api.getCached("forward/{project}/{device}/img/latest.png", current, { .headers = header });
    }
};


// *****************************************************************************
// The request
// *****************************************************************************

void test_without_validators_the_request_is_unconditional()
{
    Fixture f;
    f.get({});
    TEST_ASSERT_EQUAL_UINT(1, f.http.requestCount());
    TEST_ASSERT_EQUAL_STRING("GET", f.http.lastRequest().method);
    TEST_ASSERT_EQUAL_STRING("", f.http.header(0, "If-None-Match").c_str());
    TEST_ASSERT_EQUAL_STRING("", f.http.header(0, "If-Modified-Since").c_str());
}

void test_the_validators_become_conditional_headers()
{
    Fixture f;
    f.get({ "\"abc\"", "Wed, 21 Oct 2026 07:28:00 GMT" });
    TEST_ASSERT_EQUAL_STRING("\"abc\"", f.http.header(0, "If-None-Match").c_str());
    TEST_ASSERT_EQUAL_STRING("Wed, 21 Oct 2026 07:28:00 GMT", f.http.header(0, "If-Modified-Since").c_str());
}

void test_caller_headers_are_sent_and_win()
{
    Fixture f;
    f.get({ "\"abc\"", "" }, { { "Accept", "image/png" }, { "If-None-Match", "\"forced\"" } });
    TEST_ASSERT_EQUAL_STRING("image/png", f.http.header(0, "Accept").c_str());
    TEST_ASSERT_EQUAL_STRING("\"forced\"", f.http.header(0, "If-None-Match").c_str());
}

void test_the_cache_headers_are_collected()
{
    Fixture f;
    f.get({});
    const auto& keys = f.http.lastRequest().collectResponseHeaders;
    TEST_ASSERT_EQUAL_UINT(3, keys.size());
    TEST_ASSERT_EQUAL_STRING("ETag", keys[0].c_str());
    TEST_ASSERT_EQUAL_STRING("Last-Modified", keys[1].c_str());
    TEST_ASSERT_EQUAL_STRING("Cache-Control", keys[2].c_str());
}


// *****************************************************************************
// The response
// *****************************************************************************

void test_a_200_returns_the_body_new_validators_and_max_age()
{
    Fixture f;
    f.http.reply(200, "PNGDATA", { { "ETag", "\"new\"" }, { "Last-Modified", "Thu, 22 Oct 2026 08:00:00 GMT" },
        { "Cache-Control", "public, max-age=900" } });
    IotCachedResponse r = f.get({ "\"old\"", "" });
    TEST_ASSERT_TRUE(r.result.isOk());
    TEST_ASSERT_EQUAL_STRING("PNGDATA", r.body.c_str());
    TEST_ASSERT_EQUAL_STRING("\"new\"", r.validators.etag.c_str());
    TEST_ASSERT_EQUAL_STRING("Thu, 22 Oct 2026 08:00:00 GMT", r.validators.lastModified.c_str());
    TEST_ASSERT_EQUAL_INT32(900, r.maxAge_s);
}

void test_a_200_without_validators_returns_empty_ones()
{
    Fixture f;
    f.http.reply(200, "x");
    IotCachedResponse r = f.get({ "\"old\"", "Wed, 21 Oct 2026 07:28:00 GMT" });
    TEST_ASSERT_TRUE(r.validators.isEmpty());
    TEST_ASSERT_EQUAL_INT32(-1, r.maxAge_s);
}

void test_a_304_keeps_the_current_validators_and_has_no_body()
{
    Fixture f;
    f.http.reply(304, "stale", { { "Cache-Control", "max-age=60" } });
    IotCachedResponse r = f.get({ "\"abc\"", "Wed, 21 Oct 2026 07:28:00 GMT" });
    TEST_ASSERT_TRUE(r.result.isNotModified());
    TEST_ASSERT_FALSE(r.result.isOk());
    TEST_ASSERT_TRUE(r.result.isOkOrNotModified());
    TEST_ASSERT_EQUAL_STRING("", r.body.c_str());
    TEST_ASSERT_EQUAL_STRING("\"abc\"", r.validators.etag.c_str());
    TEST_ASSERT_EQUAL_STRING("Wed, 21 Oct 2026 07:28:00 GMT", r.validators.lastModified.c_str());
    TEST_ASSERT_EQUAL_INT32(60, r.maxAge_s);
}

void test_a_304_takes_over_a_resent_validator()
{
    Fixture f;
    f.http.reply(304, "", { { "ETag", "\"abc2\"" } });
    IotCachedResponse r = f.get({ "\"abc\"", "Wed, 21 Oct 2026 07:28:00 GMT" });
    TEST_ASSERT_EQUAL_STRING("\"abc2\"", r.validators.etag.c_str());
    TEST_ASSERT_EQUAL_STRING("Wed, 21 Oct 2026 07:28:00 GMT", r.validators.lastModified.c_str());
}

void test_an_error_keeps_the_current_validators()
{
    Fixture f;
    f.http.reply(500, "oops", { { "ETag", "\"err\"" }, { "Cache-Control", "max-age=60" } });
    IotCachedResponse r = f.get({ "\"abc\"", "" });
    TEST_ASSERT_TRUE(r.result.isHttpError());
    TEST_ASSERT_EQUAL_INT(500, r.result.httpStatus);
    TEST_ASSERT_EQUAL_STRING("", r.body.c_str());
    TEST_ASSERT_EQUAL_STRING("\"abc\"", r.validators.etag.c_str());
    TEST_ASSERT_EQUAL_INT32(-1, r.maxAge_s);
}

void test_a_transport_error_is_reported_as_such()
{
    Fixture f;
    f.http.reply(-1);
    IotCachedResponse r = f.get({ "\"abc\"", "" });
    TEST_ASSERT_TRUE(r.result.isTransportError());
    TEST_ASSERT_EQUAL_STRING("\"abc\"", r.validators.etag.c_str());
}

void test_response_header_names_are_case_insensitive()
{
    Fixture f;
    f.http.reply(200, "x", { { "etag", "\"lower\"" }, { "CACHE-CONTROL", "max-age=5" } });
    IotCachedResponse r = f.get({});
    TEST_ASSERT_EQUAL_STRING("\"lower\"", r.validators.etag.c_str());
    TEST_ASSERT_EQUAL_INT32(5, r.maxAge_s);
}

void test_a_401_re_provisions_and_retries_conditionally()
{
    Fixture f;
    f.http.reply(401);
    f.http.reply(200, "{\"tokenType\":\"bearer\",\"accessToken\":\"fresh\",\"expiresIn\":604800}");
    f.http.reply(304);
    IotCachedResponse r = f.get({ "\"abc\"", "" });
    TEST_ASSERT_TRUE(r.result.isNotModified());
    TEST_ASSERT_EQUAL_UINT(3, f.http.requestCount());
    TEST_ASSERT_EQUAL_STRING("\"abc\"", f.http.header(2, "If-None-Match").c_str());
    TEST_ASSERT_EQUAL_STRING("bearer fresh", f.http.header(2, "Authorization").c_str());
}


// *****************************************************************************
// Cache-Control
// *****************************************************************************

void test_max_age_is_parsed_from_cache_control()
{
    TEST_ASSERT_EQUAL_INT32(60, iotParseMaxAge("max-age=60"));
    TEST_ASSERT_EQUAL_INT32(300, iotParseMaxAge("public, max-age=300"));
    TEST_ASSERT_EQUAL_INT32(300, iotParseMaxAge("max-age=300 , must-revalidate"));
    TEST_ASSERT_EQUAL_INT32(5, iotParseMaxAge("MAX-AGE=5"));
    TEST_ASSERT_EQUAL_INT32(7, iotParseMaxAge("max-age=\"7\""));
    TEST_ASSERT_EQUAL_INT32(0, iotParseMaxAge("max-age=0"));
    TEST_ASSERT_EQUAL_INT32(INT32_MAX, iotParseMaxAge("max-age=99999999999"));
}

void test_no_usable_max_age_is_minus_one()
{
    TEST_ASSERT_EQUAL_INT32(-1, iotParseMaxAge(""));
    TEST_ASSERT_EQUAL_INT32(-1, iotParseMaxAge("public"));
    TEST_ASSERT_EQUAL_INT32(-1, iotParseMaxAge("s-maxage=10"));
    TEST_ASSERT_EQUAL_INT32(-1, iotParseMaxAge("max-age=abc"));
    TEST_ASSERT_EQUAL_INT32(-1, iotParseMaxAge("max-age=-5"));
    TEST_ASSERT_EQUAL_INT32(-1, iotParseMaxAge("max-age="));
}

void test_no_cache_and_no_store_override_max_age()
{
    TEST_ASSERT_EQUAL_INT32(-1, iotParseMaxAge("no-cache"));
    TEST_ASSERT_EQUAL_INT32(-1, iotParseMaxAge("max-age=60, no-store"));
    TEST_ASSERT_EQUAL_INT32(-1, iotParseMaxAge("No-Cache, max-age=60"));
}


// *****************************************************************************
// IotStoredValidators
// *****************************************************************************

void test_stored_validators_round_trip()
{
    IotFakeStore store;
    IotStoredValidators cache(store, "imgEtag", "imgDate");
    TEST_ASSERT_TRUE(cache.load().isEmpty());

    cache.commit({ "\"abc\"", "Wed, 21 Oct 2026 07:28:00 GMT" });
    TEST_ASSERT_EQUAL_STRING("\"abc\"", store.getString("imgEtag").c_str());
    IotCacheValidators loaded = cache.load();
    TEST_ASSERT_EQUAL_STRING("\"abc\"", loaded.etag.c_str());
    TEST_ASSERT_EQUAL_STRING("Wed, 21 Oct 2026 07:28:00 GMT", loaded.lastModified.c_str());

    cache.clear();
    TEST_ASSERT_TRUE(cache.load().isEmpty());
}

void test_committing_unchanged_validators_writes_nothing()
{
    IotFakeStore store;
    IotStoredValidators cache(store, "imgEtag", "imgDate");
    cache.commit({ "\"abc\"", "d" });
    store.resetWriteCount();
    cache.commit({ "\"abc\"", "d" });
    TEST_ASSERT_EQUAL_UINT(0, store.writeCount());
}


// *****************************************************************************
// IotConfig::updateConfig() over the global api
// *****************************************************************************

/// The global api on a fake transport, and a config with one value.
struct ConfigFixture
{
    IotFakeStore apiStore;
    IotFakeStore configStore;
    IotFakeHttp http;
    IotFakeClock clock;
    IotConfig cfg;
    IotConfigValue<int32_t> sleep_s{ cfg, 300, "sleep_s" };

    ConfigFixture()
    {
        clock.setNow(1700000000LL);
        apiStore.putString(iot_nvs_keys::API_URL, "https://api.example.org/api");
        apiStore.putString(iot_nvs_keys::PROJECT, "myproject");
        apiStore.putString(iot_nvs_keys::DEVICE_TOKEN, "bearer old");
        api.begin(apiStore, http, clock);
        api.setDeviceName("e32_aabbccddeeff");
        cfg.begin(configStore);
    }
    ~ConfigFixture() { api.end(); }
};

void test_update_config_applies_and_commits_the_validators()
{
    ConfigFixture f;
    TEST_ASSERT_FALSE(f.cfg.isMirrored());
    f.http.reply(200, "{\"sleep_s\":600}", { { "ETag", "\"c1\"" }, { "Last-Modified", "d1" } });
    IotResult r = f.cfg.updateConfig();
    TEST_ASSERT_TRUE(r.isOk());
    TEST_ASSERT_EQUAL_INT32(600, f.sleep_s.get());
    TEST_ASSERT_EQUAL_STRING("\"c1\"", f.cfg.validators().etag.c_str());
    TEST_ASSERT_EQUAL_STRING("d1", f.cfg.validators().lastModified.c_str());
    TEST_ASSERT_TRUE(f.cfg.isMirrored());
    TEST_ASSERT_EQUAL_STRING("https://api.example.org/api/file/myproject/e32_aabbccddeeff/config.json",
        f.http.lastRequest().url.c_str());

    f.http.reply(304);
    r = f.cfg.updateConfig();
    TEST_ASSERT_TRUE(r.isNotModified());
    TEST_ASSERT_EQUAL_STRING("\"c1\"", f.http.lastRequest().headers.at("If-None-Match").c_str());
    TEST_ASSERT_EQUAL_STRING("\"c1\"", f.cfg.validators().etag.c_str());
}

void test_update_config_with_a_malformed_body_keeps_the_old_validators()
{
    ConfigFixture f;
    f.configStore.putString(iot_nvs_keys::CONFIG_ETAG, "\"c1\"");
    f.http.reply(200, "not json", { { "ETag", "\"c2\"" } });
    IotResult r = f.cfg.updateConfig();
    TEST_ASSERT_EQUAL_INT(IotResult::STATUS_MALFORMED_RESPONSE, r.httpStatus);
    TEST_ASSERT_EQUAL_STRING("\"c1\"", f.cfg.validators().etag.c_str());
}


void test_publish_schema_uploads_once_per_schema()
{
    ConfigFixture f;
    f.http.reply(201);
    IotResult r = f.cfg.publishSchema();
    TEST_ASSERT_TRUE(r.isOk());
    TEST_ASSERT_EQUAL_STRING("PUT", f.http.lastRequest().method);
    TEST_ASSERT_EQUAL_STRING("https://api.example.org/api/file/myproject/e32_aabbccddeeff/config.schema.json",
        f.http.lastRequest().url.c_str());
    TEST_ASSERT_EQUAL_STRING("application/schema+json", f.http.header(0, "Content-Type").c_str());
    TEST_ASSERT_TRUE(f.http.lastRequest().body.find("\"sleep_s\"") != std::string::npos);

    // the same schema again: nothing sent
    r = f.cfg.publishSchema();
    TEST_ASSERT_TRUE(r.isNotModified());
    TEST_ASSERT_EQUAL_UINT(1, f.http.requestCount());
}

void test_a_failed_publish_is_retried_next_time()
{
    ConfigFixture f;
    f.http.reply(-1);
    TEST_ASSERT_TRUE(f.cfg.publishSchema().isTransportError());
    f.http.reply(201);
    TEST_ASSERT_TRUE(f.cfg.publishSchema().isOk());
    TEST_ASSERT_EQUAL_UINT(2, f.http.requestCount());
}

// *****************************************************************************

int main(int argc, char **argv)
{
    UNITY_BEGIN();

    RUN_TEST(test_without_validators_the_request_is_unconditional);
    RUN_TEST(test_the_validators_become_conditional_headers);
    RUN_TEST(test_caller_headers_are_sent_and_win);
    RUN_TEST(test_the_cache_headers_are_collected);

    RUN_TEST(test_a_200_returns_the_body_new_validators_and_max_age);
    RUN_TEST(test_a_200_without_validators_returns_empty_ones);
    RUN_TEST(test_a_304_keeps_the_current_validators_and_has_no_body);
    RUN_TEST(test_a_304_takes_over_a_resent_validator);
    RUN_TEST(test_an_error_keeps_the_current_validators);
    RUN_TEST(test_a_transport_error_is_reported_as_such);
    RUN_TEST(test_response_header_names_are_case_insensitive);
    RUN_TEST(test_a_401_re_provisions_and_retries_conditionally);

    RUN_TEST(test_max_age_is_parsed_from_cache_control);
    RUN_TEST(test_no_usable_max_age_is_minus_one);
    RUN_TEST(test_no_cache_and_no_store_override_max_age);

    RUN_TEST(test_stored_validators_round_trip);
    RUN_TEST(test_committing_unchanged_validators_writes_nothing);

    RUN_TEST(test_update_config_applies_and_commits_the_validators);
    RUN_TEST(test_update_config_with_a_malformed_body_keeps_the_old_validators);
    RUN_TEST(test_publish_schema_uploads_once_per_schema);
    RUN_TEST(test_a_failed_publish_is_retried_next_time);

    return UNITY_END();
}
