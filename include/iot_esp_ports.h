/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

#include <functional>
#include <string>

#include "Arduino.h"
#include <IPAddress.h>
#include <Preferences.h>

#include "iot_ports.h"
#include "iot_rtc_state.h"

// *****************************************************************************
// ESP32 adapters for the ports in iot_ports.h.
//
// The application picks and instantiates the adapters it needs and hands them
// to the library; the library never names one. That is what keeps the unused
// ones out of the binary: a firmware that references no IotAdcBattery does not
// link the ADC code, one that references no IotNtpClock does not link SNTP, and
// so on.
//
// This header pulls in Arduino and must therefore never be included from the
// Arduino-free tier (iot_ports.h, iot_result.h, iot_telemetry.h, iot_text.h)
// or from a native test.
// *****************************************************************************


// *****************************************************************************
// IotNvsStore - IotStore over an NVS (Preferences) namespace
// *****************************************************************************

/**
 * IotStore backed by one NVS namespace.
 *
 * The namespace handle is opened by begin() and kept open until end(), so the
 * accessors do not reopen NVS on every call (the same trick IotConfig already
 * uses). Reads of absent keys are silent: Preferences::getXxx() logs
 * "NOT_FOUND" through Arduino's log_e(), which bypasses the ESP-IDF log level
 * machinery and cannot be filtered, so every read is guarded by isKey().
 *
 * Integer encoding: writes normalize to NVS i64, reads accept any integer type
 * NVS reports for the key. That keeps a device provisioned by an older firmware
 * (or by an externally generated NVS partition image, where `tlsMode` is u8 and
 * `seedGen` is u32 - see docs/concepts.md) readable, while new writes converge
 * on a single encoding.
 */
class IotNvsStore : public IotStore
{
public:
    IotNvsStore(const IotNvsStore&) = delete;
    IotNvsStore& operator=(const IotNvsStore&) = delete;

    /// @param nvsNamespace the NVS namespace, at most 15 characters
    explicit IotNvsStore(const char* nvsNamespace);
    ~IotNvsStore() override;

    /// Open the namespace read-write. @return true on success.
    bool begin();
    void end();
    bool isOpen() const { return _isOpen; }

    bool has(const char* key) const override;
    std::string getString(const char* key, const std::string& defaultValue = "") const override;
    int64_t getInt(const char* key, int64_t defaultValue = 0) const override;
    bool getBool(const char* key, bool defaultValue = false) const override;
    void putString(const char* key, const std::string& value) override;
    void putInt(const char* key, int64_t value) override;
    void putBool(const char* key, bool value) override;
    void remove(const char* key) override;
    void clear() override;

private:
    const char* _nvsNamespace;
    mutable Preferences _preferences;  // the Preferences getters are non-const
    bool _isOpen;

    /// @return true if @p key holds a readable integer, which is then in @p oValue.
    bool _readInt(const char* key, int64_t& oValue) const;
    bool _usable(const char* key) const;
};


// *****************************************************************************
// IotSystemClock / IotNtpClock - IotClock adapters
// *****************************************************************************

/**
 * IotClock over the plain system clock, with no synchronization of its own:
 * sync() only reports whether the time is already plausible.
 *
 * For devices whose time comes from somewhere else - an external RTC chip, a
 * GPS receiver, a light-sleeping device that never loses its clock - and for
 * http:// deployments that only need timestamps to be monotonic. Links no SNTP
 * code at all.
 */
class IotSystemClock : public IotClock
{
public:
    IotSystemClock() = default;

    int64_t now() const override;
    bool isPlausible() const override;
    int64_t lastSyncTime() const override { return isPlausible() ? now() : 0; }
    bool sync(uint32_t timeout_ms) override { (void)timeout_ms; return isPlausible(); }
};

/**
 * IotClock that synchronizes over SNTP.
 *
 * sync() sends a single manual SNTP request and sets the clock directly rather
 * than starting the lwIP SNTP daemon, whose randomized 0-5 s startup delay
 * (CONFIG_LWIP_SNTP_STARTUP_DELAY) otherwise dominates the wakeup. There is no
 * daemon fallback: if the one-shot query fails there is no connectivity, and
 * the daemon would not do better.
 *
 * A sync is only attempted when the clock is implausible or the resync interval
 * has elapsed, so calling sync() once per wakeup is cheap. Pointing the first
 * server at an IP literal or the local router additionally removes DNS from the
 * path.
 *
 * @param lastSyncTime where the timestamp of the last successful sync is kept
 *        across deep sleep - normally IotRtcState::ntpLastSyncTime.
 */
class IotNtpClock : public IotClock
{
public:
    IotNtpClock(const IotNtpClock&) = delete;
    IotNtpClock& operator=(const IotNtpClock&) = delete;

    explicit IotNtpClock(int64_t& lastSyncTime);

    /// Up to three servers; empty entries are skipped. Defaults to
    /// pool.ntp.org / time.nist.gov / time.google.com.
    void setServers(const char* server1, const char* server2 = "", const char* server3 = "");

    /// How long a sync stays valid. Default 24 h.
    void setResyncInterval_s(int32_t resyncInterval_s) { _resyncInterval_s = resyncInterval_s; }

    int64_t now() const override;
    bool isPlausible() const override;
    int64_t lastSyncTime() const override { return _lastSyncTime; }
    bool sync(uint32_t timeout_ms) override;

private:
    int64_t& _lastSyncTime;
    int32_t _resyncInterval_s;
    String _server[3];

    bool _syncOneShot(uint32_t timeout_ms);
};


// *****************************************************************************
// IotWifiNetwork - IotNetwork over the Arduino WiFi station
// *****************************************************************************

/**
 * IotNetwork over WiFi, with the optimizations that matter for a deep-sleep
 * device. All of them degrade to a plain full-scan DHCP connect on failure.
 *
 * - **Fast reconnect** (on by default): the last-good BSSID and channel are
 *   kept in the RTC state, so the next connect targets that AP directly and
 *   skips the all-channel scan (~0.3-1.5 s). A timeout discards the cache and
 *   retries with a full scan, so roaming or a moved device costs one slow
 *   connect.
 * - **Static IP**: skips DHCP entirely. The device then learns no DNS server,
 *   so pass dns1/dns2 if the API URL uses a hostname.
 * - **DHCP lease cache** (off by default): the assigned lease is kept in the
 *   RTC state and re-applied on the next wake, skipping the DORA exchange
 *   (~0.3-0.5 s), but only on the fast-reconnect path (same AP, hence same
 *   subnet) and only for @p maxReuse wakeups before a real bind renews it.
 *   Off by default because it trades a small correctness risk for speed.
 */
class IotWifiNetwork : public IotNetwork
{
public:
    IotWifiNetwork(const IotWifiNetwork&) = delete;
    IotWifiNetwork& operator=(const IotWifiNetwork&) = delete;

    explicit IotWifiNetwork(IotRtcState& rtcState);

    void setCredentials(const std::string& ssid, const std::string& password);

    /// Set a hostname to announce via DHCP. Defaults to none; the library
    /// normally passes the device id.
    void setHostname(const std::string& hostname);

    void setStaticIp(IPAddress ip, IPAddress gateway, IPAddress subnet,
                     IPAddress dns1 = IPAddress((uint32_t)0), IPAddress dns2 = IPAddress((uint32_t)0));
    void setFastReconnect(bool enabled) { _fastReconnect = enabled; }
    void setDhcpCache(bool enabled, uint32_t maxReuse = 20);

    bool connect(uint32_t timeout_ms) override;
    void disconnect() override;
    bool isConnected() const override;
    std::string macAddress() const override;
    std::string localIp() const override;
    int rssi() const override;
    uint32_t lastConnectDuration_ms() const override { return _connectDuration_ms; }

private:
    IotRtcState& _rtc;
    String _ssid;
    String _password;
    String _hostname;
    bool _fastReconnect;
    bool _staticIpConfigured;
    bool _dhcpCache;
    uint32_t _dhcpMaxReuse;
    IPAddress _ip, _gw, _mask, _dns1, _dns2;
    uint32_t _connectDuration_ms;

    void _invalidateDhcpCache();
    bool _waitForConnection(unsigned long deadline_ms) const;
};


// *****************************************************************************
// IotAdcBattery - IotBattery over an ADC pin behind a voltage divider
// *****************************************************************************

/**
 * IotBattery for the common case: the supply behind a resistive divider on an
 * ADC pin.
 *
 *   voltage_mV = raw_mV * factor / divider + offset_mV
 *
 * This is one adapter among many, not the library's idea of what a battery is -
 * an I2C fuel gauge, an external ADC or a coulomb counter goes through
 * IotCallbackBattery or an own IotBattery implementation instead, without the
 * core noticing.
 *
 * Over the pre-4.0 built-in measurement this adds averaging over several
 * samples (an ESP32 ADC reading is noisy) and an optional enable pin, so the
 * divider can be switched off between measurements instead of draining the
 * battery continuously.
 */
class IotAdcBattery : public IotBattery
{
public:
    struct Config
    {
        int pin           = -1;   ///< ADC pin; < 0 disables the measurement
        int factor        = 2;    ///< divider numerator
        int divider       = 1;    ///< divider denominator (must not be 0)
        int offset_mV     = 0;    ///< additive correction
        int samples       = 4;    ///< readings to average, at least 1
        int enablePin     = -1;   ///< optional pin powering the divider; < 0 = always on
        bool enableActiveHigh = true;
        uint32_t settleTime_ms = 2;  ///< delay after enabling before sampling
    };

    explicit IotAdcBattery(const Config& config): _config(config), _cached_mV(-1) {}

    /// @return the averaged, corrected voltage in mV, or -1 if no pin is
    /// configured. Cached after the first call within a wakeup cycle.
    int readMilliVolts() override;

    /// Drop the cached reading so the next readMilliVolts() measures again.
    void invalidate() { _cached_mV = -1; }

private:
    Config _config;
    int _cached_mV;
};


// *****************************************************************************
// IotEspPower - IotPower over the ESP32 sleep and reset API
// *****************************************************************************

/**
 * IotPower over esp_deep_sleep()/esp_restart().
 *
 * The three exit methods do not return. An application that needs to do
 * something first (park a peripheral, pull a chip-enable low) derives from this
 * class or implements IotPower itself - which is exactly what the three
 * pre-4.0 handler setters were for.
 */
class IotEspPower : public IotPower
{
public:
    IotEspPower() = default;

    void deepSleep(uint32_t duration_s) override;
    void restart() override;
    void shutdown() override;
    IotResetReason resetReason() const override;
    IotWakeupCause wakeupCause() const override;
    uint32_t uptime_ms() const override { return (uint32_t)millis(); }
};
