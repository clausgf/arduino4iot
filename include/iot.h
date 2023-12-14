/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

#include <cstdio>
#include <cstdarg>
#include <functional>

#include "Arduino.h"
#include <HTTPClient.h>
#include <WiFiClient.h>

#include <iot_api.h>
#include <iot_logger.h>
#include <iot_config.h>

// *****************************************************************************

class Iot
{
public:
    // disallow copying & assignment
    Iot(const Iot&) = delete;
    Iot& operator=(const Iot&) = delete;

    Iot();

    /**
     * Initialize the IoT system including all subsystems 
     * and complete its setup.
     * 
     * Call begin() after setting all parameters and 
     * before calling any other function. It overwrites parameters
     * set so far with values from a configuration file / nvram.
     * 
     * WiFi must be connected before calling begin() or the device
     * name will not be determined correctly.
     */
    void begin();

    /**
     * Shut down the IoT system.
     */
    void end();

    /**
     * Connect to the given WiFi network and return true if successful.
     * This function uses the standard Arduino WiFi library and blocks 
     * until the connection is established or 
     * the timeout (in milliseconds) is reached.
     */
    bool connectWifi(const char *ssid, const char *password, unsigned long timeout_ms = 10000);

    /**
     * Return a unique device ID which is derived from the WiFi MAC address,
     * e.g. "e32-123456780abc"
     */
    String getDeviceId();


    // **********************************************************************
    // NTP time
    // **********************************************************************

    /**
     * @return the current time as a string in ISO 8601 format, e.g. "2020-01-01T12:34:56Z"
     */
    String getTimeIso();

    bool isTimePlausible();

    /**
     * Synchronize the system time with an NTP server.
     * 
     * This function blocks until the system time is plausible or the timeout
     * (in milliseconds) is reached. NTP sync continues in the background.
     */
    bool syncNtpTime(const char * ntpServer1 = "pool.ntp.org", 
        const char * ntpServer2 = "time.nist.gov", 
        const char * ntpServer3 = nullptr, 
        unsigned long timeout_ms = 10000);


    // **********************************************************************
    // API
    // **********************************************************************

    /**
     * Post telemetry data to the API. The body must be a valid JSON string.
     * 
     * This method is similar to @see apiGet.
     */
    int postTelemetry(String kind, String jsonData, String apiPath = "telemetry/{project}/{device}/{kind}");

    /**
     * Post telemetry data to the API. The body must be a valid JSON string.
     * 
     * This method is similar to @see apiGet.
     */
    int postSystemTelemetry(String kind = "system", String apiPath = "telemetry/{project}/{device}/{kind}");


    // **********************************************************************
    // Battery
    // **********************************************************************

    /// set the pin for the battery voltage measurement
    void setBatteryOffset_mV(int batteryOffset_mV) { _batteryOffset_mV = batteryOffset_mV; }
    void setBatteryDivider(int batteryDivider) { _batteryDivider = batteryDivider; }
    void setBatteryFactor(int batteryFactor) { _batteryFactor = batteryFactor; }
    void setBatteryPin(int batteryPin) { _batteryPin = batteryPin; }
    void setBatteryMin_mV(int batteryMin_mV) { _batteryMin_mV = batteryMin_mV; }

    /**
     * Return the battery voltage in Millivolt.
     * 
     * If not available, measure the battery voltage using the configured
     * pin, apply the correction using factor, divider and offset and
     * return the result.
     * The voltage is cached for subsequent calls.
     * 
     * @return the battery voltage in Volt
     */
    int getBatteryVoltage_mV();


    // **********************************************************************
    // Error handling
    // **********************************************************************

    /**
     * Log an error message employ a panic strategy defined by the
     * panic handler (@see setPanicHandler, @see defaultPanicHandler).
     */
    void panic(const char* format...);

    /**
     * Log an error message without using the remote api and restart 
     * the system, @see panic. This function is safe to be used
     * in the panic handler or during system initialization.
     */
    void panicEarly(const char* format...);

    /**
     * Panic handler sending the system to sleep for an increasing
     * duration.
     * 
     * The default strategy is to restart the system. 
     * If it panics again before calling @see deepSleep, 
     * @see restart or @see shutdown, 
     * put the system into sleep mode for one minute and 
     * try again. The sleep duration is doubled on each panic
     * until it reaches 24 hours.
     * 
     * @param msg the error message
     */
    void escalatingSleepPanicHandler();

    /**
     * Set a panic handler which is called in case of a panic
     * after logging an error message.
     * 
     * The default panic handler is @see escalatingSleepPanicHandler.
     * 
     * @return the previous panic handler
     */
    std::function<void()> setPanicHandler(std::function<void()> panicHandler);


    // **********************************************************************
    // System management: firmware, sleep, restart, shutdown
    // **********************************************************************

    int64_t getBootTimestamp_ms() { return _bootTimestamp_ms; }
    esp_sleep_wakeup_cause_t getWakeupCause() { return _wakeupCause; };
    uint32_t getBootCount() { return _bootCount; }
    int64_t getActiveDuration_ms() { return _activeDuration_ms; }
    int getLastSleepDuration_s() { return _lastSleepDuration_s; }
    int getPanicSleepDuration_s() { return _panicSleepDuration_s; }

    String getFirmwareVersion();
    String getFirmwareSha256();

    /// Set the duration of the next sleep cycle in seconds.
    void setSleepDuration_s(int sleep_duration_s);

    void deepSleep();
    static void deepSleep(int sleep_duration_s, bool panic = false);
    static void restart(bool panic = false);
    static void shutdown(bool panic = false);


    // **********************************************************************
    // P r i v a t e
    // **********************************************************************

private:
    static int64_t _bootTimestamp_ms;
    esp_sleep_wakeup_cause_t _wakeupCause;
    RTC_DATA_ATTR static uint32_t _bootCount;
    RTC_DATA_ATTR static int64_t _activeDuration_ms;
    RTC_DATA_ATTR static int _lastSleepDuration_s;
    String _deviceId;
    int _battery_mV;
    RTC_DATA_ATTR static int _panicSleepDuration_s;
    std::function<void()> _panicHandler;
    String _firmwareVersion;
    String _firmwareSha256;

    // configurable variables
    IotConfigValue<int> _logLevel;
    IotConfigValue<int> _sleepDuration_s;
    IotConfigValue<int> _batteryOffset_mV;
    IotConfigValue<int> _batteryDivider;
    IotConfigValue<int> _batteryFactor;
    IotConfigValue<int> _batteryPin;
    IotConfigValue<int> _batteryMin_mV;
    IotConfigValue<int> _panicSleepDurationInit_s;
    IotConfigValue<int> _panicSleepDurationMax_s;
    IotConfigValue<int> _panicSleepDurationFactor;

    /**
     * Configure the IoT system.
     * 
     * This method overrides the default configuration with value from
     * a configuration file / nvram.
     */
    void _readConfiguration();
};

extern Iot iot;
