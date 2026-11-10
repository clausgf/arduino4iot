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
#include <iot_endpoints.h>
#include <iot_telemetry.h>
#include <iot_result.h>
#include <iot_lifecycle.h>
#include <iot_ports.h>
#include <iot_rtc_state.h>

// *****************************************************************************

#define IOT_VERSION_MAJOR 4
#define IOT_VERSION_MINOR 0
#define IOT_VERSION_PATCH 0

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
    static constexpr uint32_t DEFAULT_CLOCK_SYNC_TIMEOUT_MS = 10000;
    /// Default budget of begin() for the network connect.
    static constexpr uint32_t DEFAULT_CONNECT_TIMEOUT_MS = 10000;

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
     * continuing, @see setBackoff().
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
    bool begin(uint32_t timeout_ms = DEFAULT_CONNECT_TIMEOUT_MS);

    /**
     * Seed the build-time bootstrap configuration (WiFi credentials, API
     * endpoint, project name, provisioning token, TLS trust) into NVS.
    * There they survive a normal firmware update, making secretless
     * builds an a public CI server sufficient for updates after initial
     * initialization.
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
     * before the new values take effect.
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
     * Register a check that lets a person ask for the setup handler on a device
     * that already has credentials - e.g. to look up its MAC address for a
     * network's allow list, or to move it to another WiFi:
     *
     *   iot.setNoCredentialsHandler([] { apProvisioning.run(); });
     *   iot.setSetupTrigger([] { return apProvisioning.waitForButton(); });
     *
     * begin() calls the check only after a power-on or a reset-pin start,
     * never after a deep-sleep wakeup or a software restart, so it costs a
     * battery device nothing in its normal cycle. If it returns true, begin()
     * calls the handler of setNoCredentialsHandler() before connecting; if
     * that handler returns, begin() continues normally.
     */
    void setSetupTrigger(std::function<bool()> trigger);

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
    IotNetwork& network() const { return *_network; }

    /**
     * The built-in WiFi station, for tuning it before begin():
     *
     *   iot.wifi().setStaticIp(IPAddress(192,168,178,42), IPAddress(192,168,178,1),
     *                          IPAddress(255,255,255,0));
     *   iot.wifi().setDhcpCache(true);
     *   iot.wifi().setMaxTxPower_qdBm(WIFI_POWER_8_5dBm);
     *
     * Needs #include <iot_esp_ports.h>. This is always the built-in adapter, also
     * after setNetwork() replaced it as the network in use.
     */
    IotWifiNetwork& wifi();

    /**
     * Switch the radio off for the rest of the cycle, e.g. before a long
     * display refresh that needs no network: flushes the log, closes the API
     * connection and disconnects the network adapter.
     *
     * Log lines written afterwards cannot be sent this cycle. They are kept in
     * RTC RAM and go out with the first flush of a later cycle that has
     * connectivity, @see IotLogger::flush(). iot.begin() in the next cycle
     * connects again.
     */
    void radioOff();

    /**
     * Return a unique device ID derived from the hardware address, e.g.
     * "e32_123456780abc". The underscore separator keeps the ID a valid
     * nice4iot device name (^[a-zA-Z_][a-zA-Z0-9_]*$). Available without an
     * established connection.
     */
    std::string deviceId() const;

    /**
     * The hardware address of the network adapter in use, e.g.
     * "12:34:56:78:0a:bc" - the address to enter into a network's MAC allow
     * list. Available without an established connection; begin() logs it
     * before connecting.
     */
    std::string macAddress() const;


    // **********************************************************************
    // Time
    // **********************************************************************

    /**
     * @return @p epoch_s (unix seconds; negative: the clock's current time) in
     *         ISO 8601 format, e.g. "2020-01-01T12:34:56Z"
     */
    std::string timeIso(int64_t epoch_s = -1) const;

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
    IotClock& clock() const { return *_clock; }

    /// @return true if the time is plausible, i.e. after 2020-01-01.
    bool isTimePlausible() const;

    /// Budget of begin() for synchronizing the clock, which it does once the
    /// network is up. Call before begin(); default DEFAULT_CLOCK_SYNC_TIMEOUT_MS.
    void setClockSyncTimeout_ms(uint32_t timeout_ms) { _clockSyncTimeout_ms = timeout_ms; }


    // **********************************************************************
    // Server endpoints
    // **********************************************************************

    /// telemetry/...: iot.telemetry.post("sensors", t), iot.telemetry.postSystem()
    [[no_unique_address]] IotTelemetryEndpoint telemetry;
    /// file/...: iot.files.upload("dump.txt", s), iot.files.getCached(...)
    [[no_unique_address]] IotFileEndpoint files;
    /// forward/...: iot.forward.get("weather", "today")
    [[no_unique_address]] IotForwardEndpoint forward;
    /// firmware: iot.ota.update()
    [[no_unique_address]] IotOtaEndpoint ota;


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
    void setBatteryMin_mV(int32_t batteryMin_mV) { _batteryMin_mV = batteryMin_mV; }

    /// @return the supply voltage in millivolts, or -1 without a battery.
    int32_t battery_mV() const;


    // **********************************************************************
    // Error handling / Panic
    // **********************************************************************

    /**
     * Configure the escalating backoff a panic() enters, @see IotBackoffPolicy:
     * after a failed cycle the device sleeps initialDuration_s, each further
     * failure multiplies that by factor up to maxDuration_s, and a clean exit
     * (deepSleep(), restart(), shutdown()) clears it. The state lives in RTC
     * RAM, so it costs no flash writes in a crash loop.
     *
     * The three values are also config.json keys (panic_init_s, panic_factor,
     * panic_max_s); this call overrides them for the cycle.
     */
    void setBackoff(const IotBackoffPolicy& policy);

    /**
     * Replace what panic() does after logging. The default enters the
     * escalating backoff, @see setBackoff().
     *
     * @return the previous panic handler
     */
    std::function<void()> setPanicHandler(std::function<void()> panicHandler);

    /**
     * Log an error message and employ the panic strategy defined by the
     * panic handler. The message reaches the server with the exit flush, or
     * with a later cycle when offline.
     */
    void panic(const char* format...);


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
    std::string firmwareVersion() const;

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
     * environment rather than the sketch - use firmwareVersion() for the
     * actual firmware version. Reported as the "firmware_id" telemetry field.
     */
    std::string firmwareId() const;

    std::string firmwareSha256() const;

    /**
     * @return the board identifier from the IOT_BOARD_ID build define (e.g.
     * `-DIOT_BOARD_ID="waveshare_esp32_driver"` in a PlatformIO env), or empty
     * if undefined. Unlike the seeded bootstrap values (see IotSeedConfig),
     * this identifies the hardware variant the sketch was built for and is a
     * plain compile-time constant, not persisted to NVS. Reported as the
     * "board" telemetry field.
     */
    std::string boardId() const;


    // **********************************************************************
    // System management: watchdog
    // **********************************************************************

    /**
     * Initialize the task watchdog timer and start supervising the current task.
     *
     * The timeout given here is remembered, so calling this before begin()
     * chooses the timeout begin() will use.
     *
     * Watchdog supervision is by default enabled for the application
     * main task in begin(). Calling this again from a task already supervised
     * only changes the timeout, e.g. to allow for one long operation; from
     * another task it starts supervising that task as well.
     *
     * A watchdog timeout will cause the system to reboot and then be
     * handled like a panic() call. The watchdog timeout is
     * global to the system. A task specific watchdog is reset using resetWatchdog().
     */
    void startWatchdog(int32_t watchdogTimeout_s = DEFAULT_WATCHDOG_TIMEOUT_S);

    /**
     * Stop the watchdog timer for the current task. Does nothing if the
     * task is not supervised.
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
    uint32_t bootCount() const { return _lifecycle.bootCount(); }

    /**
     * @return how long the *previous* boot cycle stayed awake, in milliseconds
     * (recorded at its exit).
     */
    uint32_t lastActiveDuration_ms() const { return _lifecycle.lastActiveDuration_ms(); }

    /**
     * @return the nominal duration of the sleep cycle we just woke up from, in
     * seconds; 0 after a restart or shutdown.
     */
    int32_t lastSleepDuration_s() const { return _lifecycle.lastSleepDuration_s(); }

    /**
     * Set the duration of the next sleep cycle in seconds.
     * 
     * On begin(), corresponding configuration values are read from
     * sleep_s. This method allows overwriting these values later.
     */
    void setSleepDuration_s(int32_t sleep_duration_s);

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
     * Put the system into deep sleep mode for the given duration, ending the
     * cycle cleanly: the escalating backoff is cleared. A failure goes through
     * panic() instead. Records lastActiveDuration_ms() and
     * lastSleepDuration_s() and runs the exit hooks.
     * @param sleep_duration_s the sleep duration in seconds
     */
    void deepSleep(int32_t sleep_duration_s);

    /// Restart the system, ending the cycle cleanly; @see deepSleep(int).
    void restart();

    /// Deep sleep without a wakeup timer - only a reset wakes the device.
    /// Ends the cycle cleanly; @see deepSleep(int).
    void shutdown();


    // **********************************************************************
    // P r i v a t e
    // **********************************************************************

private:
    mutable std::string _deviceId;
    std::function<void()> _panicHandler;
    mutable std::string _firmwareVersion;
    mutable std::string _firmwareId;
    mutable std::string _firmwareSha256;
    static bool _isWatchdogEnabled;

    /// Ports. The network and the clock always have a default; the battery is
    /// nullptr unless the application attaches one.
    IotNetwork* _network;
    IotClock* _clock;
    IotBattery* _battery;

    int32_t _watchdogTimeout_s;
    uint32_t _clockSyncTimeout_ms;

    /// Cycle bookkeeping, exits and failure backoff; holds the state that
    /// survives a deep sleep but not a power loss (IotRtcState).
    IotLifecycle _lifecycle;
    bool _exitHooksRegistered;

    /// @see setNoCredentialsHandler(); empty unless the application registers one
    std::function<void()> _noCredentialsHandler;
    /// @see setSetupTrigger(); empty unless the application registers one
    std::function<bool()> _setupTrigger;

    // configurable variables
    IotConfigValue<int32_t> _logLevel;
    IotConfigValue<int32_t> _sleepDuration_s;
    IotConfigValue<int32_t> _batteryMin_mV;

    IotConfigValue<int32_t> _panicSleepDurationInit_s;
    IotConfigValue<int32_t> _panicSleepDurationFactor;
    IotConfigValue<int32_t> _panicSleepDurationMax_s;
    IotConfigValue<int32_t> _wifiTxPower_qdBm;

    /// Register the library's own exit hooks (log flush, connection close)
    /// exactly once; idempotent, so every exit path can call it.
    void _registerExitHooks();
    bool _syncClock();
    /// "boot 17, 2026-11-08T10:00:00Z" - names this cycle in the offline log.
    std::string _cycleName() const;

    /// panic() before the network is up: serial and RTC log only, no request.
    void _panicEarly(const char* format...);
    /// The default panic handler: sleep for the next backoff duration.
    void _enterBackoff();
    /// 0 if not in backoff, otherwise the current backoff sleep in seconds.
    int32_t _backoffDuration_s() const { return _lifecycle.backoffDuration_s(); }
    friend struct IotTelemetryEndpoint;
    void _deepSleep(int32_t sleep_duration_s, bool isFailure);
    void _restart(bool isFailure);
    void _shutdown(bool isFailure);

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
