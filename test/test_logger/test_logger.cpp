/**
 * Native unit tests for IotLogger (pio test -e native).
 *
 * The logger posts through the global api, here over an IotFakeHttp. What it
 * cannot send goes to the RTC log, which on the host is a plain static - a
 * new IotLogger stands in for the next wakeup cycle, which finds the lines
 * there.
 */

#include <string>
#include <vector>
#include <unity.h>

#include "../iot_fakes.h"
#include "iot_api.h"
#include "iot_logger.h"
#include "iot_telemetry.h"

/// The logger's private side, as Iot uses it.
struct IotLoggerTest
{
    static void setNetwork(IotLogger& logger, IotNetwork& network) { logger.setNetwork(network); }
    static void setCycleName(IotLogger& logger, std::string (*name)()) { logger._offlineContext = name; }
    static void logToRtc(IotLogger& logger, const char* text) { logger.logToRtc(IotLogger::Level::Error, "rtc", "%s", text); }
};

static IotFakeStore store;
static IotFakeHttp http;
static IotFakeClock clock_;
static IotFakeNetwork network;

static std::string cycleName() { return "boot 7"; }

/// The request bodies sent to the log endpoint, time stamps replaced by "T".
static std::vector<std::string> sentLogs()
{
    std::vector<std::string> bodies;
    for (const IotHttpRequest& request : http.requests)
    {
        if (request.url.find("/log/") == std::string::npos) { continue; }
        std::string body;
        const std::string& raw = request.body;
        for (size_t i = 0; i < raw.size(); ++i)
        {
            body += raw[i];
            if (raw[i] == '(' && i + 1 < raw.size() && isdigit((unsigned char)raw[i + 1]))
            {
                while (i + 1 < raw.size() && isdigit((unsigned char)raw[i + 1])) { ++i; }
                body += 'T';
            }
        }
        bodies.push_back(body);
    }
    return bodies;
}

static std::string allSent()
{
    std::string all;
    for (const std::string& body : sentLogs()) { all += body; }
    return all;
}

/// A logger for one wakeup cycle, online unless told otherwise.
static IotLogger& newCycle(IotLogger& logger, bool online = true)
{
    network.setConnected(online);
    IotLoggerTest::setNetwork(logger, network);
    IotLoggerTest::setCycleName(logger, cycleName);
    return logger;
}

void setUp()
{
    store = IotFakeStore();
    clock_.setNow(1700000000LL);
    store.putString(iot_nvs_keys::API_URL, "https://api.example.org/api");
    store.putString(iot_nvs_keys::PROJECT, "myproject");
    store.putString(iot_nvs_keys::DEVICE_TOKEN, "bearer token");
    http = IotFakeHttp();
    api.begin(store, http, clock_);
    api.setDeviceName("e32_aabbccddeeff");

    // whatever an earlier test left in the RTC log goes out here
    IotLogger drain;
    newCycle(drain).flush();
    http.requests.clear();
}

void tearDown() {}

// *****************************************************************************
// Lines
// *****************************************************************************

void test_lines_have_the_esp_log_format()
{
    IotLogger logger;
    newCycle(logger);
    logger.error("tag", "e %d", 1);
    logger.warn("tag", "w");
    logger.info("tag", "i");
    logger.debug("tag", "d");
    logger.verbose("tag", "v %s", "x");
    TEST_ASSERT_TRUE(logger.flush().isOk());
    TEST_ASSERT_EQUAL(1, sentLogs().size());
    TEST_ASSERT_EQUAL_STRING(
        "E (T) tag: e 1\nW (T) tag: w\nI (T) tag: i\nD (T) tag: d\nV (T) tag: v x\n",
        sentLogs()[0].c_str());
    TEST_ASSERT_EQUAL_STRING("https://api.example.org/api/log/myproject/e32_aabbccddeeff",
        http.lastRequest().url.c_str());
    TEST_ASSERT_EQUAL_STRING("text/plain", http.header(0, "Content-Type").c_str());
}

void test_a_message_is_one_line()
{
    IotLogger logger;
    newCycle(logger);
    logger.info("tag", "first\nsecond\r\nthird\r\n\n");
    logger.flush();
    TEST_ASSERT_EQUAL_STRING("I (T) tag: first second  third\n", allSent().c_str());
}

void test_a_long_message_is_cut_not_overflowing()
{
    IotLogger logger;
    newCycle(logger);
    logger.info("tag", "%s", std::string(1000, 'x').c_str());
    logger.flush();
    const std::string sent = allSent();
    TEST_ASSERT_TRUE(sent.size() < 170);
    TEST_ASSERT_EQUAL_CHAR('\n', sent.back());
}

void test_lines_above_the_level_are_dropped()
{
    IotLogger logger;
    newCycle(logger);
    logger.setLevel(IotLogger::Level::Warning);
    logger.error("tag", "kept");
    logger.warn("tag", "kept too");
    logger.info("tag", "dropped");
    logger.debug("tag", "dropped");
    logger.flush();
    TEST_ASSERT_EQUAL_STRING("E (T) tag: kept\nW (T) tag: kept too\n", allSent().c_str());
}

// *****************************************************************************
// Buffered
// *****************************************************************************

void test_buffered_lines_go_out_in_one_request_on_flush()
{
    IotLogger logger;
    newCycle(logger);
    logger.info("a", "1");
    logger.info("b", "2");
    TEST_ASSERT_EQUAL(0, http.requestCount());
    TEST_ASSERT_TRUE(logger.flush().isOk());
    TEST_ASSERT_EQUAL(1, http.requestCount());

    // nothing left: no request, and still Ok
    TEST_ASSERT_TRUE(logger.flush().isOk());
    TEST_ASSERT_EQUAL(1, http.requestCount());
}

void test_a_full_buffer_is_flushed_before_the_size_limit()
{
    IotLogger logger;
    newCycle(logger);
    std::string expected;
    const std::string text(90, 'x');
    for (int i = 0; i < 200; ++i)  // ~20 KB
    {
        logger.info("tag", "%03d %s", i, text.c_str());
        char line[128];
        snprintf(line, sizeof(line), "I (T) tag: %03d %s\n", i, text.c_str());
        expected += line;
    }
    TEST_ASSERT_TRUE(http.requestCount() >= 2);
    logger.flush();
    for (const IotHttpRequest& request : http.requests)
    {
        TEST_ASSERT_TRUE(request.body.size() <= IOT_MAX_TELEMETRY_SIZE);
    }
    TEST_ASSERT_EQUAL_STRING(expected.c_str(), allSent().c_str());
}

// *****************************************************************************
// Offline and failures
// *****************************************************************************

void test_offline_lines_go_out_first_in_the_next_cycle()
{
    {
        IotLogger offline;
        newCycle(offline, false);
        offline.info("tag", "while offline");
        TEST_ASSERT_TRUE(offline.flush().isTransportError());
        TEST_ASSERT_EQUAL(0, http.requestCount());
    }
    IotLogger next;
    newCycle(next);
    next.info("tag", "now");
    TEST_ASSERT_TRUE(next.flush().isOk());
    TEST_ASSERT_EQUAL(1, sentLogs().size());
    TEST_ASSERT_EQUAL_STRING(
        "I (T) iot: --- kept offline: boot 7 ---\nI (T) tag: while offline\nI (T) tag: now\n",
        sentLogs()[0].c_str());
}

void test_lines_of_a_5xx_are_kept()
{
    IotLogger logger;
    newCycle(logger);
    logger.info("tag", "server down");
    http.reply(503);
    TEST_ASSERT_FALSE(logger.flush().isOk());

    http.requests.clear();
    IotLogger next;
    newCycle(next);
    next.flush();
    TEST_ASSERT_TRUE(allSent().find("I (T) tag: server down") != std::string::npos);
}

void test_lines_of_a_4xx_are_dropped()
{
    // the server will not take them in a later cycle either
    IotLogger logger;
    newCycle(logger);
    logger.info("tag", "rejected");
    http.reply(400);
    TEST_ASSERT_FALSE(logger.flush().isOk());

    http.requests.clear();
    IotLogger next;
    newCycle(next);
    next.flush();
    TEST_ASSERT_EQUAL(0, http.requestCount());
}

void test_a_transport_error_keeps_the_lines()
{
    IotLogger logger;
    newCycle(logger);
    logger.info("tag", "lost");
    http.replyTransportError(-1, IotTransportPhase::Connect);
    TEST_ASSERT_TRUE(logger.flush().isTransportError());

    http.requests.clear();
    IotLogger next;
    newCycle(next);
    next.flush();
    TEST_ASSERT_TRUE(allSent().find("I (T) tag: lost") != std::string::npos);
}

void test_what_the_server_accepted_is_not_sent_again()
{
    // earlier lines plus a nearly full buffer: two requests
    const std::string text(90, 'y');
    {
        IotLogger offline;
        newCycle(offline, false);
        for (int i = 0; i < 10; ++i) { offline.info("tag", "earlier %s", text.c_str()); }
        offline.flush();
    }
    IotLogger logger;
    newCycle(logger);
    for (int i = 0; i < 70; ++i) { logger.info("tag", "%02d %s", i, text.c_str()); }
    TEST_ASSERT_EQUAL(0, http.requestCount());
    http.reply(200);
    http.reply(500);
    TEST_ASSERT_FALSE(logger.flush().isOk());
    TEST_ASSERT_EQUAL(2, sentLogs().size());
    const std::string accepted = sentLogs()[0];
    const std::string refused = sentLogs()[1];
    TEST_ASSERT_TRUE(accepted.find("earlier") != std::string::npos);

    // the next cycle sends exactly what was refused, behind its own marker
    http.requests.clear();
    IotLogger next;
    newCycle(next);
    next.flush();
    const std::string resent = allSent();
    TEST_ASSERT_TRUE(resent.find("earlier") == std::string::npos);
    TEST_ASSERT_TRUE(resent.find("kept offline") != std::string::npos);
    // the RTC log holds the newest lines that fit
    const std::string lastLine = refused.substr(refused.rfind('\n', refused.size() - 2) + 1);
    TEST_ASSERT_TRUE(resent.find(lastLine) != std::string::npos);
    TEST_ASSERT_TRUE(accepted.find(lastLine) == std::string::npos);
}

void test_the_rtc_log_keeps_the_newest_lines_and_counts_the_rest()
{
    {
        IotLogger offline;
        newCycle(offline, false);
        for (int i = 0; i < 100; ++i) { offline.info("tag", "line %03d of the offline cycle", i); }
        offline.flush();
    }
    IotLogger next;
    newCycle(next);
    next.flush();
    const std::string sent = allSent();
    TEST_ASSERT_EQUAL(0, sent.find("W (T) iot: "));
    TEST_ASSERT_TRUE(sent.find("earlier offline log line(s) dropped") != std::string::npos);
    TEST_ASSERT_TRUE(sent.find("line 099") != std::string::npos);
    TEST_ASSERT_TRUE(sent.find("line 000") == std::string::npos);
    TEST_ASSERT_TRUE(sent.size() <= IOT_RTC_LOG_SIZE + 80);
}

void test_log_to_rtc_never_touches_the_network()
{
    IotLogger logger;
    newCycle(logger);
    IotLoggerTest::logToRtc(logger, "panic before the network");
    TEST_ASSERT_EQUAL(0, http.requestCount());
    logger.flush();
    TEST_ASSERT_TRUE(allSent().find("E (T) rtc: panic before the network") != std::string::npos);
}

// *****************************************************************************
// Unbuffered
// *****************************************************************************

void test_unbuffered_lines_are_posted_at_once()
{
    IotLogger logger;
    newCycle(logger);
    logger.setBuffered(false);
    logger.info("tag", "one");
    logger.info("tag", "two");
    TEST_ASSERT_EQUAL(2, sentLogs().size());
    TEST_ASSERT_EQUAL_STRING("I (T) tag: two\n", sentLogs()[1].c_str());
}

void test_unbuffered_lines_offline_or_failed_are_kept()
{
    IotLogger logger;
    newCycle(logger, false);
    logger.setBuffered(false);
    logger.info("tag", "offline");
    TEST_ASSERT_EQUAL(0, http.requestCount());
    network.setConnected(true);
    http.reply(502);
    logger.info("tag", "bad gateway");
    TEST_ASSERT_EQUAL(1, http.requestCount());

    http.requests.clear();
    IotLogger next;
    newCycle(next);
    next.flush();
    const std::string sent = allSent();
    TEST_ASSERT_TRUE(sent.find("offline") != std::string::npos);
    TEST_ASSERT_TRUE(sent.find("bad gateway") != std::string::npos);
}

void test_without_the_api_the_logger_is_offline()
{
    api.end();
    IotLogger logger;  // no network set: only the api decides
    logger.info("tag", "early");
    TEST_ASSERT_TRUE(logger.flush().isTransportError());
    TEST_ASSERT_EQUAL(0, http.requestCount());

    api.begin(store, http, clock_);
    api.setDeviceName("e32_aabbccddeeff");
    IotLogger next;
    newCycle(next);
    next.flush();
    TEST_ASSERT_TRUE(allSent().find("I (T) tag: early") != std::string::npos);
}

// *****************************************************************************

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_lines_have_the_esp_log_format);
    RUN_TEST(test_a_message_is_one_line);
    RUN_TEST(test_a_long_message_is_cut_not_overflowing);
    RUN_TEST(test_lines_above_the_level_are_dropped);
    RUN_TEST(test_buffered_lines_go_out_in_one_request_on_flush);
    RUN_TEST(test_a_full_buffer_is_flushed_before_the_size_limit);
    RUN_TEST(test_offline_lines_go_out_first_in_the_next_cycle);
    RUN_TEST(test_lines_of_a_5xx_are_kept);
    RUN_TEST(test_lines_of_a_4xx_are_dropped);
    RUN_TEST(test_a_transport_error_keeps_the_lines);
    RUN_TEST(test_what_the_server_accepted_is_not_sent_again);
    RUN_TEST(test_the_rtc_log_keeps_the_newest_lines_and_counts_the_rest);
    RUN_TEST(test_log_to_rtc_never_touches_the_network);
    RUN_TEST(test_unbuffered_lines_are_posted_at_once);
    RUN_TEST(test_unbuffered_lines_offline_or_failed_are_kept);
    RUN_TEST(test_without_the_api_the_logger_is_offline);
    return UNITY_END();
}
