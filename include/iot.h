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
#include <iot_telemetry.h>
#include <iot_result.h>

// *****************************************************************************

#define IOT_VERSION_MAJOR 3
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
     * Connect WiFi (using the seeded credentials from NVS), initialize the IoT
     * system and sync the NTP time.
     *
     * Call seedCredentials() before this (once, with build-time defaults) so the
     * WiFi credentials and API configuration are present in NVS. begin() reads
     * configuration values (class IotConfigValue) and persistent values from RTC
     * RAM / NVRAM, sets the log level from the *log_level* config value, checks
     * the battery (undervoltage triggers panic()), and starts the watchdog.
     *
     * @param timeout_ms the WiFi connect timeout in milliseconds
     * @return true if WiFi connected and NTP sync succeeded
     */
    bool begin(unsigned long timeout_ms = 10000);

    /**
     * Seed the build-time bootstrap configuration (WiFi credentials, API
     * endpoint, project name, provisioning token, TLS trust) into NVS.
     *
     * Each non-empty value is written only if the corresponding NVS key is
     * absent (or if cfg.seedGeneration exceeds the value stored in NVS, which
     * forces an overwrite - see IotSeedConfig). A secretless firmware built
     * without the -D defaults passes empty values and thus leaves NVS untouched,
     * which is what allows flashing updates without secrets. Call once, before
     * begin(). Use factoryReset() to clear everything.
     */
    void seedCredentials(const IotSeedConfig& cfg);

    /**
     * Erase all persisted state (seeded configuration, tokens, config values and
     * persistent variables) from NVS. After this the device must boot a firmware
     * that re-seeds the configuration. Does not touch RTC RAM.
     */
    void factoryReset();

    /**
     * Shut down the IoT system.
     */
    void end();

    /**
     * Connect to the given WiFi network and return true if successful.
     * This function uses the standard Arduino WiFi library and blocks
     * until the connection is established or
     * the timeout (in milliseconds) is reached.
     *
     * For deep-sleep clients the connect dominates the radio-on time. Two opt-in
     * optimizations shorten it (see setFastReconnect() and setStaticIp()); both
     * fall back automatically to a plain full-scan/DHCP connect on failure.
     */
    bool connectWifi(const char *ssid, const char *password, unsigned long timeout_ms = 10000);

    /**
     * Configure a static IP so the connect can skip DHCP (a large part of the
     * radio-on time each wakeup). When unset, DHCP is used as before.
     *
     * With a static IP the device no longer learns a DNS server from DHCP: pass
     * dns1/dns2 if the API URL uses a hostname (not needed for an IP literal).
     */
    void setStaticIp(IPAddress ip, IPAddress gateway, IPAddress subnet,
                     IPAddress dns1 = IPAddress((uint32_t)0), IPAddress dns2 = IPAddress((uint32_t)0));

    /**
     * Cache the last-good AP (BSSID + channel) in RTC RAM so the next connect
     * after deep sleep can skip the all-channel scan. Enabled by default.
     *
     * Degrades gracefully: on a connect timeout the cache is discarded and a
     * plain full-scan connect is retried, so roaming, an AP channel change or a
     * moved device are handled transparently (at the cost of that one slow
     * connect). Disable to always do a full scan.
     */
    void setFastReconnect(bool enabled);

    /**
     * @return the duration of the last successful WiFi connect in milliseconds,
     * or 0 if not connected this cycle. Useful to verify the fast-reconnect and
     * static-IP savings via telemetry (also posted as "wifi_connect_ms").
     */
    unsigned long getWifiConnectDuration_ms() { return _wifiConnectDuration_ms; }

    /**
     * Return a unique device ID which is derived from the WiFi MAC address,
     * e.g. "e32_123456780abc". The underscore separator keeps the ID a valid
     * nice4iot device name (^[a-zA-Z_][a-zA-Z0-9_]*$).
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
     * The server expects a flat JSON object with numeric values;
     * prefer the IotTelemetry overload which guarantees this.
     * Bodies larger than IOT_MAX_TELEMETRY_SIZE trigger a warning as the
     * server rejects them by default (HTTP 413).
     *
     * This method is similar to apiGet().
     */
    IotResult postTelemetry(const String& kind, const String& jsonData, const String& apiPath = "telemetry/{project}/{device}/{kind}");

    /**
     * Post telemetry data built with an IotTelemetry builder to the API.
     *
     * Example:
     *   IotTelemetry telemetry;
     *   telemetry.add("temperature", 22.5);
     *   if (!iot.postTelemetry("sensors", telemetry)) { ... }
     */
    IotResult postTelemetry(const String& kind, const IotTelemetry& telemetry, const String& apiPath = "telemetry/{project}/{device}/{kind}");

    /**
     * Post system telemetry (battery voltage if configured, WiFi RSSI,
     * boot count, active duration, sleep durations, time and firmware
     * information) to the API.
     *
     * This method is similar to apiGet().
     */
    IotResult postSystemTelemetry(const String& kind = "system", const String& apiPath = "telemetry/{project}/{device}/{kind}");


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

    // WiFi fast-connect
    bool _fastReconnect;
    bool _staticIpConfigured;
    IPAddress _ip, _gw, _mask, _dns1, _dns2;
    unsigned long _wifiConnectDuration_ms;
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
    IotConfigValue<int32_t> _logLevel;
    IotConfigValue<int32_t> _sleepDuration_s;
    IotConfigValue<int32_t> _watchdogTimeout_s;
    IotConfigValue<int32_t> _ledPin;

    IotConfigValue<int32_t> _ntpResyncInterval_s;
    IotConfigValue<int32_t> _ntpTimeout_ms;
    IotConfigValue<String> _ntpServer1;
    IotConfigValue<String> _ntpServer2;
    IotConfigValue<String> _ntpServer3;
    String __ntpServer1; // needed to keep the string in memory
    String __ntpServer2;
    String __ntpServer3;

    IotConfigValue<int32_t> _batteryOffset_mV;
    IotConfigValue<int32_t> _batteryFactor;
    IotConfigValue<int32_t> _batteryDivider;
    IotConfigValue<int32_t> _batteryPin;
    IotConfigValue<int32_t> _batteryMin_mV;

    IotConfigValue<int32_t> _panicSleepDurationInit_s;
    IotConfigValue<int32_t> _panicSleepDurationFactor;
    IotConfigValue<int32_t> _panicSleepDurationMax_s;

    static void _ntpSyncCallback(struct timeval *tv);

    /// Initialize all subsystems (config, logger, api, watchdog, battery check)
    /// after WiFi is connected. Called by begin().
    void _initSubsystems();
};

extern Iot iot;
