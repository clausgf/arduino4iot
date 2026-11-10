/**
 * On-device tests of the OTA download (pio test -e device -f device/test_ota)
 * against the host's test server, which serves this suite's own image.
 *
 * Every failure must leave the device booting what it runs now. The one
 * successful update writes the other app partition and then points the boot
 * partition back at the running one, so the suite never restarts the device
 * and a failure in it at worst boots the same test image again.
 */

// Arduino first: the ESP-IDF HTTP headers define INADDR_NONE as a macro,
// which breaks Arduino's IPAddress.h when it comes later
#include "../device_test_support.h"

#include <esp_ota_ops.h>

#include "iot_ota_internal.h"

static const esp_partition_t* running = nullptr;

static std::string firmwareUrl(const char* variant = "")
{
    return deviceTestUrl((std::string("/firmware") + variant).c_str(), true);
}

/// One update from @p url, with the test CA unless @p ca says otherwise.
static bool update(const std::string& url, const char* ca = DEVICE_TEST_CA_PEM,
    std::string* oEtag = nullptr)
{
    IotOtaInternal ota;
    ota.setTimeout(20000);  // per read: generous for a weak link
    ota.setServerCert(ca, false);
    std::string etag;
    std::string date;
    const bool ok = ota.updateFirmwareFromUrl(etag, date, url.c_str(), nullptr);
    if (oEtag != nullptr) { *oEtag = etag; }
    return ok;
}

static void assertStillBootingTheRunningImage()
{
    TEST_ASSERT_EQUAL_PTR(running, esp_ota_get_running_partition());
    TEST_ASSERT_EQUAL_PTR(running, esp_ota_get_boot_partition());
}

void setUp()
{
    deviceTestConnect();
}

void tearDown()
{
    // whatever a failed test left behind: boot what runs now
    if (esp_ota_get_boot_partition() != running) { esp_ota_set_boot_partition(running); }
}

/// The OTA download erases only the sectors it writes. A short image over an
/// earlier copy of the same image would combine with that copy's tail into a
/// valid image again, so the cases that must fail start from an empty slot.
static void eraseUpdatePartition()
{
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    TEST_ASSERT_EQUAL(ESP_OK, esp_partition_erase_range(next, 0, next->size));
}

// *****************************************************************************

void test_ota_partitions_exist()
{
    TEST_ASSERT_NOT_NULL(running);
    TEST_ASSERT_NOT_NULL_MESSAGE(esp_ota_get_next_update_partition(nullptr),
        "no second app partition - the partition table has no OTA slots");
}

void test_ota_404_fails()
{
    TEST_ASSERT_FALSE(update(deviceTestUrl("/status/404", true)));
    assertStillBootingTheRunningImage();
}

void test_ota_garbage_is_not_an_image()
{
    TEST_ASSERT_FALSE(update(deviceTestUrl("/bytes/100000", true)));
    assertStillBootingTheRunningImage();
}

void test_ota_truncated_image_fails_validation()
{
    eraseUpdatePartition();
    TEST_ASSERT_FALSE(update(firmwareUrl("/truncated")));
    assertStillBootingTheRunningImage();
}

void test_ota_aborted_download_fails()
{
    eraseUpdatePartition();
    TEST_ASSERT_FALSE(update(firmwareUrl("/cut")));
    assertStillBootingTheRunningImage();
}

void test_ota_untrusted_server_fails()
{
    TEST_ASSERT_FALSE(update(firmwareUrl(), DEVICE_TEST_OTHER_CA_PEM));
    assertStillBootingTheRunningImage();
}

void test_ota_valid_image_is_written_and_activated()
{
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    const uint32_t start_ms = millis();
    std::string etag;
    TEST_ASSERT_TRUE(update(firmwareUrl(), DEVICE_TEST_CA_PEM, &etag));
    const uint32_t elapsed_ms = millis() - start_ms;
    TEST_ASSERT_EQUAL_STRING("\"fw1\"", etag.c_str());

    // the next boot would start the new image: the one this device runs
    TEST_ASSERT_EQUAL_PTR(next, esp_ota_get_boot_partition());
    esp_app_desc_t written;
    esp_app_desc_t current;
    TEST_ASSERT_EQUAL(ESP_OK, esp_ota_get_partition_description(next, &written));
    TEST_ASSERT_EQUAL(ESP_OK, esp_ota_get_partition_description(running, &current));
    TEST_ASSERT_EQUAL_MEMORY(current.app_elf_sha256, written.app_elf_sha256, sizeof(current.app_elf_sha256));

    // and back, so the suite leaves the device as it found it
    TEST_ASSERT_EQUAL(ESP_OK, esp_ota_set_boot_partition(running));
    assertStillBootingTheRunningImage();

    char text[100];
    snprintf(text, sizeof(text), "image written to %s in %lu ms", next->label, (unsigned long)elapsed_ms);
    TEST_MESSAGE(text);
}

// *****************************************************************************

void setup()
{
    running = esp_ota_get_running_partition();
    DEVICE_TEST_BEGIN();
    RUN_TEST(test_ota_partitions_exist);
    RUN_TEST(test_ota_404_fails);
    RUN_TEST(test_ota_garbage_is_not_an_image);
    RUN_TEST(test_ota_truncated_image_fails_validation);
    RUN_TEST(test_ota_aborted_download_fails);
    RUN_TEST(test_ota_untrusted_server_fails);
    RUN_TEST(test_ota_valid_image_is_written_and_activated);
    UNITY_END();
}

void loop() {}
