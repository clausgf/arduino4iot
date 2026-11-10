/**
 * On-device tests of the platform adapters (pio test -e device -f
 * device/test_platform): NVS store, MAC before the connect, WiFi connect and
 * transmit power.
 */

#include <esp_mac.h>
#include <esp_wifi.h>

#include "../device_test_support.h"

/// Its own namespace: the tests never touch the device's provisioning.
static const char* TEST_NAMESPACE = "iot-test";

void setUp() {}
void tearDown() {}

// *****************************************************************************
// IotNvsStore
// *****************************************************************************

void test_nvs_store_round_trip()
{
    IotNvsStore store(TEST_NAMESPACE);
    TEST_ASSERT_TRUE(store.begin());
    store.clear();

    TEST_ASSERT_FALSE(store.has("s"));
    TEST_ASSERT_EQUAL_STRING("dflt", store.getString("s", "dflt").c_str());
    store.putString("s", "value");
    store.putInt("i", -1234567890123LL);
    store.putBool("b", true);
    TEST_ASSERT_TRUE(store.has("s"));
    TEST_ASSERT_EQUAL_STRING("value", store.getString("s").c_str());
    TEST_ASSERT_TRUE(store.getInt("i") == -1234567890123LL);
    TEST_ASSERT_TRUE(store.getBool("b"));

    // survives closing and reopening
    store.end();
    TEST_ASSERT_TRUE(store.begin());
    TEST_ASSERT_EQUAL_STRING("value", store.getString("s").c_str());
    store.remove("s");
    TEST_ASSERT_FALSE(store.has("s"));
    store.clear();
    TEST_ASSERT_FALSE(store.has("i"));
    store.end();
}

void test_nvs_store_reads_integers_of_any_width()
{
    // an externally generated partition image stores e.g. u8 or u32
    Preferences raw;
    TEST_ASSERT_TRUE(raw.begin(TEST_NAMESPACE, false));
    raw.putUChar("u8", 200);
    raw.putUInt("u32", 4000000000u);
    raw.end();

    IotNvsStore store(TEST_NAMESPACE);
    TEST_ASSERT_TRUE(store.begin());
    TEST_ASSERT_TRUE(store.getInt("u8") == 200);
    TEST_ASSERT_TRUE(store.getInt("u32") == 4000000000LL);
    store.clear();
    store.end();
}

// *****************************************************************************
// WiFi
// *****************************************************************************

void test_mac_is_known_before_the_connect()
{
    IotWifiNetwork& wifi = deviceTestWifi();
    TEST_ASSERT_FALSE(wifi.isConnected());
    uint8_t mac[6];
    TEST_ASSERT_EQUAL(ESP_OK, esp_read_mac(mac, ESP_MAC_WIFI_STA));
    char expected[18];
    snprintf(expected, sizeof(expected), "%02x:%02x:%02x:%02x:%02x:%02x",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    std::string actual = wifi.macAddress();
    for (char& c : actual) { c = tolower(c); }
    if (actual.find(':') == std::string::npos && actual.size() == 12)
    {
        std::string withColons;
        for (size_t i = 0; i < 12; i += 2) { withColons += (i ? ":" : "") + actual.substr(i, 2); }
        actual = withColons;
    }
    TEST_ASSERT_EQUAL_STRING(expected, actual.c_str());
}

void test_connect_applies_the_transmit_power()
{
    deviceTestConnect();
    IotWifiNetwork& wifi = deviceTestWifi();
    TEST_ASSERT_TRUE(wifi.isConnected());
    TEST_ASSERT_GREATER_THAN(0, wifi.lastConnectDuration_ms());
    TEST_ASSERT_TRUE(wifi.rssi() < 0 && wifi.rssi() > -100);
    int8_t power_qdBm = 0;
    TEST_ASSERT_EQUAL(ESP_OK, esp_wifi_get_max_tx_power(&power_qdBm));
    // the driver maps the request to its nearest supported level
    TEST_ASSERT_INT_WITHIN(4, DEVICE_TEST_TX_POWER_QDBM, power_qdBm);
    char text[120];
    snprintf(text, sizeof(text), "ip %s, rssi %d dBm, tx power %d.%02d dBm, connected in %lu ms",
        wifi.localIp().c_str(), wifi.rssi(), power_qdBm / 4, (power_qdBm % 4) * 25,
        (unsigned long)wifi.lastConnectDuration_ms());
    TEST_MESSAGE(text);
}

void test_reconnect_after_disconnect()
{
    deviceTestConnect();
    IotWifiNetwork& wifi = deviceTestWifi();
    wifi.disconnect();
    TEST_ASSERT_FALSE(wifi.isConnected());
    TEST_ASSERT_TRUE(wifi.connect(15000));
    int8_t power_qdBm = 0;
    TEST_ASSERT_EQUAL(ESP_OK, esp_wifi_get_max_tx_power(&power_qdBm));
    TEST_ASSERT_INT_WITHIN(4, DEVICE_TEST_TX_POWER_QDBM, power_qdBm);
}

// *****************************************************************************

void setup()
{
    DEVICE_TEST_BEGIN();
    RUN_TEST(test_nvs_store_round_trip);
    RUN_TEST(test_nvs_store_reads_integers_of_any_width);
    RUN_TEST(test_mac_is_known_before_the_connect);
    RUN_TEST(test_connect_applies_the_transmit_power);
    RUN_TEST(test_reconnect_after_disconnect);
    UNITY_END();
}

void loop() {}
