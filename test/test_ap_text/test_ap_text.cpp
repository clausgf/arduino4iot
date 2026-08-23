/**
 * Native unit tests for the captive-probe lookup table (pio test -e native).
 */

#include <string>
#include <unity.h>

#include "iot_ap_text.h"

using namespace iot_ap_text;

// *****************************************************************************

void setUp() {}
void tearDown() {}

// *****************************************************************************
// lookupCaptiveProbeResponse
// *****************************************************************************

void test_android_generate_204()
{
    CaptiveProbeResponse r;
    TEST_ASSERT_TRUE(lookupCaptiveProbeResponse("/generate_204", r));
    TEST_ASSERT_EQUAL_INT(204, r.status);
}

void test_android_gen_204()
{
    CaptiveProbeResponse r;
    TEST_ASSERT_TRUE(lookupCaptiveProbeResponse("/gen_204", r));
    TEST_ASSERT_EQUAL_INT(204, r.status);
}

void test_apple_hotspot_detect()
{
    CaptiveProbeResponse r;
    TEST_ASSERT_TRUE(lookupCaptiveProbeResponse("/hotspot-detect.html", r));
    TEST_ASSERT_EQUAL_INT(200, r.status);
    TEST_ASSERT_TRUE(std::string(r.body).find("Success") != std::string::npos);
}

void test_apple_library_success()
{
    CaptiveProbeResponse r;
    TEST_ASSERT_TRUE(lookupCaptiveProbeResponse("/library/test/success.html", r));
    TEST_ASSERT_EQUAL_INT(200, r.status);
}

void test_windows_connecttest()
{
    CaptiveProbeResponse r;
    TEST_ASSERT_TRUE(lookupCaptiveProbeResponse("/connecttest.txt", r));
    TEST_ASSERT_EQUAL_STRING("Microsoft Connect Test", r.body);
}

void test_windows_ncsi()
{
    CaptiveProbeResponse r;
    TEST_ASSERT_TRUE(lookupCaptiveProbeResponse("/ncsi.txt", r));
    TEST_ASSERT_EQUAL_STRING("Microsoft NCSI", r.body);
}

void test_firefox_success()
{
    CaptiveProbeResponse r;
    TEST_ASSERT_TRUE(lookupCaptiveProbeResponse("/success.txt", r));
    TEST_ASSERT_EQUAL_STRING("success\n", r.body);
}

void test_unknown_path_returns_false()
{
    CaptiveProbeResponse r;
    TEST_ASSERT_FALSE(lookupCaptiveProbeResponse("/foo", r));
}

void test_root_path_returns_false()
{
    CaptiveProbeResponse r;
    TEST_ASSERT_FALSE(lookupCaptiveProbeResponse("/", r));
}

void test_save_path_returns_false()
{
    CaptiveProbeResponse r;
    TEST_ASSERT_FALSE(lookupCaptiveProbeResponse("/save", r));
}

// *****************************************************************************

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_android_generate_204);
    RUN_TEST(test_android_gen_204);
    RUN_TEST(test_apple_hotspot_detect);
    RUN_TEST(test_apple_library_success);
    RUN_TEST(test_windows_connecttest);
    RUN_TEST(test_windows_ncsi);
    RUN_TEST(test_firefox_success);
    RUN_TEST(test_unknown_path_returns_false);
    RUN_TEST(test_root_path_returns_false);
    RUN_TEST(test_save_path_returns_false);
    return UNITY_END();
}
