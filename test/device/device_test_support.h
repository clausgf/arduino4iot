/**
 * Shared helpers of the on-device tests (pio test -e device).
 */

#pragma once

#include <string>

#include <Arduino.h>
#include <unity.h>

#include "iot_esp_ports.h"
#include "iot_names.h"
#include "iot_rtc_state.h"

#include "generated/device_test_env.h"

/// Low enough for a devkit on a weak USB supply, which can brown out on the
/// transmit peaks at full power.
static constexpr int8_t DEVICE_TEST_TX_POWER_QDBM = 34;  // 8.5 dBm

inline std::string deviceTestUrl(const char* path, bool https = false)
{
    return std::string(https ? "https://" : "http://") + DEVICE_TEST_HOST + ":"
        + std::to_string(https ? DEVICE_TEST_HTTPS_PORT : DEVICE_TEST_HTTP_PORT) + path;
}

/// The WiFi station of all suites, configured on first use.
inline IotWifiNetwork& deviceTestWifi()
{
    static IotWifiNetwork wifi(iotRtcState());
    static bool configured = false;
    if (!configured)
    {
        std::string ssid = DEVICE_TEST_WIFI_SSID;
        std::string password = DEVICE_TEST_WIFI_PASSWORD;
        if (ssid.empty())
        {
            // the credentials the device was provisioned with; read only
            IotNvsStore store(iot_nvs_keys::NAMESPACE);
            if (store.begin())
            {
                ssid = store.getString(iot_nvs_keys::WIFI_SSID);
                password = store.getString(iot_nvs_keys::WIFI_PASSWORD);
                store.end();
            }
        }
        wifi.setCredentials(ssid, password);
        wifi.setFastReconnect(false);
        wifi.setMaxTxPower_qdBm(DEVICE_TEST_TX_POWER_QDBM);
        configured = true;
    }
    return wifi;
}

/// Connect, or fail the calling test with a reason. Three attempts: on a weak
/// link the WPA handshake times out now and then, which the driver reports
/// as a wrong password.
inline void deviceTestConnect()
{
    IotWifiNetwork& wifi = deviceTestWifi();
    for (int attempt = 0; attempt < 3 && !wifi.isConnected(); ++attempt)
    {
        wifi.connect(15000);
    }
    if (!wifi.isConnected())
    {
        const std::string message = "WiFi connect failed: " + wifi.lastError()
            + " (credentials from NVS namespace \"iot\" or DEVICE_TEST_WIFI_SSID/_PASSWORD)";
        TEST_FAIL_MESSAGE(message.c_str());
    }
}

/// Unity's start in the Arduino loop task: give the test runner time to
/// open the serial port after the reset. A macro, so Unity reports the
/// suite's file rather than this one.
#define DEVICE_TEST_BEGIN() \
    do { delay(2000); UNITY_BEGIN(); } while (0)
