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

#include <iot_util.h>
#include <iot_api.h>
#include <iot_logger.h>
#include <iot_config.h>

// *****************************************************************************

#define IOT_VERSION_MAJOR 1
#define IOT_VERSION_MINOR 0
#define IOT_VERSION_PATCH 0

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
     * before calling any other function. It configures the system
     * by reading configuration values (class IotConfigValue) and
     * persistent values (class IotPersistableValue) used within the
     * library from RTC RAM and/or NVRAM.The application can modify these
     * values afterwards.
     * 
     * The log level is configured using IotLogger::setLogLevel() according
     * to the configuration value *log_level*.
     * 
     * If battery voltage measurement is enabled as configured using
     * setBattery(), setBatteryMin_mV(), the battery voltage is
     * measured and checked. Undervoltage triggers a panic().
     * 
     * begin() starts watchdog supervision for the application main task, startWatchdog().
     * 
     * If you need persistent
     * persistent storage other than RTC RAM, call
     * setPreferedPersistentStorage() before calling begin().
     * 
     * WiFi must be connected before calling begin() or the device
     * name will not be determined correctly.
     */
    void begin();

    /**
     * Connect WiFi, initialize the IoT system, and sync the NTP time.
     */
    bool begin(const char *ssid, const char *password, unsigned long timeout_ms = 10000);

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
     * @return the given time as a string in ISO 8601 format, e.g. "2020-01-01T12:34:56Z"
     */
    String getTimeIso(time_t time);

    /**
     * @return the current time as a string in ISO 8601 format, e.g. "2020-01-01T12:34:56Z"
     */
    String getTimeIso();

    /**
     * Return the time of the last NTP sync.
     * 
     * This value is backed by a persistent variable and must survive restarts,
     * using either RTC RAM or NVRAM.
     */
    time_t getNtpLastSyncTime() { return _ntpLastSyncTime.get(); }

    /**
     * Set the NTP configuration for time synchronization.
     * 
     * On begin(), corresponding configuration values are read from 
     * *ntp_resync_s*, *ntp_timeout_ms*, *ntp_server1*, *ntp_server2*, *ntp_server3*.
     * This method allows overwriting these values later.
     */
    void setNtp(
        int resyncInterval_s = 24*60*60,
        int timeout_ms = 10000,
        const char *ntpServer1 = "pool.ntp.org", 
        const char *ntpServer2 = "time.nist.gov", 
        const char *ntpServer3 = "time.google.com");

    /**
     * @return the current time is considered plausible if it is after 2020-01-01, i.e. 50 years after the epoch
     */
    bool isTimePlausible();

    //bool waitUntilTimePlausible(unsigned long timeout_ms);
    /**
     * Wait until the time is plausible or the timeout (in milliseconds) is reached.
     */
    bool waitUntilNtpSync(unsigned long timeout_ms);

    /**
     * Synchronize the system time with an NTP server.
     * 
     * This function blocks until the system time is synchronized or the timeout
     * (in milliseconds) is reached. Call it after connecting WiFi and after begin().
     * Time resynchronization is performed when the time is not plausible and 
     * periodically as configured using setNtp().
     * Timeout and NTP servers are also configured using setNtp().
     */
    bool syncNtpTime();


    // **********************************************************************
    // API
    // **********************************************************************

    /**
     * Post telemetry data to the API. The body must be a valid JSON string.
     * 
     * This method is similar to apiGet().
     */
    int postTelemetry(String kind, String jsonData, String apiPath = "telemetry/{project}/{device}/{kind}");

    /**
     * Post telemetry data to the API. The body must be a valid JSON string.
     * 
     * This method is similar to apiGet().
     */
    int postSystemTelemetry(String kind = "system", String apiPath = "telemetry/{project}/{device}/{kind}");


    // **********************************************************************
    // Led
    // **********************************************************************

    /**
     * Set the LED pin.
     * 
     * On begin(), corresponding configuration values are read from 
     * *led_pin*.
     * This method allows overwriting these values later.
     */
    void setLedPin(int ledPin);

    /**
     * Set the LED pin to the given value.
     * 
     * If the LED pin is configured, set the pin to the given value.
     * Otherwise, do nothing.
     */
    void setLed(bool value);


    // **********************************************************************
    // Battery
    // **********************************************************************

    /**
     * Setup battery voltage measurement using the internal ADC.
     * 
     * The measurement is corrected using factor, divider and offset.
     * 
     * On begin(), corresponding configuration values are read from 
     * *battery_factor*, *battery_divider*, *battery_offset_mv*.
     * This method allows overwriting these values later.
     * @param batteryPin the ADC pin to use for battery voltage measurement; values <0 disable battery voltage measurement
     */
    void setBattery(int batteryPin, int batteryFactor, int batteryDivider, int batteryOffset_mV);

    /**
     * Set the minimum battery voltage in Millivolt.
     * 
     * This value is used in begin(), which measures battery voltage
     * and triggers panic() on undervoltage.
     * 
     * On begin(), corresponding configuration values are read from
     * battery_min_mv. This method allows overwriting these values later.
     * @param batteryMin_mV the minimum battery voltage in Millivolt; values <0 disable battery voltage measurement
     */
    void setBatteryMin_mV(int batteryMin_mV) { _batteryMin_mV = batteryMin_mV; }

    /**
     * Return the battery voltage in Millivolt.
     * 
     * If not already available, measure the battery voltage using the configured
     * pin, apply the correction using factor, divider and offset and
     * return the result.
     * The voltage is cached for subsequent calls.
     * 
     * @return the battery voltage in Volt
     */
    int getBatteryVoltage_mV();


    // **********************************************************************
    // Error handling / Panic
    // **********************************************************************

    /**
     * Return the whether the last restart was caused by a @panic() and the 
     * panic sleep duration.
     * 
     * This value is backed by a persistent variable and must survive restarts,
     * using either RTC RAM or NVRAM.
     * 
     * @return <0 if the system is not in panic() mode or the sleep duration 
     * of the current panic cycle in seconds, escalatingSleepPanicHandler().
     */
    int getPanicSleepDuration_s() { return _panicSleepDuration_s.get(); }

    /**
     * Configure the default panic strategy, default is escalatingSleepPanicHandler().
     * 
     * On begin(), corresponding configuration values are read from 
     * *panic_sleep_init_s*, *panic_sleep_factor*, *panic_sleep_max_s*.
     * This method allows overwriting these values later.
     */
    void setPanic(int initialDuration_s = 60, int factor = 3, int maxDuration_s = 24*60*60);

    /**
     * Set a panic handler which is called in case of a panic
     * after logging an error message.
     * 
     * The default panic handler is escalatingSleepPanicHandler().
     * 
     * @return the previous panic handler
     */
    std::function<void()> setPanicHandler(std::function<void()> panicHandler);

    /**
     * Log an error message and employ the panic strategy defined by the
     * panic handler.
     * The default panic handler is escalatingSleepPanicHandler().
     */
    void panic(const char* format...);

    /**
     * Log an error message without using the remote api and restart 
     * the system, see panic(). This function is safe to be used
     * in the panic handler or during system initialization.
     */
    void panicEarly(const char* format...);

    /**
     * Panic handler sending the system to sleep for an increasing
     * duration.
     * 
     * The default strategy is to restart the system after sleeping
     * for an initial duration. 
     * If it panics again before a clean shutdown, i.e. calling deepSleep(), 
     * restart() or shutdown(), 
     * multiply the sleeping time by a factor until it reaches a maximum
     * duration.
     * The parameters for this strategy are configurable using
     * setPanic().
     */
    void escalatingSleepPanicHandler();


    // **********************************************************************
    // System management: firmware
    // **********************************************************************

    String getFirmwareVersion();
    String getFirmwareSha256();


    // **********************************************************************
    // System management: watchdog
    // **********************************************************************

    /**
     * Initialize the task watchdog timer and start supervising the current task.
     * 
     * Watdog supervision is by default enabled for the application 
     * main task in begin(). 
     * With this method, it can be enabled for further tasks.
     * 
     * A watchdog timeout will cause the system to reboot and then be
     * handled like a panic() call. The watchdog timeout is
     * global to the system. A task specific watchdog is reset using resetWatchdog().
     */
    void startWatchdog(int watchdogTimeout_s = 20);

    /**
     * Stop the watchdog timer for the current task.
     */
    void stopWatchdog();

    /**
     * Reset the watchdog timer for the current task.
     * This function must be called periodically before the watchdog
     * timeout is reached.
     */
    void resetWatchdog();

    // **********************************************************************
    // System management: sleep, restart, shutdown
    // **********************************************************************

    /**
     * @return the number of times the system has been booted since the last power-on reset
     * (only available with RTC RAM)
     */
    uint32_t getBootCount() { return _bootCount.get(); }

    /**
     * @return the number of milliseconds the system has been active since in the last boot cycle
     * (only available with RTC RAM)
     */
    int64_t getActiveDuration_ms() { return _activeDuration_ms.get(); }

    /**
     * Nominal duration of the sleep cycle we just woke up from in seconds
     * (only available with RTC RAM)
     */
    int getLastSleepDuration_s() { return _lastSleepDuration_s.get(); }

    /**
     * Set the duration of the next sleep cycle in seconds.
     * 
     * On begin(), corresponding configuration values are read from
     * sleep_s. This method allows overwriting these values later.
     */
    void setSleepDuration_s(int sleep_duration_s);

    /**
     * Register a handler for putting the system into deepsleep for the
     * given duration. The default handler just calls esp_deep_sleep().
     */
    void setDeepSleepHandler(std::function<void(int duration_s)> deepSleepHandler);

    /**
     * Register a handler for restarting the system. The default handler
     * just calls esp_restart().
     */
    void setRestartHandler(std::function<void()> restartHandler);

    /**
     * Register a handler for shutting down the system. The default handler
     * just calls esp_deep_sleep_start() without configuring a wakeup source.
     */
    void setShutdownHandler(std::function<void()> shutdownHandler);

    /**
     * Put the system into deep sleep mode using for the sleep duration
     * from setSleepDuration_s().
     */
    void deepSleep();

    /**
     * Put the system into deep sleep mode for the given duration.
     * Call this function for an orderly shutdown or a panic() situation.
     * This function keeps track of getActiveDuration_ms() and
     * getLastSleepDuration_s(). It internally calls the deep sleep
     * handler registered with setDeepSleepHandler().
     * @param sleep_duration_s the sleep duration in seconds
     * @param panic set this to true if the sleep is due to a panic; it defaults to false for a regular sleep
     */
    void deepSleep(int sleep_duration_s, bool panic = false);

    /**
     * Restart the system immediately.
     * Call this function for an orderly shutdown or a panic() situation.
     * This function keeps track of getActiveDuration_ms() and
     * getLastSleepDuration_s(). It internally calls the restart
     * handler registered with setRestartHandler().
     * @param panic set this to true if the restart is due to a panic; it defaults to false for a regular restart
     */
    void restart(bool panic = false);

    /**
     * Shutdown the system immediately.
     * Call this function for an orderly shutdown or a panic() situation.
     * This function keeps track of getActiveDuration_ms() and
     * getLastSleepDuration_s(). It internally calls the shutdown
     * handler registered with setShutdownHandler().
     * @param panic set this to true if the shutdown is due to a panic; it defaults to false for a regular shutdown
     */
    void shutdown(bool panic = false);


    // **********************************************************************
    // P r i v a t e
    // **********************************************************************

private:
    String _deviceId;
    int _battery_mV;
    std::function<void()> _panicHandler;
    String _firmwareVersion;
    String _firmwareSha256;
    static bool _isWatchdogEnabled;
    std::function<void(int)> _deepSleepHandler;
    std::function<void()> _restartHandler;
    std::function<void()> _shutdownHandler;

    // persistent variables
    IotPersistentValue<int32_t> _bootCount;
    IotPersistentValue<int64_t> _activeDuration_ms;
    IotPersistentValue<int32_t> _lastSleepDuration_s;
    IotPersistentValue<int64_t> _ntpLastSyncTime;
    IotPersistentValue<int32_t> _panicSleepDuration_s;

    // configurable variables
    IotConfigValue<int> _logLevel;
    IotConfigValue<int> _sleepDuration_s;
    IotConfigValue<int> _watchdogTimeout_s;
    IotConfigValue<int> _ledPin;

    IotConfigValue<int> _ntpResyncInterval_s;
    IotConfigValue<int> _ntpTimeout_ms;
    IotConfigValue<String> _ntpServer1;
    IotConfigValue<String> _ntpServer2;
    IotConfigValue<String> _ntpServer3;
    String __ntpServer1; // needed to keep the string in memory
    String __ntpServer2;
    String __ntpServer3;

    IotConfigValue<int> _batteryOffset_mV;
    IotConfigValue<int> _batteryFactor;
    IotConfigValue<int> _batteryDivider;
    IotConfigValue<int> _batteryPin;
    IotConfigValue<int> _batteryMin_mV;

    IotConfigValue<int> _panicSleepDurationInit_s;
    IotConfigValue<int> _panicSleepDurationFactor;
    IotConfigValue<int> _panicSleepDurationMax_s;

    static void _ntpSyncCallback(struct timeval *tv);
};

extern Iot iot;
