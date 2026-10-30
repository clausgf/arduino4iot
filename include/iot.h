/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

#include <cstdio>
#include <cstdarg>
#include <functional>

#include "Arduino.h"

#include <iot_api.h>
#include <iot_logger.h>
#include <iot_config.h>
#include <iot_telemetry.h>
#include <iot_result.h>
#include <iot_lifecycle.h>
#include <iot_ports.h>
#include <iot_rtc_state.h>

// *****************************************************************************

#define IOT_VERSION_MAJOR 3
#define IOT_VERSION_MINOR 5
#define IOT_VERSION_PATCH 1

// *****************************************************************************

class IotWifiNetwork;

class Iot
{
public:
    /// Lower bounds for the two config values that can render a fleet
    /// unreachable if a config.json sets them too low: a zero sleep wakes the
    /// device immediately, a tiny watchdog resets every cycle. Both are clamped.
    static constexpr int32_t MIN_SLEEP_DURATION_S = 1;
    static constexpr int32_t MIN_WATCHDOG_TIMEOUT_S = 5;

    /// Defaults for the two values begin() needs before any configuration is
    /// available. Neither comes from config.json: a watchdog set too short from
    /// the server bricks a fleet, and a sync timeout is a property of the
    /// deployment's uplink, not an operational knob.
    static constexpr int32_t DEFAULT_WATCHDOG_TIMEOUT_S = 20;
    static constexpr uint32_t DEFAULT_NTP_TIMEOUT_MS = 10000;

    // disallow copying & assignment
    Iot(const Iot&) = delete;
    Iot& operator=(const Iot&) = delete;

    Iot();

    /**
     * Connect WiFi (using the seeded credentials from NVS), initialize the IoT
     * system and sync the NTP time.
     *
     * Call seedCredentials() before this (once, with build-time defaults) so the
     * WiFi credentials and API configuration are present in NVS. begin()
     * initializes NVS, starts the wakeup cycle (boot count, reset reason), reads
     * configuration values (class IotConfigValue), sets the log level from the
     * *log_level* config value, checks the battery (undervoltage triggers
     * shutdown), and starts the watchdog.
     *
     * If the *previous* cycle ended in a crash, a watchdog timeout or a
     * brownout, begin() logs that and enters the escalating backoff instead of
     * continuing, @see setPanic().
     *
     * If no WiFi SSID is seeded in NVS, begin() calls the handler registered
     * with setNoCredentialsHandler() instead of attempting to connect. Without
     * one it logs an explicit error and shuts down - see
     * setNoCredentialsHandler() and docs/concepts.md, "First-time
     * provisioning: SoftAP + captive portal".
     *
     * @param timeout_ms the connect timeout in milliseconds
     * @return true if the network came up. The clock is synchronized after
     *         that, but does not decide this result - a device on an http://
     *         endpoint works with an unsynchronized clock. Check
     *         isTimePlausible() where it matters; begin() logs a warning.
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
     *
     * Calling this on an already-running device (e.g. from a runtime
     * re-provisioning flow) only updates NVS: IotApi::begin() copies these
     * values into RAM once and never reloads them, so a reboot is required
     * before the new values take effect - restart the device after seeding
     * rather than continuing the current wakeup cycle.
     */
    void seedCredentials(const IotSeedConfig& cfg);

    /**
     * Register what begin() should do when it finds no seeded WiFi SSID in NVS
     * - a factory-fresh device, or one after clearProvisioning()/factoryReset().
     * Such a device cannot reach the server at all, so there is nothing
     * sensible for the library itself to do.
     *
     * The usual handler starts the SoftAP + captive-portal setup form:
     *
     *   #include <iot_ap.h>
     *   iot.setNoCredentialsHandler([] { apProvisioning.run(); });
     *
     * A hook rather than a direct call because IotAp costs ~42 KB of flash
     * (DNSServer + WebServer) that most devices never need: since the core
     * names no provisioning module, the linker drops it unless an application
     * references it, as here.
     *
     * The handler is not expected to return - a successful setup ends in
     * iot.restart(), because a runtime re-seed only writes NVS and needs a
     * reboot to take effect. If it returns, or if none is registered, begin()
     * logs an error and calls shutdown(): indefinite deep sleep, since the "no
     * credentials" condition cannot resolve itself between wakeups, so a timer
     * retry would only drain the battery.
     */
    void setNoCredentialsHandler(std::function<void()> handler);

    /**
     * Erase all persisted state (seeded configuration, tokens, config values and
     * persistent variables) from NVS. After this the device must boot a firmware
     * that re-seeds the configuration. Does not touch RTC RAM.
     */
    void factoryReset();

    /**
     * Erase only the seed/provisioning state (the "iot" NVS namespace: WiFi
     * credentials, API endpoint, project, provisioning token, TLS trust,
     * device token, OTA cache, seed generation) - unlike factoryReset(), does
     * not touch the "iot-cfg" namespace (the server config mirror). After this,
     * an empty seeded WiFi SSID makes the next begin() enter AP provisioning mode.
     */
    void clearProvisioning();

    /**
     * Shut down the IoT system.
     */
    void end();

    /**
     * Replace the network adapter. Defaults to the built-in WiFi station, which
     * begin() configures with the seeded credentials; a replacement carries its
     * own configuration. Call before begin(); @p network must outlive this object.
     */
    void setNetwork(IotNetwork& network);

    /// @return the network adapter in use.
    IotNetwork& network() { return *_network; }

    /**
     * Connect using the credentials given here rather than the seeded ones.
     * Only meaningful for the built-in WiFi adapter.
     */
    bool connectWifi(const char *ssid, const char *password, unsigned long timeout_ms = 10000);

    /**
     * The built-in WiFi station, for tuning it before begin():
     *
     *   iot.wifi().setStaticIp(IPAddress(192,168,178,42), IPAddress(192,168,178,1),
     *                          IPAddress(255,255,255,0));
     *   iot.wifi().setDhcpCache(true);
     *
     * Needs #include <iot_esp_ports.h>. This is always the built-in adapter, also
     * after setNetwork() replaced it as the network in use.
     */
    IotWifiNetwork& wifi();

    /**
     * @return the duration of the last successful WiFi connect in milliseconds,
     * or 0 if not connected this cycle. Useful to verify the fast-reconnect and
     * static-IP savings via telemetry (also posted as "wifi_connect_ms").
     */
    unsigned long getWifiConnectDuration_ms();

    /**
     * Return a unique device ID derived from the hardware address, e.g.
     * "e32_123456780abc". The underscore separator keeps the ID a valid
     * nice4iot device name (^[a-zA-Z_][a-zA-Z0-9_]*$). Available without an
     * established connection.
     */
    std::string getDeviceId();


    // **********************************************************************
    // NTP time
    // **********************************************************************

    /**
     * @return the given time as a string in ISO 8601 format, e.g. "2020-01-01T12:34:56Z"
     */
    std::string getTimeIso(time_t time);

    /**
     * @return the current time as a string in ISO 8601 format, e.g. "2020-01-01T12:34:56Z"
     */
    std::string getTimeIso();

    /**
     * Replace the clock. The default reads the system time and never
     * synchronizes it, which is enough for an http:// endpoint or a device
     * whose time comes from elsewhere. For SNTP:
     *
     *   static IotNtpClock ntpClock(iotRtcState().ntpLastSyncTime);
     *   iot.setClock(ntpClock);
     *
     * Call before begin(); @p clock must outlive this object. Only a firmware
     * that names IotNtpClock links any SNTP code.
     */
    void setClock(IotClock& clock);

    /// @return the clock in use.
    IotClock& clock() { return *_clock; }

    /// @return the time of the last successful clock synchronization, or 0.
    time_t getNtpLastSyncTime();

    /// @return true if the time is plausible, i.e. after 2020-01-01.
    bool isTimePlausible();

    /// Budget for a clock synchronization, @see syncNtpTime(). Call before
    /// begin(); default DEFAULT_NTP_TIMEOUT_MS.
    void setNtpTimeout_ms(uint32_t timeout_ms) { _ntpTimeout_ms = timeout_ms; }

    /**
     * Synchronize the clock if it is due. Called by begin() after the network
     * is up.
     *
     * @return true if the time is plausible afterwards
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
    IotResult postTelemetry(const std::string& kind, const std::string& jsonData, const std::string& apiPath = "telemetry/{project}/{device}/{kind}");

    /**
     * Post telemetry data built with an IotTelemetry builder to the API.
     *
     * Example:
     *   IotTelemetry telemetry;
     *   telemetry.add("temperature", 22.5);
     *   if (!iot.postTelemetry("sensors", telemetry)) { ... }
     */
    IotResult postTelemetry(const std::string& kind, const IotTelemetry& telemetry, const std::string& apiPath = "telemetry/{project}/{device}/{kind}");

    /**
     * Post system telemetry (battery voltage if configured, WiFi RSSI,
     * boot count, active duration, sleep durations, time and firmware
     * information) to the API.
     *
     * This method is similar to apiGet().
     */
    IotResult postSystemTelemetry(const std::string& kind = "system", const std::string& apiPath = "telemetry/{project}/{device}/{kind}");


    // **********************************************************************
    // Battery
    // **********************************************************************

    /**
     * Attach a battery. Without one there is no battery field in the system
     * telemetry and no undervoltage supervision - the normal case for a
     * mains-powered device.
     *
     * How the voltage is obtained is the application's business:
     *
     *   IotAdcBattery battery({ .pin = 34, .factor = 2, .divider = 1 });
     *   iot.setBattery(battery);
     *
     * or IotCallbackBattery for a fuel gauge, or an own IotBattery.
     * @p battery must outlive this object.
     */
    void setBattery(IotBattery& battery);

    /**
     * Undervoltage threshold in millivolts; begin() shuts the device down below
     * it. Values < 0 disable the check. Server-configurable as *battery_min_mv*.
     */
    void setBatteryMin_mV(int batteryMin_mV) { _batteryMin_mV = batteryMin_mV; }

    /// @return the supply voltage in millivolts, or -1 without a battery.
    int getBatteryVoltage_mV();


    // **********************************************************************
    // Error handling / Panic
    // **********************************************************************

    /**
     * Return whether the device is in the failure backoff and for how long it
     * will sleep.
     *
     * Kept in the RTC state, which survives a deep sleep as well as a software
     * or watchdog reset, but not a power loss - a physically power-cycled
     * device deliberately starts over. Costs no flash write cycles, which
     * matters precisely in the crash loop it exists for.
     *
     * @return <0 if not in backoff, otherwise the sleep duration of the current
     * backoff cycle in seconds, @see escalatingSleepPanicHandler().
     */
    int getPanicSleepDuration_s() { return _lifecycle.backoffDuration_s(); }

    /**
     * Configure the escalating backoff, @see IotBackoffPolicy.
     *
     * On begin(), corresponding configuration values are read from
     * *panic_sleep_init_s*, *panic_sleep_factor*, *panic_sleep_max_s*.
     * This method allows overwriting these values later.
     */
    void setPanic(int initialDuration_s = 60, int factor = 2, int maxDuration_s = 24*60*60);

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

    /**
     * @return the firmware version, derived at build time from the consuming
     * project's git via `git describe --tags --dirty --always` (IOT_FW_VERSION,
     * injected by the library's pre-build script), e.g. "0.10.0",
     * "0.10.0-3-gcce20b9" or "0.10.0-3-gcce20b9-dirty". An override set via
     * setFirmwareVersion() takes precedence; empty if neither is available.
     * Reported as the "firmware_version" telemetry field.
     */
    std::string getFirmwareVersion();

    /**
     * Override the version reported in telemetry (e.g. a CI-supplied value, or
     * for non-git builds). Takes precedence over the injected IOT_FW_VERSION
     * build define.
     */
    void setFirmwareVersion(const std::string& version);

    /**
     * @return the composite firmware identifier from the ESP application
     * descriptor: "<project> <version> <date> <time> IDF <idf> sec <sec>
     * ARDUINO x.y.z IOT a.b.c". In a PlatformIO/Arduino build the project/version
     * come from the precompiled framework, so this identifies the build
     * environment rather than the sketch - use getFirmwareVersion() for the
     * actual firmware version. Reported as the "firmware_id" telemetry field.
     */
    std::string getFirmwareId();

    std::string getFirmwareSha256();

    /**
     * @return the board identifier from the IOT_BOARD_ID build define (e.g.
     * `-DIOT_BOARD_ID="waveshare_esp32_driver"` in a PlatformIO env), or empty
     * if undefined. Unlike the seeded bootstrap values (see IotSeedConfig),
     * this identifies the hardware variant the sketch was built for and is a
     * plain compile-time constant, not persisted to NVS. Reported as the
     * "board_id" telemetry field.
     */
    std::string getBoardId();


    // **********************************************************************
    // System management: watchdog
    // **********************************************************************

    /**
     * Initialize the task watchdog timer and start supervising the current task.
     *
     * The timeout given here is remembered, so calling this before begin()
     * chooses the timeout begin() will use.
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
     * @return the number of times the system has been booted since the last
     * power-on reset, this boot included. Counted in begin(), so a cycle that
     * fails to connect counts too.
     */
    uint32_t getBootCount() { return _lifecycle.bootCount(); }

    /**
     * @return how long the *previous* boot cycle stayed awake, in milliseconds
     * (recorded at its exit).
     */
    int64_t getActiveDuration_ms() { return _lifecycle.lastActiveDuration_ms(); }

    /**
     * @return the nominal duration of the sleep cycle we just woke up from, in
     * seconds; 0 after a restart or shutdown.
     */
    int getLastSleepDuration_s() { return _lifecycle.lastSleepDuration_s(); }

    /**
     * Set the duration of the next sleep cycle in seconds.
     * 
     * On begin(), corresponding configuration values are read from
     * sleep_s. This method allows overwriting these values later.
     */
    void setSleepDuration_s(int sleep_duration_s);

    /**
     * Replace the power adapter, i.e. how the device actually sleeps, restarts
     * and shuts down. Defaults to IotEspPower (esp_deep_sleep()/esp_restart()).
     *
     * To park a peripheral or pull a chip-enable low, derive from IotEspPower
     * and override the one method that matters. Call before begin(); @p power
     * has to outlive the Iot object. To run something at *every* exit without
     * replacing the mechanism, use addExitHook().
     */
    void setPower(IotPower& power);

    /**
     * Register a callback to run at every exit of the wakeup cycle - deep
     * sleep, restart, shutdown and the backoff path - e.g. to park a sensor or
     * store application state. Hooks run in registration order, after the
     * library's own (log flush, connection close).
     */
    void addExitHook(std::function<void()> hook);

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
    std::string _deviceId;
    std::function<void()> _panicHandler;
    std::string _firmwareVersion;
    std::string _firmwareId;
    std::string _firmwareSha256;
    static bool _isWatchdogEnabled;

    /// Ports. The network and the clock always have a default; the battery is
    /// nullptr unless the application attaches one.
    IotNetwork* _network;
    IotClock* _clock;
    IotBattery* _battery;

    int32_t _watchdogTimeout_s;
    uint32_t _ntpTimeout_ms;

    /// Cycle bookkeeping, exits and failure backoff; holds the state that
    /// survives a deep sleep but not a power loss (IotRtcState).
    IotLifecycle _lifecycle;
    bool _exitHooksRegistered;

    /// @see setNoCredentialsHandler(); empty unless the application registers one
    std::function<void()> _noCredentialsHandler;

    // configurable variables
    IotConfigValue<int32_t> _logLevel;
    IotConfigValue<int32_t> _sleepDuration_s;
    IotConfigValue<int32_t> _batteryMin_mV;

    IotConfigValue<int32_t> _panicSleepDurationInit_s;
    IotConfigValue<int32_t> _panicSleepDurationFactor;
    IotConfigValue<int32_t> _panicSleepDurationMax_s;

    /// Register the library's own exit hooks (log flush, connection close)
    /// exactly once; idempotent, so every exit path can call it.
    void _registerExitHooks();

    /// Start the wakeup cycle before any network exists: initialize NVS, count
    /// the boot, latch the reset reason, start the watchdog, and enter the
    /// backoff if the previous cycle ended abnormally. Called by begin().
    void _beginCycle();

    /// Initialize all subsystems (config, logger, api, watchdog, battery check)
    /// after the network is up. Called by begin().
    void _initSubsystems();
};

extern Iot iot;

/// The "iot" NVS namespace: seeded bootstrap configuration, tokens, OTA cache.
/// Opened on first use, so it is available before iot.begin().
IotStore& iotBootstrapStore();
