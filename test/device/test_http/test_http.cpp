/**
 * On-device tests of the HTTP adapters against the host's test server
 * (pio test -e device -f device/test_http): framing, keep-alive, the phase
 * and the bound of every kind of transport failure, TLS trust, and the
 * watchdog during a slow download.
 *
 * Most cases run against IotIdfHttp, the default; a subset also against the
 * fallback IotEspHttp.
 */

#include <esp_heap_caps.h>
#include <esp_task_wdt.h>

#include "../device_test_support.h"
#include "iot_idf_http.h"

static IotIdfHttp idfHttp;
static IotEspHttp espHttp;

static IotTlsConfig caTrust;
static IotTlsConfig otherCaTrust;
static IotTlsConfig insecureTrust;
static IotTlsConfig noTrust;
static IotTlsConfig bundleTrust;

static IotHttpResponse request(IotHttp& http, const char* method, const std::string& url,
    const std::string& body = "", std::vector<std::string> collect = {})
{
    IotHttpRequest request;
    request.method = method;
    request.url = url;
    request.body = body;
    request.collectResponseHeaders = collect;
    IotHttpResponse response;
    http.send(request, response);
    return response;
}

static IotHttpResponse get(IotHttp& http, const std::string& url, std::vector<std::string> collect = {})
{
    return request(http, "GET", url, "", collect);
}

static bool hasPattern(const std::string& body, size_t n)
{
    if (body.size() != n) { return false; }
    for (size_t i = 0; i < n; ++i)
    {
        if ((uint8_t)body[i] != i % 251) { return false; }
    }
    return true;
}

/// Back to the defaults and the CA trust, with no connection open.
static void reset(IotHttp& http)
{
    http.close();
    http.setTls(caTrust);
    http.setConnectTimeout_ms(5000);
    http.setHandshakeTimeout_ms(6000);
    http.setRequestTimeout_ms(5000);
    http.setResponseTimeout_ms(60000);
}

void setUp()
{
    deviceTestConnect();
    reset(idfHttp);
    reset(espHttp);
}

void tearDown() {}

// *****************************************************************************
// Framing and keep-alive (both adapters)
// *****************************************************************************

static void checkContentLength(IotHttp& http)
{
    IotHttpResponse r = get(http, deviceTestUrl("/hello"), { "ETag", "Last-Modified" });
    TEST_ASSERT_EQUAL(200, r.status);
    TEST_ASSERT_EQUAL_STRING("hello", r.body.c_str());
    TEST_ASSERT_EQUAL_STRING("\"e1\"", r.headers["ETag"].c_str());
    TEST_ASSERT_EQUAL_STRING("Tue, 10 Nov 2026 08:00:00 GMT", r.headers["Last-Modified"].c_str());
}

static void checkLargeAndChunked(IotHttp& http)
{
    IotHttpResponse r = get(http, deviceTestUrl("/bytes/40000"));
    TEST_ASSERT_EQUAL(200, r.status);
    TEST_ASSERT_TRUE(hasPattern(r.body, 40000));
    r = get(http, deviceTestUrl("/chunked/25000"));
    TEST_ASSERT_EQUAL(200, r.status);
    TEST_ASSERT_TRUE(hasPattern(r.body, 25000));
}

static void checkKeepAlive(IotHttp& http)
{
    IotHttpResponse first = get(http, deviceTestUrl("/hello"), { "X-Conn" });
    IotHttpResponse second = get(http, deviceTestUrl("/hello"), { "X-Conn" });
    TEST_ASSERT_EQUAL(200, second.status);
    TEST_ASSERT_EQUAL_STRING(first.headers["X-Conn"].c_str(), second.headers["X-Conn"].c_str());
}

void test_idf_content_length_and_headers() { checkContentLength(idfHttp); }
void test_esp_content_length_and_headers() { checkContentLength(espHttp); }
void test_idf_large_and_chunked_bodies() { checkLargeAndChunked(idfHttp); }
void test_esp_large_and_chunked_bodies() { checkLargeAndChunked(espHttp); }
void test_idf_keeps_the_connection() { checkKeepAlive(idfHttp); }
void test_esp_keeps_the_connection() { checkKeepAlive(espHttp); }

void test_idf_body_until_close()
{
    IotHttpResponse r = get(idfHttp, deviceTestUrl("/close/3000"));
    TEST_ASSERT_EQUAL(200, r.status);
    TEST_ASSERT_TRUE(hasPattern(r.body, 3000));
    // the server closed it: the next request needs a new connection and works
    TEST_ASSERT_EQUAL(200, get(idfHttp, deviceTestUrl("/hello")).status);
}

void test_idf_head_204_304_have_no_body()
{
    IotHttpResponse r = request(idfHttp, "HEAD", deviceTestUrl("/hello"));
    TEST_ASSERT_EQUAL(200, r.status);
    TEST_ASSERT_TRUE(r.body.empty());
    r = get(idfHttp, deviceTestUrl("/status/204"));
    TEST_ASSERT_EQUAL(204, r.status);
    r = get(idfHttp, deviceTestUrl("/status/304"));
    TEST_ASSERT_EQUAL(304, r.status);
    TEST_ASSERT_TRUE(r.body.empty());
    // still in sync: no body bytes were left over
    TEST_ASSERT_EQUAL_STRING("hello", get(idfHttp, deviceTestUrl("/hello")).body.c_str());
}

void test_idf_http_error_status_is_no_transport_error()
{
    IotHttpResponse r = get(idfHttp, deviceTestUrl("/status/404"));
    TEST_ASSERT_EQUAL(404, r.status);
    TEST_ASSERT_EQUAL((int)IotTransportPhase::None, (int)r.phase);
}

void test_idf_post_small_and_large_bodies()
{
    IotHttpRequest req;
    req.method = "POST";
    req.url = deviceTestUrl("/echo");
    req.headers = { { "X-Test", "abc" }, { "X-Skipped", "" } };
    req.collectResponseHeaders = { "X-Echo" };
    for (size_t n : { (size_t)0, (size_t)100, (size_t)20000 })  // 20000: not coalesced with the head
    {
        req.body.assign(n, 'z');
        IotHttpResponse r;
        TEST_ASSERT_EQUAL(200, idfHttp.send(req, r));
        const std::string expected = std::to_string(n) + ":" + std::to_string((n * 'z') & 0xFFFFFFFF);
        TEST_ASSERT_EQUAL_STRING(expected.c_str(), r.body.c_str());
        TEST_ASSERT_EQUAL_STRING("abc", r.headers["X-Echo"].c_str());
    }
}

void test_idf_repeats_a_request_the_server_dropped_on_a_reused_connection()
{
    IotHttpResponse armed = get(idfHttp, deviceTestUrl("/drop-next"), { "X-Conn" });
    TEST_ASSERT_EQUAL(200, armed.status);
    // the server closes the connection on the next request without answering
    IotHttpResponse r = get(idfHttp, deviceTestUrl("/hello"), { "X-Conn" });
    TEST_ASSERT_EQUAL(200, r.status);
    TEST_ASSERT_NOT_EQUAL(0, strcmp(armed.headers["X-Conn"].c_str(), r.headers["X-Conn"].c_str()));
}

// *****************************************************************************
// Failures: phase and bound
// *****************************************************************************

void test_idf_dns_failure()
{
    IotHttpResponse r = get(idfHttp, "http://no-such-host.invalid/x");
    TEST_ASSERT_EQUAL(IotIdfHttp::ERROR_CONNECT, r.status);
    TEST_ASSERT_EQUAL((int)IotTransportPhase::Dns, (int)r.phase);
}

void test_esp_dns_failure()
{
    IotHttpResponse r = get(espHttp, "http://no-such-host.invalid/x");
    TEST_ASSERT_TRUE(r.status < 0);
    TEST_ASSERT_EQUAL((int)IotTransportPhase::Dns, (int)r.phase);
}

void test_idf_closed_port()
{
    // refused, or - behind a host firewall that drops - timed out
    idfHttp.setConnectTimeout_ms(2000);
    const std::string url = std::string("http://") + DEVICE_TEST_HOST + ":"
        + std::to_string(DEVICE_TEST_CLOSED_PORT) + "/x";
    IotHttpResponse r = get(idfHttp, url);
    TEST_ASSERT_EQUAL(IotIdfHttp::ERROR_CONNECT, r.status);
    TEST_ASSERT_EQUAL((int)IotTransportPhase::Connect, (int)r.phase);
    TEST_ASSERT_LESS_THAN(2700, r.elapsed_ms);
}

void test_idf_connect_timeout()
{
    idfHttp.setConnectTimeout_ms(1500);
    // TEST-NET-3: routed nowhere, so the SYN goes unanswered
    IotHttpResponse r = get(idfHttp, "http://203.0.113.1/x");
    TEST_ASSERT_EQUAL(IotIdfHttp::ERROR_CONNECT, r.status);
    TEST_ASSERT_EQUAL((int)IotTransportPhase::Connect, (int)r.phase);
    TEST_ASSERT_INT_WITHIN(700, 1500, (int)r.elapsed_ms);
}

void test_idf_read_timeout_on_a_stalled_body()
{
    idfHttp.setRequestTimeout_ms(1500);
    IotHttpResponse r = get(idfHttp, deviceTestUrl("/stall"));
    TEST_ASSERT_EQUAL(IotIdfHttp::ERROR_READ_TIMEOUT, r.status);
    TEST_ASSERT_EQUAL((int)IotTransportPhase::Receive, (int)r.phase);
    TEST_ASSERT_INT_WITHIN(700, 1500, (int)r.elapsed_ms);
    TEST_ASSERT_TRUE(r.body.empty());
}

void test_idf_deadline_ends_a_trickling_body()
{
    idfHttp.setResponseTimeout_ms(3000);
    // a byte every 300 ms never hits the 5 s read timeout
    IotHttpResponse r = get(idfHttp, deviceTestUrl("/trickle-body/100/300"));
    TEST_ASSERT_EQUAL(IotIdfHttp::ERROR_RESPONSE_TIMEOUT, r.status);
    TEST_ASSERT_EQUAL((int)IotTransportPhase::Receive, (int)r.phase);
    TEST_ASSERT_INT_WITHIN(700, 3000, (int)r.elapsed_ms);
}

void test_idf_deadline_ends_trickling_headers()
{
    idfHttp.setResponseTimeout_ms(3000);
    IotHttpResponse r = get(idfHttp, deviceTestUrl("/trickle-head/300"));
    TEST_ASSERT_EQUAL(IotIdfHttp::ERROR_RESPONSE_TIMEOUT, r.status);
    TEST_ASSERT_INT_WITHIN(700, 3000, (int)r.elapsed_ms);
}

void test_idf_malformed_response()
{
    IotHttpResponse r = get(idfHttp, deviceTestUrl("/malformed"));
    TEST_ASSERT_EQUAL(IotIdfHttp::ERROR_MALFORMED_RESPONSE, r.status);
    TEST_ASSERT_EQUAL((int)IotTransportPhase::Receive, (int)r.phase);
}

void test_idf_invalid_url_and_missing_trust()
{
    TEST_ASSERT_EQUAL(IotIdfHttp::ERROR_INVALID_URL, get(idfHttp, "ftp://x/").status);
    idfHttp.setTls(noTrust);
    TEST_ASSERT_EQUAL(IotIdfHttp::ERROR_NO_SERVER_TRUST, get(idfHttp, deviceTestUrl("/hello", true)).status);
}

// *****************************************************************************
// TLS
// *****************************************************************************

void test_idf_https_with_the_pinned_ca()
{
    IotHttpResponse first = get(idfHttp, deviceTestUrl("/hello", true), { "X-Conn" });
    TEST_ASSERT_EQUAL(200, first.status);
    TEST_ASSERT_EQUAL_STRING("hello", first.body.c_str());
    IotHttpResponse second = get(idfHttp, deviceTestUrl("/bytes/20000", true), { "X-Conn" });
    TEST_ASSERT_EQUAL(200, second.status);
    TEST_ASSERT_TRUE(hasPattern(second.body, 20000));
    // one handshake for both
    TEST_ASSERT_EQUAL_STRING(first.headers["X-Conn"].c_str(), second.headers["X-Conn"].c_str());
}

void test_esp_https_with_the_pinned_ca()
{
    IotHttpResponse r = get(espHttp, deviceTestUrl("/hello", true));
    TEST_ASSERT_EQUAL(200, r.status);
    TEST_ASSERT_EQUAL_STRING("hello", r.body.c_str());
}

void test_idf_https_insecure()
{
    idfHttp.setTls(insecureTrust);
    TEST_ASSERT_EQUAL(200, get(idfHttp, deviceTestUrl("/hello", true)).status);
}

void test_idf_https_wrong_ca_is_a_tls_error()
{
    idfHttp.setTls(otherCaTrust);
    IotHttpResponse r = get(idfHttp, deviceTestUrl("/hello", true));
    TEST_ASSERT_EQUAL(IotIdfHttp::ERROR_CONNECT, r.status);
    TEST_ASSERT_EQUAL((int)IotTransportPhase::Tls, (int)r.phase);
    TEST_ASSERT_EQUAL(-0x2700, r.tlsError);  // MBEDTLS_ERR_X509_CERT_VERIFY_FAILED
}

void test_idf_tls_handshake_timeout()
{
    idfHttp.setHandshakeTimeout_ms(1500);
    const std::string url = std::string("https://") + DEVICE_TEST_HOST + ":"
        + std::to_string(DEVICE_TEST_STALL_PORT) + "/x";
    IotHttpResponse r = get(idfHttp, url);
    TEST_ASSERT_EQUAL(IotIdfHttp::ERROR_CONNECT, r.status);
    TEST_ASSERT_EQUAL((int)IotTransportPhase::Tls, (int)r.phase);
    TEST_ASSERT_EQUAL(0, r.tlsError);  // a timeout, not a TLS failure
    TEST_ASSERT_INT_WITHIN(700, 1500, (int)r.elapsed_ms);
}

void test_idf_bundle_trusts_a_public_server()
{
    idfHttp.setTls(bundleTrust);
    IotHttpResponse r = get(idfHttp, "https://www.google.com/generate_204");
    if (r.status < 0 && r.phase != IotTransportPhase::Tls)
    {
        TEST_IGNORE_MESSAGE("no internet access from the test network");
    }
    TEST_ASSERT_EQUAL(204, r.status);
}

/// @return the TLS error of a request to @p url under the bundle, or 1 if the
/// server is out of reach (no internet), so the caller can skip.
static int bundleTlsError(const char* url)
{
    idfHttp.close();
    idfHttp.setTls(bundleTrust);
    IotHttpResponse r = get(idfHttp, url);
    if (r.status < 0 && r.phase != IotTransportPhase::Tls) { return 1; }
    TEST_ASSERT_EQUAL_MESSAGE(IotIdfHttp::ERROR_CONNECT, r.status, url);
    return r.tlsError;
}

void test_idf_bundle_rejects_untrusted_and_mismatched_certificates()
{
    const int selfSigned = bundleTlsError("https://self-signed.badssl.com/");
    const int untrustedRoot = bundleTlsError("https://untrusted-root.badssl.com/");
    const int wrongHost = bundleTlsError("https://wrong.host.badssl.com/");
    if (selfSigned == 1 || untrustedRoot == 1 || wrongHost == 1)
    {
        TEST_IGNORE_MESSAGE("badssl.com out of reach");
    }
    // no CA of the bundle: its verify callback fails the chain
    TEST_ASSERT_EQUAL(-0x3000, selfSigned);     // MBEDTLS_ERR_X509_FATAL_ERROR
    TEST_ASSERT_EQUAL(-0x3000, untrustedRoot);
    // a trusted chain for another name
    TEST_ASSERT_EQUAL(-0x2700, wrongHost);      // MBEDTLS_ERR_X509_CERT_VERIFY_FAILED
}

// *****************************************************************************
// Throughput
// *****************************************************************************
// Resources
// *****************************************************************************

static size_t freeHeap() { return heap_caps_get_free_size(MALLOC_CAP_8BIT); }

/// One round of everything that allocates: a kept-alive request, a new
/// connection, a full TLS handshake, a failed handshake, a malformed
/// response. A leaked socket shows as a failed connect once lwIP runs out.
static void allocatingRound()
{
    TEST_ASSERT_EQUAL(200, get(idfHttp, deviceTestUrl("/hello")).status);
    TEST_ASSERT_EQUAL(200, get(idfHttp, deviceTestUrl("/bytes/4000")).status);
    idfHttp.close();
    TEST_ASSERT_EQUAL(200, get(idfHttp, deviceTestUrl("/hello", true)).status);
    idfHttp.setTls(otherCaTrust);
    TEST_ASSERT_EQUAL(IotIdfHttp::ERROR_CONNECT, get(idfHttp, deviceTestUrl("/hello", true)).status);
    idfHttp.setTls(caTrust);
    TEST_ASSERT_EQUAL(IotIdfHttp::ERROR_MALFORMED_RESPONSE, get(idfHttp, deviceTestUrl("/malformed")).status);
}

void test_idf_leaks_neither_memory_nor_sockets()
{
    // warm up: lwIP keeps closed connections in TIME_WAIT for a while, and
    // mbedTLS and the resolver allocate some state once
    for (int i = 0; i < 10; ++i) { allocatingRound(); }
    idfHttp.close();
    delay(200);
    const size_t before = freeHeap();

    const int rounds = 30;
    for (int i = 0; i < rounds; ++i) { allocatingRound(); }
    idfHttp.setRequestTimeout_ms(300);
    for (int i = 0; i < 3; ++i)
    {
        TEST_ASSERT_EQUAL(IotIdfHttp::ERROR_READ_TIMEOUT, get(idfHttp, deviceTestUrl("/stall")).status);
    }
    idfHttp.close();
    delay(200);
    const size_t after = freeHeap();

    char text[160];
    snprintf(text, sizeof(text), "free heap %u before, %u after %d rounds (%+d); minimum ever %u, largest block %u",
        (unsigned)before, (unsigned)after, rounds, (int)after - (int)before,
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    TEST_MESSAGE(text);
    // a leak of one allocation per round would be 30 of them; this allows
    // for TIME_WAIT connections and allocator noise, not for that
    TEST_ASSERT_INT_WITHIN_MESSAGE(2048, (int)before, (int)after, "free heap drifted");
}

void test_esp_leaks_neither_memory_nor_sockets()
{
    for (int i = 0; i < 5; ++i)
    {
        TEST_ASSERT_EQUAL(200, get(espHttp, deviceTestUrl("/hello")).status);
        TEST_ASSERT_EQUAL(200, get(espHttp, deviceTestUrl("/hello", true)).status);
        espHttp.close();
    }
    delay(200);
    const size_t before = freeHeap();
    for (int i = 0; i < 20; ++i)
    {
        TEST_ASSERT_EQUAL(200, get(espHttp, deviceTestUrl("/hello")).status);
        TEST_ASSERT_EQUAL(200, get(espHttp, deviceTestUrl("/hello", true)).status);
        espHttp.close();
    }
    delay(200);
    const size_t after = freeHeap();
    char text[100];
    snprintf(text, sizeof(text), "free heap %u before, %u after 20 rounds (%+d)",
        (unsigned)before, (unsigned)after, (int)after - (int)before);
    TEST_MESSAGE(text);
    TEST_ASSERT_INT_WITHIN_MESSAGE(2048, (int)before, (int)after, "free heap drifted");
}

// *****************************************************************************

static void measure(const char* name, IotHttp& http, bool https, size_t n)
{
    const std::string url = deviceTestUrl(("/bytes/" + std::to_string(n)).c_str(), https);
    get(http, url);  // connect (and handshake) outside of the measurement
    IotHttpResponse r = get(http, url);
    char text[160];
    snprintf(text, sizeof(text), "%s %-5s %6u bytes: status %d after %lu ms",
        name, https ? "https" : "http", (unsigned)n, r.status, (unsigned long)r.elapsed_ms);
    TEST_MESSAGE(text);
}

/// A measurement, not a check: the result depends on the radio link far more
/// than on the code (at -86 dBm, TCP retransmits dominate from ~20 KB on).
/// Compare runs at the same place; `pio test -v` shows the lines.
void test_throughput()
{
    char text[60];
    snprintf(text, sizeof(text), "rssi %d dBm", deviceTestWifi().rssi());
    TEST_MESSAGE(text);
    for (size_t n : { (size_t)8000, (size_t)40000 })
    {
        measure("IotIdfHttp", idfHttp, false, n);
        measure("IotIdfHttp", idfHttp, true, n);
        measure("IotEspHttp", espHttp, false, n);
        measure("IotEspHttp", espHttp, true, n);
    }
}

// *****************************************************************************
// Watchdog
// *****************************************************************************

void test_idf_feeds_the_watchdog_during_a_slow_download()
{
    // 40 KB in 1 KB pieces every 200 ms: 8 s, beyond a 3 s watchdog
    const esp_task_wdt_config_t config = { .timeout_ms = 3000, .idle_core_mask = 0, .trigger_panic = true };
    TEST_ASSERT_EQUAL(ESP_OK, esp_task_wdt_reconfigure(&config));
    const bool added = esp_task_wdt_status(nullptr) != ESP_OK && esp_task_wdt_add(nullptr) == ESP_OK;

    idfHttp.setResponseTimeout_ms(20000);
    IotHttpResponse r = get(idfHttp, deviceTestUrl("/progress/40960/200"));

    if (added) { esp_task_wdt_delete(nullptr); }
    TEST_ASSERT_EQUAL(200, r.status);
    TEST_ASSERT_TRUE(hasPattern(r.body, 40960));
    TEST_ASSERT_GREATER_THAN(6000, r.elapsed_ms);  // it did outlast the watchdog
}

// *****************************************************************************

void setup()
{
    caTrust.mode = IotTlsMode::CaPin;
    caTrust.caCertPem = DEVICE_TEST_CA_PEM;
    otherCaTrust.mode = IotTlsMode::CaPin;
    otherCaTrust.caCertPem = DEVICE_TEST_OTHER_CA_PEM;
    insecureTrust.mode = IotTlsMode::Insecure;
    bundleTrust.mode = IotTlsMode::Bundle;

    DEVICE_TEST_BEGIN();
    RUN_TEST(test_idf_content_length_and_headers);
    RUN_TEST(test_esp_content_length_and_headers);
    RUN_TEST(test_idf_large_and_chunked_bodies);
    RUN_TEST(test_esp_large_and_chunked_bodies);
    RUN_TEST(test_idf_keeps_the_connection);
    RUN_TEST(test_esp_keeps_the_connection);
    RUN_TEST(test_idf_body_until_close);
    RUN_TEST(test_idf_head_204_304_have_no_body);
    RUN_TEST(test_idf_http_error_status_is_no_transport_error);
    RUN_TEST(test_idf_post_small_and_large_bodies);
    RUN_TEST(test_idf_repeats_a_request_the_server_dropped_on_a_reused_connection);
    RUN_TEST(test_idf_dns_failure);
    RUN_TEST(test_esp_dns_failure);
    RUN_TEST(test_idf_closed_port);
    RUN_TEST(test_idf_connect_timeout);
    RUN_TEST(test_idf_read_timeout_on_a_stalled_body);
    RUN_TEST(test_idf_deadline_ends_a_trickling_body);
    RUN_TEST(test_idf_deadline_ends_trickling_headers);
    RUN_TEST(test_idf_malformed_response);
    RUN_TEST(test_idf_invalid_url_and_missing_trust);
    RUN_TEST(test_idf_https_with_the_pinned_ca);
    RUN_TEST(test_esp_https_with_the_pinned_ca);
    RUN_TEST(test_idf_https_insecure);
    RUN_TEST(test_idf_https_wrong_ca_is_a_tls_error);
    RUN_TEST(test_idf_tls_handshake_timeout);
    RUN_TEST(test_idf_bundle_trusts_a_public_server);
    RUN_TEST(test_idf_bundle_rejects_untrusted_and_mismatched_certificates);
    RUN_TEST(test_idf_leaks_neither_memory_nor_sockets);
    RUN_TEST(test_esp_leaks_neither_memory_nor_sockets);
    RUN_TEST(test_throughput);
    RUN_TEST(test_idf_feeds_the_watchdog_during_a_slow_download);
    UNITY_END();
}

void loop() {}
