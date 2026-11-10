/**
 * Native unit tests for the HTTP/1.1 framing (pio test -e native).
 */

#include <algorithm>
#include <cstring>
#include <string>
#include <unity.h>

#include "iot_http1.h"

using namespace iot_http1;

void setUp() {}
void tearDown() {}

// *****************************************************************************
// parseUrl
// *****************************************************************************

void test_url_https_with_default_port()
{
    Url url;
    TEST_ASSERT_TRUE(parseUrl("https://iot.example.org/api/v1/x?a=1#frag", url));
    TEST_ASSERT_TRUE(url.https);
    TEST_ASSERT_EQUAL_STRING("iot.example.org", url.host.c_str());
    TEST_ASSERT_EQUAL(443, url.port);
    TEST_ASSERT_EQUAL_STRING("/api/v1/x?a=1", url.target.c_str());
    TEST_ASSERT_EQUAL_STRING("iot.example.org", url.hostHeader().c_str());
    TEST_ASSERT_EQUAL_STRING("https://iot.example.org:443", url.origin().c_str());
}

void test_url_http_with_port_and_no_path()
{
    Url url;
    TEST_ASSERT_TRUE(parseUrl("http://192.168.1.2:8000", url));
    TEST_ASSERT_FALSE(url.https);
    TEST_ASSERT_EQUAL(8000, url.port);
    TEST_ASSERT_EQUAL_STRING("/", url.target.c_str());
    TEST_ASSERT_EQUAL_STRING("192.168.1.2:8000", url.hostHeader().c_str());
}

void test_url_query_without_path()
{
    Url url;
    TEST_ASSERT_TRUE(parseUrl("http://host?x=1", url));
    TEST_ASSERT_EQUAL_STRING("/?x=1", url.target.c_str());
}

void test_url_ipv6_literal()
{
    Url url;
    TEST_ASSERT_TRUE(parseUrl("https://[fe80::1]:8443/p", url));
    TEST_ASSERT_EQUAL_STRING("fe80::1", url.host.c_str());
    TEST_ASSERT_EQUAL(8443, url.port);
    TEST_ASSERT_EQUAL_STRING("[fe80::1]:8443", url.hostHeader().c_str());
}

void test_url_rejects_bad_input()
{
    Url url;
    TEST_ASSERT_FALSE(parseUrl("ftp://host/", url));
    TEST_ASSERT_FALSE(parseUrl("https:///path", url));
    TEST_ASSERT_FALSE(parseUrl("https://host:/", url));
    TEST_ASSERT_FALSE(parseUrl("https://host:70000/", url));
    TEST_ASSERT_FALSE(parseUrl("https://host:8a/", url));
    TEST_ASSERT_FALSE(parseUrl("https://user:pw@host/", url));
}

// *****************************************************************************
// requestHead
// *****************************************************************************

void test_request_head_for_a_post()
{
    Url url;
    parseUrl("https://host/api/telemetry", url);
    const std::string head = requestHead("POST", url,
        { { "Content-Type", "application/json" }, { "X-Empty", "" } }, 12, "arduino4iot/4.2.0");
    TEST_ASSERT_EQUAL_STRING(
        "POST /api/telemetry HTTP/1.1\r\n"
        "Host: host\r\n"
        "User-Agent: arduino4iot/4.2.0\r\n"
        "Content-Length: 12\r\n"
        "Content-Type: application/json\r\n"
        "\r\n", head.c_str());
}

void test_request_head_get_has_no_content_length()
{
    Url url;
    parseUrl("http://host:8080/x", url);
    const std::string head = requestHead("GET", url, {}, 0, "");
    TEST_ASSERT_EQUAL_STRING("GET /x HTTP/1.1\r\nHost: host:8080\r\n\r\n", head.c_str());
}

void test_request_head_empty_post_has_content_length_zero()
{
    Url url;
    parseUrl("http://host/x", url);
    const std::string head = requestHead("POST", url, {}, 0, "");
    TEST_ASSERT_NOT_EQUAL(std::string::npos, head.find("Content-Length: 0\r\n"));
}

void test_request_head_caller_overrides_defaults()
{
    Url url;
    parseUrl("http://host/x", url);
    const std::string head = requestHead("GET", url, { { "user-agent", "me" } }, 0, "default");
    TEST_ASSERT_EQUAL(std::string::npos, head.find("default"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, head.find("user-agent: me\r\n"));
}

void test_header_safety()
{
    TEST_ASSERT_TRUE(isHeaderSafe("Bearer abc"));
    TEST_ASSERT_FALSE(isHeaderSafe("a\r\nX-Evil: 1"));
    TEST_ASSERT_FALSE(isHeaderSafe(std::string("a\0b", 3)));
}

// *****************************************************************************
// ResponseParser
// *****************************************************************************

static const std::vector<std::string> COLLECT = { "ETag", "Last-Modified" };

/// Feed @p text in pieces of @p step bytes. @return the bytes consumed.
static size_t feedInSteps(ResponseParser& parser, const std::string& text, size_t step)
{
    size_t consumed = 0;
    for (size_t i = 0; i < text.size() && !parser.done() && !parser.failed(); i += step)
    {
        consumed += parser.feed(text.data() + i, std::min(step, text.size() - i));
    }
    return consumed;
}

void test_parser_content_length_body_in_any_split()
{
    const std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 11\r\n"
        "etag: \"abc\"\r\n"
        "Server: x\r\n"
        "\r\n"
        "hello world";
    for (size_t step = 1; step <= response.size(); ++step)
    {
        std::string body;
        ResponseParser parser;
        parser.begin(false, COLLECT, body);
        TEST_ASSERT_EQUAL(response.size(), feedInSteps(parser, response, step));
        TEST_ASSERT_TRUE(parser.done());
        TEST_ASSERT_EQUAL(200, parser.status());
        TEST_ASSERT_EQUAL_STRING("hello world", body.c_str());
        TEST_ASSERT_EQUAL(11, parser.contentLength());
        TEST_ASSERT_TRUE(parser.keepAlive());
        TEST_ASSERT_EQUAL(1, parser.headers().size());
        TEST_ASSERT_EQUAL_STRING("\"abc\"", parser.headers().at("ETag").c_str());
    }
}

void test_parser_reserves_the_content_length()
{
    std::string body;
    ResponseParser parser;
    parser.begin(false, COLLECT, body);
    const std::string head = "HTTP/1.1 200 OK\r\nContent-Length: 50000\r\n\r\n";
    parser.feed(head.data(), head.size());
    TEST_ASSERT_GREATER_OR_EQUAL(50000, body.capacity());
}

void test_parser_absurd_content_length_is_out_of_memory()
{
    std::string body;
    ResponseParser parser;
    parser.begin(false, COLLECT, body);
    const std::string head = "HTTP/1.1 200 OK\r\nContent-Length: 999999999999999999\r\n\r\n";
    parser.feed(head.data(), head.size());
    TEST_ASSERT_TRUE(parser.failed());
    TEST_ASSERT_EQUAL((int)ResponseParser::Error::OutOfMemory, (int)parser.error());
}

void test_parser_head_complete_before_body()
{
    std::string body;
    ResponseParser parser;
    parser.begin(false, COLLECT, body);
    const std::string head = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\n";
    parser.feed(head.data(), head.size());
    TEST_ASSERT_TRUE(parser.headComplete());
    TEST_ASSERT_FALSE(parser.done());
    parser.feed("ab", 2);
    TEST_ASSERT_FALSE(parser.done());
    parser.onClose();
    TEST_ASSERT_TRUE(parser.failed());
    TEST_ASSERT_EQUAL((int)ResponseParser::Error::Truncated, (int)parser.error());
}

void test_parser_stops_at_the_end_of_the_response()
{
    std::string body;
    ResponseParser parser;
    parser.begin(false, COLLECT, body);
    const std::string text = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nokEXTRA";
    TEST_ASSERT_EQUAL(text.size() - 5, parser.feed(text.data(), text.size()));
    TEST_ASSERT_EQUAL_STRING("ok", body.c_str());
}

void test_parser_chunked_body_in_any_split()
{
    const std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5;ext=1\r\nhello\r\n"
        "6\r\n world\r\n"
        "0\r\n"
        "X-Trailer: 1\r\n"
        "\r\n";
    for (size_t step = 1; step <= response.size(); ++step)
    {
        std::string body;
        ResponseParser parser;
        parser.begin(false, COLLECT, body);
        TEST_ASSERT_EQUAL(response.size(), feedInSteps(parser, response, step));
        TEST_ASSERT_TRUE(parser.done());
        TEST_ASSERT_TRUE(parser.chunked());
        TEST_ASSERT_EQUAL(-1, parser.contentLength());
        TEST_ASSERT_EQUAL_STRING("hello world", body.c_str());
        TEST_ASSERT_TRUE(parser.keepAlive());
    }
}

void test_parser_chunked_with_uppercase_hex()
{
    std::string body;
    ResponseParser parser;
    parser.begin(false, COLLECT, body);
    const std::string chunk(26, 'x');
    const std::string text = "HTTP/1.1 200 OK\r\nTransfer-Encoding: Chunked\r\n\r\n1A\r\n" + chunk + "\r\n0\r\n\r\n";
    parser.feed(text.data(), text.size());
    TEST_ASSERT_TRUE(parser.done());
    TEST_ASSERT_EQUAL(26, body.size());
}

void test_parser_chunked_without_crlf_after_data_fails()
{
    std::string body;
    ResponseParser parser;
    parser.begin(false, COLLECT, body);
    const std::string text = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nabX\r\n";
    parser.feed(text.data(), text.size());
    TEST_ASSERT_TRUE(parser.failed());
    TEST_ASSERT_EQUAL((int)ResponseParser::Error::Malformed, (int)parser.error());
}

void test_parser_body_until_close()
{
    std::string body;
    ResponseParser parser;
    parser.begin(false, COLLECT, body);
    const std::string text = "HTTP/1.0 200 OK\r\n\r\nall of it";
    parser.feed(text.data(), text.size());
    TEST_ASSERT_TRUE(parser.headComplete());
    TEST_ASSERT_FALSE(parser.done());
    parser.onClose();
    TEST_ASSERT_TRUE(parser.done());
    TEST_ASSERT_EQUAL_STRING("all of it", body.c_str());
    TEST_ASSERT_FALSE(parser.keepAlive());
}

void test_parser_no_body_for_head_204_304()
{
    const char* responses[] = {
        "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n",  // as a HEAD response
        "HTTP/1.1 204 No Content\r\n\r\n",
        "HTTP/1.1 304 Not Modified\r\nETag: \"e\"\r\n\r\n",
    };
    for (int i = 0; i < 3; ++i)
    {
        std::string body;
        ResponseParser parser;
        parser.begin(i == 0, COLLECT, body);
        const std::string text = responses[i];
        TEST_ASSERT_EQUAL(text.size(), parser.feed(text.data(), text.size()));
        TEST_ASSERT_TRUE(parser.done());
        TEST_ASSERT_TRUE(body.empty());
        TEST_ASSERT_TRUE(parser.keepAlive());
    }
}

void test_parser_skips_interim_responses()
{
    std::string body;
    ResponseParser parser;
    parser.begin(false, COLLECT, body);
    const std::string text =
        "HTTP/1.1 100 Continue\r\n\r\n"
        "HTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\nok";
    parser.feed(text.data(), text.size());
    TEST_ASSERT_TRUE(parser.done());
    TEST_ASSERT_EQUAL(201, parser.status());
    TEST_ASSERT_EQUAL_STRING("ok", body.c_str());
}

void test_parser_connection_close_and_http10()
{
    std::string body;
    ResponseParser parser;
    parser.begin(false, COLLECT, body);
    std::string text = "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
    parser.feed(text.data(), text.size());
    TEST_ASSERT_TRUE(parser.done());
    TEST_ASSERT_FALSE(parser.keepAlive());

    parser.begin(false, COLLECT, body);
    text = "HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n";
    parser.feed(text.data(), text.size());
    TEST_ASSERT_FALSE(parser.keepAlive());

    parser.begin(false, COLLECT, body);
    text = "HTTP/1.0 200 OK\r\nConnection: Keep-Alive\r\nContent-Length: 0\r\n\r\n";
    parser.feed(text.data(), text.size());
    TEST_ASSERT_TRUE(parser.keepAlive());
}

void test_parser_combines_repeated_collected_headers()
{
    std::string body;
    ResponseParser parser;
    parser.begin(false, COLLECT, body);
    const std::string text = "HTTP/1.1 200 OK\r\nETag: a\r\nETag: b\r\nContent-Length: 0\r\n\r\n";
    parser.feed(text.data(), text.size());
    TEST_ASSERT_EQUAL_STRING("a, b", parser.headers().at("ETag").c_str());
}

void test_parser_rejects_malformed_responses()
{
    const char* responses[] = {
        "HTP/1.1 200 OK\r\n\r\n",
        "HTTP/1.1 20 OK\r\n\r\n",
        "HTTP/1.1 200 OK\r\nno colon\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 1x\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n",
        "HTTP/1.1 200 OK\r\n folded: header\r\n\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n",
    };
    for (const char* response : responses)
    {
        std::string body;
        ResponseParser parser;
        parser.begin(false, COLLECT, body);
        parser.feed(response, strlen(response));
        TEST_ASSERT_TRUE_MESSAGE(parser.failed(), response);
        TEST_ASSERT_EQUAL((int)ResponseParser::Error::Malformed, (int)parser.error());
    }
}

void test_parser_rejects_other_transfer_codings()
{
    std::string body;
    ResponseParser parser;
    parser.begin(false, COLLECT, body);
    const std::string text = "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n";
    parser.feed(text.data(), text.size());
    TEST_ASSERT_TRUE(parser.failed());
    TEST_ASSERT_EQUAL((int)ResponseParser::Error::Encoding, (int)parser.error());
}

void test_parser_limits_the_line_length()
{
    std::string body;
    ResponseParser parser;
    parser.begin(false, COLLECT, body);
    const std::string text = "HTTP/1.1 200 OK\r\nX-Long: " + std::string(ResponseParser::MAX_LINE_LENGTH, 'a');
    parser.feed(text.data(), text.size());
    TEST_ASSERT_TRUE(parser.failed());
    TEST_ASSERT_EQUAL((int)ResponseParser::Error::TooLarge, (int)parser.error());
}

void test_parser_close_before_the_head_is_truncated()
{
    std::string body;
    ResponseParser parser;
    parser.begin(false, COLLECT, body);
    parser.feed("HTTP/1.1 2", 10);
    parser.onClose();
    TEST_ASSERT_TRUE(parser.failed());
    TEST_ASSERT_FALSE(parser.headComplete());
    TEST_ASSERT_EQUAL((int)ResponseParser::Error::Truncated, (int)parser.error());
}

// *****************************************************************************

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_url_https_with_default_port);
    RUN_TEST(test_url_http_with_port_and_no_path);
    RUN_TEST(test_url_query_without_path);
    RUN_TEST(test_url_ipv6_literal);
    RUN_TEST(test_url_rejects_bad_input);
    RUN_TEST(test_request_head_for_a_post);
    RUN_TEST(test_request_head_get_has_no_content_length);
    RUN_TEST(test_request_head_empty_post_has_content_length_zero);
    RUN_TEST(test_request_head_caller_overrides_defaults);
    RUN_TEST(test_header_safety);
    RUN_TEST(test_parser_content_length_body_in_any_split);
    RUN_TEST(test_parser_reserves_the_content_length);
    RUN_TEST(test_parser_absurd_content_length_is_out_of_memory);
    RUN_TEST(test_parser_head_complete_before_body);
    RUN_TEST(test_parser_stops_at_the_end_of_the_response);
    RUN_TEST(test_parser_chunked_body_in_any_split);
    RUN_TEST(test_parser_chunked_with_uppercase_hex);
    RUN_TEST(test_parser_chunked_without_crlf_after_data_fails);
    RUN_TEST(test_parser_body_until_close);
    RUN_TEST(test_parser_no_body_for_head_204_304);
    RUN_TEST(test_parser_skips_interim_responses);
    RUN_TEST(test_parser_connection_close_and_http10);
    RUN_TEST(test_parser_combines_repeated_collected_headers);
    RUN_TEST(test_parser_rejects_malformed_responses);
    RUN_TEST(test_parser_rejects_other_transfer_codings);
    RUN_TEST(test_parser_limits_the_line_length);
    RUN_TEST(test_parser_close_before_the_head_is_truncated);
    return UNITY_END();
}
