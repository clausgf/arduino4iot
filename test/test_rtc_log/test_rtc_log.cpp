/**
 * Native unit tests for IotRtcLog (pio test -e native).
 *
 * The bounded buffer IotLogger keeps offline log lines in: what fits is kept
 * in order, and when it does not, the oldest whole lines go first.
 */

#include <unity.h>

#include <cstring>

#include "iot_rtc_log.h"
#include "iot_rtc_slot.h"

// *****************************************************************************

void setUp() {}
void tearDown() {}

using Small = IotRtcLog<32>;

static Small empty()
{
    Small buffer;
    std::memset(&buffer, 0, sizeof(buffer));
    return buffer;
}

// *****************************************************************************

void test_a_new_buffer_is_empty()
{
    Small buffer = empty();
    TEST_ASSERT_TRUE(buffer.isEmpty());
    TEST_ASSERT_EQUAL_STRING("", buffer.take().c_str());
}

void test_lines_are_kept_in_order_and_taken_once()
{
    Small buffer = empty();
    buffer.append("I (1) a: one\n");
    buffer.append("I (2) a: two\n");
    TEST_ASSERT_FALSE(buffer.isEmpty());
    TEST_ASSERT_EQUAL_STRING("I (1) a: one\nI (2) a: two\n", buffer.take().c_str());
    TEST_ASSERT_TRUE(buffer.isEmpty());
}

void test_the_oldest_lines_make_room()
{
    Small buffer = empty();
    buffer.append("aaaaaaaaa\n");   // 10
    buffer.append("bbbbbbbbb\n");   // 20
    buffer.append("ccccccccc\n");   // 30
    buffer.append("ddddddddd\n");   // 40 > 32: "aaa" goes
    TEST_ASSERT_EQUAL_STRING("W (0) iot: 1 earlier offline log line(s) dropped\n"
        "bbbbbbbbb\nccccccccc\nddddddddd\n", buffer.take().c_str());
}

void test_text_larger_than_the_buffer_keeps_its_tail()
{
    Small buffer = empty();
    buffer.append("old\n");
    buffer.append("0123456789\n0123456789\n0123456789\nlast-line\n");   // 43 bytes
    std::string kept = buffer.take();
    TEST_ASSERT_TRUE(kept.find("old") == std::string::npos);
    TEST_ASSERT_TRUE(kept.find("last-line\n") != std::string::npos);
    TEST_ASSERT_TRUE(kept.find("dropped") != std::string::npos);
}

void test_a_single_overlong_line_is_truncated_to_fit()
{
    Small buffer = empty();
    buffer.append(std::string(50, 'x') + "\n");
    std::string kept = buffer.take();
    // the note plus the last 32 bytes of the line
    TEST_ASSERT_TRUE(kept.size() <= 32 + 64);
    TEST_ASSERT_EQUAL_CHAR('\n', kept.back());
}

void test_it_lives_in_an_rtc_slot()
{
    IotRtcSlot<IotRtcLog<64>> slot;
    std::memset(&slot, 0xA5, sizeof(slot));   // power-on contents
    TEST_ASSERT_TRUE(slot->isEmpty());          // reset, not garbage
    slot->append("kept\n");
    TEST_ASSERT_EQUAL_STRING("kept\n", slot->take().c_str());
}


// *****************************************************************************

int main(int argc, char **argv)
{
    UNITY_BEGIN();

    RUN_TEST(test_a_new_buffer_is_empty);
    RUN_TEST(test_lines_are_kept_in_order_and_taken_once);
    RUN_TEST(test_the_oldest_lines_make_room);
    RUN_TEST(test_text_larger_than_the_buffer_keeps_its_tail);
    RUN_TEST(test_a_single_overlong_line_is_truncated_to_fit);
    RUN_TEST(test_it_lives_in_an_rtc_slot);

    return UNITY_END();
}
