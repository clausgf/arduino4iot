/**
 * Randomized tests of what parses network input (pio test -e native, and
 * under the sanitizers with pio test -e native_asan).
 *
 * The response parser reads whatever a server - or anything between it and
 * the device - sends. Mutated and random responses are fed whole and in
 * random pieces; the outcome must not depend on how the bytes arrive, and a
 * few invariants must hold for every input. Under native_asan, any
 * out-of-bounds access aborts the run. The generator is seeded, so a failure
 * reproduces; its message names the case and the input.
 */

#include <cstdint>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <vector>
#include <unity.h>

#include "iot_http1.h"

using namespace iot_http1;

void setUp() {}
void tearDown() {}

static constexpr uint32_t SEED = 20261110;

// *****************************************************************************
// Helpers
// *****************************************************************************

/// The input, printable, for a failure message.
static std::string escaped(const std::string& data)
{
    std::string out;
    for (unsigned char c : data)
    {
        if (c == '\r') { out += "\\r"; }
        else if (c == '\n') { out += "\\n"; }
        else if (c == '\\') { out += "\\\\"; }
        else if (c < 0x20 || c >= 0x7f)
        {
            char hex[5];
            snprintf(hex, sizeof(hex), "\\x%02x", c);
            out += hex;
        }
        else { out += (char)c; }
    }
    return out.size() > 400 ? out.substr(0, 400) + "..." : out;
}

/// Everything a caller can observe of one parse.
struct Outcome
{
    size_t consumed = 0;
    bool done = false;
    bool failed = false;
    ResponseParser::Error error = ResponseParser::Error::None;
    int status = 0;
    std::string body;
    std::map<std::string, std::string> headers;
    int64_t contentLength = 0;
    bool chunked = false;
    bool keepAlive = false;

    bool operator==(const Outcome& o) const
    {
        // after a failure, how much was consumed is unspecified
        return (failed || consumed == o.consumed) && done == o.done && failed == o.failed && error == o.error
            && status == o.status && body == o.body && headers == o.headers
            && (!done || (contentLength == o.contentLength && chunked == o.chunked
                && keepAlive == o.keepAlive));
    }
};

static const std::vector<std::string> COLLECT = { "ETag", "content-type", "X-A" };

/// Parse @p data delivered in pieces of the given sizes (the last piece
/// takes the rest); the connection closes after the last byte unless the
/// parser stopped earlier.
static Outcome parse(const std::string& data, bool headRequest, const std::vector<size_t>& pieces)
{
    Outcome out;
    ResponseParser parser;
    parser.begin(headRequest, COLLECT, out.body);
    size_t offset = 0;
    size_t piece = 0;
    while (offset < data.size() && !parser.done() && !parser.failed())
    {
        size_t length = piece < pieces.size() ? pieces[piece++] : data.size() - offset;
        length = std::min(std::max<size_t>(length, 1), data.size() - offset);
        const size_t consumed = parser.feed(data.data() + offset, length);
        if (consumed > length)
        {
            TEST_FAIL_MESSAGE("feed() consumed more than it was given");
        }
        if (consumed < length && !parser.done() && !parser.failed())
        {
            TEST_FAIL_MESSAGE("feed() stopped early without completing or failing");
        }
        offset += consumed;
        if (consumed < length) { break; }
    }
    if (!parser.done() && !parser.failed()) { parser.onClose(); }

    out.consumed = offset;
    out.done = parser.done();
    out.failed = parser.failed();
    out.error = parser.error();
    out.status = parser.status();
    out.headers = parser.headers();
    out.contentLength = parser.contentLength();
    out.chunked = parser.chunked();
    out.keepAlive = parser.done() && parser.keepAlive();
    return out;
}

/// The invariants of a single outcome; @return a violation, or "".
static std::string violation(const Outcome& o, const std::string& data, bool headRequest)
{
    if (o.done == o.failed) { return "not exactly one of done and failed after onClose()"; }
    if (o.failed && o.error == ResponseParser::Error::None) { return "failed without an error"; }
    if (o.done && o.error != ResponseParser::Error::None) { return "done with an error"; }
    if (o.consumed > data.size()) { return "consumed more than the input"; }
    if (o.body.size() > data.size()) { return "a body larger than the input"; }
    if (o.done)
    {
        if (o.status < 200 || o.status > 999) { return "done with a status outside 200..999"; }
        const bool bodyless = headRequest || o.status == 204 || o.status == 304;
        if (bodyless && !o.body.empty()) { return "a body for HEAD, 204 or 304"; }
        if (!bodyless && !o.chunked && o.contentLength >= 0
            && o.body.size() != (size_t)o.contentLength)
        {
            return "a body that does not match its Content-Length";
        }
        if (o.consumed < data.size() && !bodyless && !o.chunked && o.contentLength < 0)
        {
            return "a body until close that stopped before the close";
        }
    }
    return "";
}

static void check(const std::string& data, bool headRequest, std::mt19937& rng, const char* what,
    size_t index)
{
    const Outcome whole = parse(data, headRequest, {});
    std::string problem = violation(whole, data, headRequest);

    // the same bytes, cut differently: byte by byte, and at random
    std::vector<std::vector<size_t>> cuts;
    cuts.push_back(std::vector<size_t>(data.size(), 1));
    for (int i = 0; i < 2; ++i)
    {
        std::vector<size_t> random;
        for (size_t n = 0; n < data.size(); n += random.back())
        {
            random.push_back(1 + rng() % 40);
        }
        cuts.push_back(random);
    }
    for (const auto& cut : cuts)
    {
        if (!problem.empty()) { break; }
        if (!(parse(data, headRequest, cut) == whole))
        {
            problem = "the outcome depends on how the bytes are split";
        }
    }

    if (!problem.empty())
    {
        char message[700];
        snprintf(message, sizeof(message), "%s case %u (seed %u, head %d): %s; input \"%s\"",
            what, (unsigned)index, (unsigned)SEED, headRequest ? 1 : 0, problem.c_str(),
            escaped(data).c_str());
        TEST_FAIL_MESSAGE(message);
    }
}

// *****************************************************************************
// Inputs
// *****************************************************************************

static const std::vector<std::string> RESPONSES = {
    "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nETag: \"e1\"\r\n\r\nhello",
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nX-A: 1\r\n\r\n"
        "5;ext=1\r\nhello\r\n6\r\n world\r\n0\r\nTrailer: x\r\n\r\n",
    "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nuntil the connection closes",
    "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\nok",
    "HTTP/1.1 204 No Content\r\nContent-Length: 10\r\n\r\n",
    "HTTP/1.1 304 Not Modified\r\nETag: \"e2\"\r\n\r\n",
    "HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 3\r\n\r\nno!",
    "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nContent-Length: 3\r\n\r\nabc",
    "HTTP/1.1 200 OK\r\nX-A: a\r\nx-a: b\r\nContent-Length: 0\r\n\r\n",
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n0\r\n\r\n",
    "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nokHTTP/1.1 200 OK\r\n\r\n",
};

/// Bytes that matter to the framing, so mutations hit the interesting paths.
static char interestingByte(std::mt19937& rng)
{
    static const char BYTES[] = "\r\n:; 0123456789abcdefABCDEF\t,-\"\x00\xff";
    return BYTES[rng() % (sizeof(BYTES) - 1)];
}

static std::string mutate(std::string data, std::mt19937& rng)
{
    const int count = 1 + rng() % 4;
    for (int i = 0; i < count && !data.empty(); ++i)
    {
        const size_t at = rng() % data.size();
        switch (rng() % 8)
        {
        case 0: data[at] = (char)(rng() & 0xff); break;
        case 1: data[at] = interestingByte(rng); break;
        case 2: data.insert(at, 1, interestingByte(rng)); break;
        case 3: data.erase(at, 1 + rng() % 8); break;
        case 4: data.resize(at); break;
        case 5: data.insert(at, data.substr(at, 1 + rng() % 16)); break;
        case 6: data.insert(at, std::to_string(rng() % 2 ? rng() : (uint64_t)rng() * rng())); break;
        default: data.insert(at, "\r\n"); break;
        }
    }
    return data;
}

// *****************************************************************************
// Tests
// *****************************************************************************

void test_the_seed_responses_parse_the_same_in_any_pieces()
{
    std::mt19937 rng(SEED);
    for (size_t i = 0; i < RESPONSES.size(); ++i)
    {
        check(RESPONSES[i], false, rng, "seed", i);
        check(RESPONSES[i], true, rng, "seed", i);
    }
}

void test_mutated_responses()
{
    std::mt19937 rng(SEED);
    for (size_t i = 0; i < 20000; ++i)
    {
        const std::string data = mutate(RESPONSES[rng() % RESPONSES.size()], rng);
        check(data, rng() % 8 == 0, rng, "mutated", i);
    }
}

void test_random_bytes()
{
    std::mt19937 rng(SEED + 1);
    for (size_t i = 0; i < 5000; ++i)
    {
        std::string data(rng() % 300, '\0');
        for (char& c : data) { c = rng() % 3 == 0 ? interestingByte(rng) : (char)(rng() & 0xff); }
        if (rng() % 2 == 0) { data = "HTTP/1.1 200 OK\r\n" + data; }
        check(data, false, rng, "random", i);
    }
}

void test_long_lines_are_too_large_not_a_crash()
{
    std::mt19937 rng(SEED);
    const std::string longValue(ResponseParser::MAX_LINE_LENGTH + 10, 'x');
    check("HTTP/1.1 200 OK\r\nX-A: " + longValue + "\r\n\r\n", false, rng, "long", 0);
    check("HTTP/1.1 200 " + longValue + "\r\n\r\n", false, rng, "long", 1);
    check("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" + longValue + "\r\n", false, rng,
        "long", 2);
}

void test_random_urls()
{
    std::mt19937 rng(SEED + 2);
    static const char ALPHABET[] = "htpsfx:/[]@?#.%0123456789abc-_ \r\n";
    static const std::vector<std::string> STARTS = { "http://", "https://", "https://[", "" };
    for (size_t i = 0; i < 20000; ++i)
    {
        std::string text = STARTS[rng() % STARTS.size()];
        const size_t length = rng() % 40;
        for (size_t n = 0; n < length; ++n) { text += ALPHABET[rng() % (sizeof(ALPHABET) - 1)]; }

        Url url;
        if (!parseUrl(text, url)) { continue; }
        char message[200];
        snprintf(message, sizeof(message), "url case %u: \"%s\"", (unsigned)i, escaped(text).c_str());
        TEST_ASSERT_FALSE_MESSAGE(url.host.empty(), message);
        TEST_ASSERT_NOT_EQUAL_MESSAGE(0, url.port, message);
        TEST_ASSERT_EQUAL_CHAR_MESSAGE('/', url.target[0], message);
        TEST_ASSERT_TRUE_MESSAGE(isHeaderSafe(url.target) && isHeaderSafe(url.host), message);

        // a valid URL gives exactly one head, ending the request head
        const std::string head = requestHead("GET", url, {}, 0, "fuzz");
        TEST_ASSERT_EQUAL_MESSAGE(head.size() - 4, head.find("\r\n\r\n"), message);
    }
}

// *****************************************************************************

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_the_seed_responses_parse_the_same_in_any_pieces);
    RUN_TEST(test_mutated_responses);
    RUN_TEST(test_random_bytes);
    RUN_TEST(test_long_lines_are_too_large_not_a_crash);
    RUN_TEST(test_random_urls);
    return UNITY_END();
}
