/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "iot_result.h"

// *****************************************************************************
// Ports - the seams between the library's services and the platform.
//
// Abstract interfaces plus their contracts. Services are written against these
// and never against Arduino, WiFi, Preferences or esp_*; the adapters live in
// iot_esp_ports.h and are chosen by the *application*.
//
// - Arduino-free, so services can be tested on the host against iot_fakes.h.
// - Optional: a port is held as a pointer that may be nullptr, meaning "this
//   device does not have that". Each port documents how the library degrades
//   without it; that is never an error.
// - Nothing unused is linked: the core names no concrete adapter, so the linker
//   drops every adapter the application does not reference.
//
// Timestamps are int64_t unix seconds, not time_t, so the contract does not
// depend on the platform's time_t width.
// *****************************************************************************


// *****************************************************************************
// IotStore - keyed, power-fail-safe persistence
// *****************************************************************************

/**
 * Key/value persistence that survives a power loss, with a bounded number of
 * write cycles (flash).
 *
 * For everything that must still be there after the battery was swapped:
 * tokens, cache validators, seeded bootstrap values, the config mirror. State
 * that only has to survive deep sleep goes into IotRtcState instead.
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
    static bool isValidKey(const char* key);

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
// IotHttp - HTTP(S) request/response
// *****************************************************************************

/// How the server certificate is verified for an https:// URL.
enum class IotTlsMode : uint8_t
{
    None,       ///< no TLS configuration (plain http:// endpoint)
    Bundle,     ///< verify against the built-in Mozilla root bundle
    Insecure,   ///< development only: do not verify the server certificate
    CaPin,      ///< verify against the given CA certificate
};

/// TLS trust as data, so the same configuration can reach the HTTP client and
/// the OTA downloader without either reaching into the other.
struct IotTlsConfig
{
    IotTlsMode mode = IotTlsMode::None;
    std::string caCertPem;      ///< for mode == CaPin
    std::string clientCertPem;  ///< optional mutual TLS
    std::string clientKeyPem;   ///< optional mutual TLS, secret

    /// @return true if the mode establishes server trust at all. Insecure does
    /// encrypt, so it counts - it just does not verify who is at the other end.
    bool hasServerTrust() const { return mode != IotTlsMode::None; }
};

struct IotHttpRequest
{
    const char* method = "GET";
    std::string url;
    std::string body;
    /// Headers to send. Entries with an empty value are skipped, which is how a
    /// caller suppresses one the library would otherwise add.
    std::map<std::string, std::string> headers;
    /// Response headers to capture; anything else is discarded unread.
    std::vector<std::string> collectResponseHeaders;
};

struct IotHttpResponse
{
    /// Negative for a transport error, otherwise the HTTP status.
    int status = 0;
    std::string body;
    std::map<std::string, std::string> headers;

    /// For a transport error: where it happened, None if the adapter cannot tell.
    IotTransportPhase phase = IotTransportPhase::None;
    /// For a TLS failure: the TLS library's error code, 0 otherwise.
    int tlsError = 0;
    /// Duration of the request, including connect and handshake.
    uint32_t elapsed_ms = 0;
};

/**
 * One HTTP(S) conversation with a server.
 *
 * The seam that keeps IotApi free of Arduino: provisioning, the 401 retry, URL
 * building and header merging are decisions, testable against a fake; sockets,
 * TLS and keep-alive are the adapter's.
 *
 * **Absent (nullptr):** not supported. Talking to a server is the point.
 */
class IotHttp
{
public:
    virtual ~IotHttp() = default;

    /// Apply the TLS trust. Must be called before the first https:// request;
    /// implementations may keep pointers into @p tls, so it has to outlive them.
    virtual void setTls(const IotTlsConfig& tls) = 0;

    /// @return true if the TLS configuration establishes server trust. Holds
    /// from setTls() on, before the first request has opened a connection.
    virtual bool hasServerTrust() const = 0;

    virtual void setConnectTimeout_ms(uint32_t timeout_ms) = 0;
    virtual void setRequestTimeout_ms(uint32_t timeout_ms) = 0;
    /// Bound the TLS handshake of a new connection.
    virtual void setHandshakeTimeout_ms(uint32_t timeout_ms) = 0;
    /// Bound the whole request, so a response that trickles in ends too.
    virtual void setResponseTimeout_ms(uint32_t timeout_ms) = 0;

    /**
     * Perform one request. Requests within a wakeup cycle share a keep-alive
     * connection where the transport supports it.
     *
     * @return the status, also placed in @p oResponse.status
     */
    virtual int send(const IotHttpRequest& request, IotHttpResponse& oResponse) = 0;

    /// Close the shared connection, e.g. before deep sleep.
    virtual void close() = 0;

    /// @return a human-readable description of a negative status.
    virtual std::string errorToString(int status) const = 0;

    /// @return a human-readable description of IotHttpResponse::tlsError, or
    /// "" if the adapter has none.
    virtual std::string tlsErrorToString(int tlsError) const { (void)tlsError; return std::string(); }
};


// *****************************************************************************
// IotClock - wall-clock time
// *****************************************************************************

/**
 * Wall-clock time and its synchronization.
 *
 * Needed for the device token's expiry arithmetic, timestamps and whatever the
 * application does with the time. All of it tolerates an unsynchronized clock,
 * hence optional.
 *
 * **Absent (nullptr):** a device token is used until the server rejects it with
 * 401 instead of being renewed before its expiry; ETag caching is unaffected.
 * TLS is unaffected too: arduino-esp32 builds mbedTLS without
 * MBEDTLS_HAVE_TIME_DATE, so certificate validity dates are not checked. Only a
 * custom sdkconfig enabling that check makes https depend on the clock.
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
 * Says nothing about WiFi: credentials, fast-reconnect caches and addressing
 * are adapter configuration, so an Ethernet or LTE adapter can take the WiFi
 * adapter's place without any service noticing.
 *
 * **Absent (nullptr):** not supported. This port is mandatory.
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

    /// @return why the last connect() failed, in a few words for the remote
    /// log (e.g. "ssid not found"); empty after a success or if unknown.
    virtual std::string lastError() const { return std::string(); }

    /// Drop connectivity and power the interface down (radio off); a later
    /// connect() brings it back.
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;

    /**
     * @return the hardware address as lower-case hex without separators, e.g.
     * "aabbccddeeff". Must work **without** a connection: the device id, and
     * hence every API URL, derives from it - including on the path where the
     * connect failed.
     */
    virtual std::string macAddress() const = 0;

    /// @return the local IP as a string, or "" if not connected.
    virtual std::string localIp() const = 0;

    /// @return the signal strength in dBm, or 0 if the medium has no such notion.
    virtual int rssi() const = 0;

    /// @return duration of the last successful connect in milliseconds, 0 if
    /// none happened this cycle. Posted as the "wifi_connect_ms" metric.
    virtual uint32_t lastConnectDuration_ms() const = 0;
};


// *****************************************************************************
// IotBattery - supply voltage
// *****************************************************************************

/**
 * The device's supply voltage.
 *
 * The core needs a single number; how it is obtained - a divider on an ADC pin,
 * an I2C fuel gauge, a coulomb counter - is the application's business.
 * IotAdcBattery covers the common divider case, IotCallbackBattery wraps any
 * lambda.
 *
 * **Absent (nullptr):** no battery field in the system telemetry and no
 * undervoltage supervision - the normal case for a mains-powered device.
 */
class IotBattery
{
public:
    virtual ~IotBattery() = default;

    /**
     * @return the supply voltage in millivolts, negative if unavailable.
     * Implementations may cache within a wakeup cycle - the library calls this
     * more than once per cycle (telemetry and undervoltage check).
     */
    virtual int32_t read_mV() = 0;
};

/**
 * IotBattery over a plain callable, so no class has to be derived:
 *
 *   IotCallbackBattery battery([]{ return fuelGauge.milliVolts(); });
 */
class IotCallbackBattery : public IotBattery
{
public:
    explicit IotCallbackBattery(std::function<int32_t()> reader): _reader(std::move(reader)) {}

    int32_t read_mV() override
    {
        return _reader ? _reader() : -1;
    }

private:
    std::function<int32_t()> _reader;
};


// *****************************************************************************
// IotPower - sleep, restart and why we booted
// *****************************************************************************

/// Why the device booted. Platform-independent projection of esp_reset_reason_t.
/// The values are stable: they are reported as the reset_reason telemetry field.
enum class IotResetReason
{
    Unknown   = 0,
    PowerOn   = 1,  ///< power applied; RTC RAM is not valid
    External  = 2,  ///< reset pin
    Software  = 3,  ///< esp_restart()
    Panic     = 4,  ///< exception / abort
    Watchdog  = 5,  ///< any watchdog
    Brownout  = 6,  ///< supply dipped - on a battery device usually the real story
    DeepSleep = 7,  ///< woke from deep sleep; RTC RAM is valid
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

const char* iotResetReasonToString(IotResetReason reason);
const char* iotWakeupCauseToString(IotWakeupCause cause);

/**
 * Ending a wakeup cycle, and how the previous one ended.
 *
 * A port so that host tests can reach a failure path without the process
 * exiting in a real esp_deep_sleep(). The escalating backoff *policy* is not
 * part of it - that is plain logic, @see IotBackoffPolicy.
 *
 * **Absent (nullptr):** not supported; every exit path needs it. A non-sleeping
 * device uses an adapter whose deepSleep() is a busy wait.
 */
class IotPower
{
public:
    virtual ~IotPower() = default;

    /**
     * Enter deep sleep for @p duration_s and wake through a full boot. Does not
     * return on a real device; a test double records the call and returns.
     */
    virtual void deepSleep(int32_t duration_s) = 0;

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

/// @return true if the previous cycle ended abnormally (crash, watchdog or
/// brownout) rather than in an orderly exit or a plain power-on. The trigger
/// for the escalating backoff.
inline bool iotIsAbnormalReset(IotResetReason reason)
{
    return reason == IotResetReason::Panic
        || reason == IotResetReason::Watchdog
        || reason == IotResetReason::Brownout;
}
