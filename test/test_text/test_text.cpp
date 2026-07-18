/**
 * Native unit tests for the pure text helpers (pio test -e native).
 */

#include <string>
#include <unity.h>

#include "iot_text.h"

using namespace iot_text;

// *****************************************************************************

void setUp() {}
void tearDown() {}

// *****************************************************************************
// buildApiUrl
// *****************************************************************************

void test_url_substitutes_project_and_device()
{
    std::string url = buildApiUrl("https://host/api/", "telemetry/{project}/{device}/{kind}",
        "proj", "e32-123");
    TEST_ASSERT_EQUAL_STRING("https://host/api/telemetry/proj/e32-123/{kind}", url.c_str());
}

void test_url_drops_leading_slash_of_path()
{
    std::string url = buildApiUrl("https://host/api/", "/provision", "proj", "dev");
    TEST_ASSERT_EQUAL_STRING("https://host/api/provision", url.c_str());
}

void test_url_without_placeholders()
{
    std::string url = buildApiUrl("http://host/api/", "provision", "proj", "dev");
    TEST_ASSERT_EQUAL_STRING("http://host/api/provision", url.c_str());
}

void test_url_multiple_device_occurrences()
{
    std::string url = buildApiUrl("http://h/", "{device}/x/{device}", "p", "dev");
    TEST_ASSERT_EQUAL_STRING("http://h/dev/x/dev", url.c_str());
}

// *****************************************************************************
// nextLogChunkLength
// *****************************************************************************

void test_chunk_shorter_than_limit_returns_all()
{
    const char* body = "line1\nline2\n";
    size_t len = 12;
    TEST_ASSERT_EQUAL_UINT(len, nextLogChunkLength(body, len, 0, 100));
}

void test_chunk_splits_at_last_newline_within_limit()
{
    // "aaa\nbbb\nccc" with limit 6 -> first chunk is "aaa\n" (4 bytes)
    const char* body = "aaa\nbbb\nccc";
    size_t len = 11;
    size_t chunk = nextLogChunkLength(body, len, 0, 6);
    TEST_ASSERT_EQUAL_UINT(4, chunk);
    // next chunk starts at 4: "bbb\nccc", limit 6 -> "bbb\n" (4 bytes)
    TEST_ASSERT_EQUAL_UINT(4, nextLogChunkLength(body, len, 4, 6));
    // final chunk starts at 8: "ccc" (3 bytes)
    TEST_ASSERT_EQUAL_UINT(3, nextLogChunkLength(body, len, 8, 6));
}

void test_chunk_hard_split_when_no_newline()
{
    // single long line without newline within the limit -> hard split
    const char* body = "aaaaaaaaaa"; // 10 chars, no newline
    TEST_ASSERT_EQUAL_UINT(4, nextLogChunkLength(body, 10, 0, 4));
}

void test_chunk_returns_zero_at_end()
{
    const char* body = "abc";
    TEST_ASSERT_EQUAL_UINT(0, nextLogChunkLength(body, 3, 3, 10));
}

void test_chunk_full_iteration_terminates_and_covers_body()
{
    const char* body = "xxxxx\nyy\nzzzzzzzz\n"; // 18 bytes
    size_t len = 18;
    size_t offset = 0;
    size_t iterations = 0;
    while (offset < len)
    {
        size_t chunk = nextLogChunkLength(body, len, offset, 6);
        TEST_ASSERT_TRUE(chunk >= 1);
        offset += chunk;
        TEST_ASSERT_TRUE(++iterations < 100); // guard against infinite loop
    }
    TEST_ASSERT_EQUAL_UINT(len, offset);
}

// *****************************************************************************

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_url_substitutes_project_and_device);
    RUN_TEST(test_url_drops_leading_slash_of_path);
    RUN_TEST(test_url_without_placeholders);
    RUN_TEST(test_url_multiple_device_occurrences);
    RUN_TEST(test_chunk_shorter_than_limit_returns_all);
    RUN_TEST(test_chunk_splits_at_last_newline_within_limit);
    RUN_TEST(test_chunk_hard_split_when_no_newline);
    RUN_TEST(test_chunk_returns_zero_at_end);
    RUN_TEST(test_chunk_full_iteration_terminates_and_covers_body);
    return UNITY_END();
}
