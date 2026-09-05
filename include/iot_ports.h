/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

// *****************************************************************************
// Ports - the seams between the library's services and the platform.
//
// Every port here is a small abstract interface plus its contract. Services
// (token management, config mirror, remote logging, OTA, telemetry) are written
// against these interfaces and never against Arduino, WiFi, Preferences or
// esp_*; the concrete adapters live in iot_esp_ports.h and are selected by the
// *application*, not by the library.
//
// Three properties follow from that, and all three are deliberate:
//
// 1. Host testability. This header is free of Arduino dependencies, so a
//    service can be exercised on the native platform against the test doubles
//    in iot_fakes.h (see test/test_ports).
//
// 2. Optionality. A port is held by the core as a *pointer* that may be
//    nullptr, meaning "this device does not have that". A device without a
//    battery divider passes no IotBattery, a device that never needs wall-clock
//    time passes no IotClock. Each port below documents how the library
//    degrades in its absence - it never becomes an error.
//
// 3. Nothing unused is linked. The core never names a concrete adapter, so the
//    linker drops every adapter the application does not reference. A firmware
//    that uses no battery does not link the ADC code, one that uses no NTP does
//    not link the SNTP code.
//
// Timestamps are int64_t unix seconds (not time_t) so the contract does not
// depend on the platform's time_t width.
// *****************************************************************************


// *****************************************************************************
// IotStore - keyed, power-fail-safe persistence
// *****************************************************************************

/**
 * Key/value persistence that survives a power loss, with a bounded number of
 * write cycles (flash).
 *
 * This is the port for everything that must still be there after the battery
 * was swapped: the device token and its expiry, cache validators (ETag,
 * Last-Modified), the seeded bootstrap values and the server config mirror.
 * State that only has to survive deep sleep does *not* belong here - it goes
 * into IotRtcState (see iot_rtc_state.h), which is a plain struct rather than
 * a port because RTC RAM has no runtime keys, no failure mode and no cost.
 *
 * Contract:
 * - Keys are ASCII, 1..MAX_KEY_LENGTH characters (the NVS limit). Callers must
 *   not exceed it; @see isValidKey().
 * - A distinct IotStore instance is a distinct namespace. Keys of different
 *   instances never collide.
 * - The put* methods MUST NOT write when the stored value is already equal.
 *   Every write costs a flash cycle, and this library writes from paths that
 *   run on every wakeup.
 * - A get* on an absent key returns @p defaultValue without logging an error.
 * - An absent key and a key holding a value of a different type behave the
 *   same on read: the default value is returned.
 */
class IotStore
{
public:
    /// Maximum key length, imposed by the NVS adapter (ESP-IDF NVS_KEY_NAME_MAX_SIZE - 1).
    static constexpr size_t MAX_KEY_LENGTH = 15;

    /// @return true if @p key is a usable key for every IotStore implementation.
    static bool isValidKey(const char* key)
    {
        if (key == nullptr || key[0] == '\0')
        {
            return false;
        }
        size_t length = 0;
        for (const char* p = key; *p != '\0'; ++p, ++length)
        {
            if (length >= MAX_KEY_LENGTH || *p < 0x20 || *p > 0x7e)
            {
                return false;
            }
        }
        return true;
    }

    virtual ~IotStore() = default;

    /// @return true if @p key currently holds a value.
    virtual bool has(const char* key) const = 0;

    virtual std::string getString(const char* key, const std::string& defaultValue = "") const = 0;
    virtual int64_t getInt(const char* key, int64_t defaultValue = 0) const = 0;
    virtual bool getBool(const char* key, bool defaultValue = false) const = 0;

    /// Store @p value under @p key. Does nothing if the stored value is already equal.
    virtual void putString(const char* key, const std::string& value) = 0;
    virtual void putInt(const char* key, int64_t value) = 0;
    virtual void putBool(const char* key, bool value) = 0;

    /// Remove @p key. Removing an absent key is not an error.
    virtual void remove(const char* key) = 0;

    /// Remove every key of this store's namespace.
    virtual void clear() = 0;
};


// *****************************************************************************
// IotClock - wall-clock time
// *****************************************************************************

/**
 * Wall-clock time and its synchronization.
 *
 * Needed for two things only: the expiry arithmetic of the device token, and
 * the certificate validity check inside the TLS handshake. Both tolerate an
 * unsynchronized clock, which is why this port is optional.
 *
 * **Absent (nullptr):** the library behaves exactly as it does today with an
 * implausible clock - a device token is used until the server rejects it with
 * 401 (rather than being renewed proactively before its expiry), and cache
 * validators keep working because ETags carry no timestamp. A device on an
 * https:// endpoint does need a plausible clock; a device on http:// does not.
 */
class IotClock
{
public:
    /// Unix timestamps before this are treated as "the clock was never set"
    /// (2020-01-01 is roughly 50 years after the epoch).
    static constexpr int64_t PLAUSIBLE_THRESHOLD = 50LL * 365 * 24 * 3600;

    virtual ~IotClock() = default;

    /// @return the current time as unix seconds (unsynchronized clocks return
    /// a small value rather than an error; test with isPlausible()).
    virtual int64_t now() const = 0;

    /// @return true if the clock has plausibly been set, @see PLAUSIBLE_THRESHOLD.
    virtual bool isPlausible() const = 0;

    /// @return the time of the last successful synchronization as unix seconds,
    /// or 0 if the clock was never synchronized.
    virtual int64_t lastSyncTime() const = 0;

    /**
     * Synchronize the clock if it is due, and return whether the time can now
     * be trusted.
     *
     * Implementations decide themselves whether a sync is necessary (an
     * implausible clock, or the resync interval elapsed) - calling this once
     * per wakeup cycle is the intended usage and is cheap when nothing is due.
     *
     * @param timeout_ms upper bound for the attempt
     * @return true if the time is plausible afterwards
     */
    virtual bool sync(uint32_t timeout_ms) = 0;
};


// *****************************************************************************
// IotNetwork - IP connectivity
// *****************************************************************************

/**
 * The device's IP connectivity.
 *
 * Deliberately says nothing about WiFi: credentials, fast-reconnect caches and
 * static/DHCP addressing are configuration of the *adapter*, not of the port,
 * so an Ethernet or LTE adapter can take the WiFi adapter's place without any
 * service knowing.
 *
 * **Absent (nullptr):** not supported - the library's reason to exist is
 * talking to a server. This port is the one mandatory one.
 */
class IotNetwork
{
public:
    virtual ~IotNetwork() = default;

    /**
     * Establish connectivity, blocking until it is up or @p timeout_ms elapsed.
     * Returns true immediately if already connected.
     */
    virtual bool connect(uint32_t timeout_ms) = 0;

    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;

    /**
     * @return the hardware address as lower-case hex without separators, e.g.
     * "aabbccddeeff".
     *
     * Must be available **without** an established connection: the device id -
     * and hence every API URL - is derived from it, and it is needed on the
     * error path where the connect failed.
     */
    virtual std::string macAddress() const = 0;

    /// @return the local IP as a string, or "" if not connected.
    virtual std::string localIp() const = 0;

    /// @return the signal strength in dBm, or 0 if the medium has no such notion.
    virtual int rssi() const = 0;

    /// @return how long the last successful connect took in milliseconds, or 0
    /// if no connect happened in this wakeup cycle. Reported as the
    /// "wifi_connect_ms" system telemetry metric.
    virtual uint32_t lastConnectDuration_ms() const = 0;
};


// *****************************************************************************
// IotBattery - supply voltage
// *****************************************************************************

/**
 * The device's supply voltage.
 *
 * Exists so that measuring it is *not* the library's business. The core needs
 * a single number; how it is obtained - an ADC behind a divider, an I2C fuel
 * gauge, a coulomb counter, an external ADC - is entirely the application's.
 * IotAdcBattery (iot_esp_ports.h) is the ready-made adapter for the common
 * divider-on-an-ADC-pin case, and IotCallbackBattery below wraps any lambda.
 *
 * **Absent (nullptr):** no battery field in the system telemetry and no
 * undervoltage supervision. This is the normal case for a mains-powered device.
 */
class IotBattery
{
public:
    virtual ~IotBattery() = default;

    /**
     * @return the supply voltage in millivolts, or a negative value if it
     * cannot be determined right now.
     *
     * Implementations may cache within a wakeup cycle; the library calls this
     * more than once per cycle (telemetry and undervoltage check).
     */
    virtual int readMilliVolts() = 0;
};

/**
 * IotBattery adapter for a plain callable, so an application does not need to
 * derive a class:
 *
 *   IotCallbackBattery battery([]{ return fuelGauge.milliVolts(); });
 */
class IotCallbackBattery : public IotBattery
{
public:
    explicit IotCallbackBattery(std::function<int()> reader): _reader(std::move(reader)) {}

    int readMilliVolts() override
    {
        return _reader ? _reader() : -1;
    }

private:
    std::function<int()> _reader;
};


// *****************************************************************************
// IotPower - sleep, restart and why we booted
// *****************************************************************************

/// Why the device booted. Platform-independent projection of esp_reset_reason_t.
enum class IotResetReason
{
    Unknown,
    PowerOn,     ///< power applied; RTC RAM is not valid
    External,    ///< reset pin
    Software,    ///< esp_restart()
    Panic,       ///< exception / abort
    Watchdog,    ///< any watchdog
    Brownout,    ///< supply dipped - on a battery device usually the real story
    DeepSleep,   ///< woke from deep sleep; RTC RAM is valid
};

/// What ended the deep sleep. Platform-independent projection of esp_sleep_wakeup_cause_t.
enum class IotWakeupCause
{
    Unknown,
    Timer,
    Gpio,        ///< ext0/ext1/gpio
    Touchpad,
    Ulp,
    Uart,
};

inline const char* iotResetReasonToString(IotResetReason reason)
{
    switch (reason)
    {
        case IotResetReason::PowerOn:   return "POWER_ON";
        case IotResetReason::External:  return "EXTERNAL_PIN";
        case IotResetReason::Software:  return "SOFTWARE";
        case IotResetReason::Panic:     return "PANIC";
        case IotResetReason::Watchdog:  return "WATCHDOG";
        case IotResetReason::Brownout:  return "BROWNOUT";
        case IotResetReason::DeepSleep: return "DEEP_SLEEP";
        case IotResetReason::Unknown:   break;
    }
    return "UNKNOWN";
}

inline const char* iotWakeupCauseToString(IotWakeupCause cause)
{
    switch (cause)
    {
        case IotWakeupCause::Timer:    return "TIMER";
        case IotWakeupCause::Gpio:     return "GPIO";
        case IotWakeupCause::Touchpad: return "TOUCHPAD";
        case IotWakeupCause::Ulp:      return "ULP";
        case IotWakeupCause::Uart:     return "UART";
        case IotWakeupCause::Unknown:  break;
    }
    return "UNKNOWN";
}

/**
 * Ending a wakeup cycle, and the information about how the previous one ended.
 *
 * Splitting this out is what makes the failure paths testable at all: without
 * it, every host test that reaches a panic terminates the test process. The
 * *policy* (the escalating backoff) is deliberately not part of this port -
 * it is plain logic that belongs in the Arduino-free tier.
 *
 * **Absent (nullptr):** not supported. Every exit path of a wakeup cycle needs
 * it; a non-sleeping device uses an adapter whose deepSleep() is a busy wait.
 */
class IotPower
{
public:
    virtual ~IotPower() = default;

    /**
     * Enter deep sleep for @p duration_s and wake up through a full boot.
     * Does not return on a real device; a test double records the call and
     * returns, which is precisely why this is a port.
     */
    virtual void deepSleep(uint32_t duration_s) = 0;

    /// Restart immediately. Does not return on a real device.
    virtual void restart() = 0;

    /// Enter deep sleep with no wakeup source configured, i.e. until a physical
    /// reset. Does not return on a real device.
    virtual void shutdown() = 0;

    virtual IotResetReason resetReason() const = 0;
    virtual IotWakeupCause wakeupCause() const = 0;

    /// @return milliseconds since this boot.
    virtual uint32_t uptime_ms() const = 0;
};

// *****************************************************************************

/// @return true if @p reason indicates the previous cycle ended abnormally
/// (crash, watchdog or brownout) rather than through an orderly sleep, restart
/// or a plain power-on. This is the trigger for the escalating backoff.
inline bool iotIsAbnormalReset(IotResetReason reason)
{
    return reason == IotResetReason::Panic
        || reason == IotResetReason::Watchdog
        || reason == IotResetReason::Brownout;
}
