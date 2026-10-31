/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "iot_ports.h"

// *****************************************************************************
// Test doubles for the ports in iot_ports.h.
//
// Header-only and Arduino-free, for native test builds: a service written
// against the ports can be exercised entirely on the host, failure paths
// included. Firmware never includes this header.
// *****************************************************************************


// *****************************************************************************

/**
 * In-memory IotStore. Counts writes, so a test can assert the port's central
 * promise: put* must not write when the value is unchanged.
 */
class IotFakeStore : public IotStore
{
public:
    IotFakeStore() = default;

    bool has(const char* key) const override
    {
        return _entries.find(std::string(key)) != _entries.end();
    }

    std::string getString(const char* key, const std::string& defaultValue = "") const override
    {
        auto it = _entries.find(std::string(key));
        return (it != _entries.end() && it->second.isString) ? it->second.stringValue : defaultValue;
    }

    int64_t getInt(const char* key, int64_t defaultValue = 0) const override
    {
        auto it = _entries.find(std::string(key));
        return (it != _entries.end() && !it->second.isString) ? it->second.intValue : defaultValue;
    }

    bool getBool(const char* key, bool defaultValue = false) const override
    {
        auto it = _entries.find(std::string(key));
        return (it != _entries.end() && !it->second.isString) ? (it->second.intValue != 0) : defaultValue;
    }

    void putString(const char* key, const std::string& value) override
    {
        Entry& entry = _entries[std::string(key)];
        if (entry.valid && entry.isString && entry.stringValue == value)
        {
            return;
        }
        entry = Entry{ true, true, value, 0 };
        _writeCount++;
    }

    void putInt(const char* key, int64_t value) override
    {
        Entry& entry = _entries[std::string(key)];
        if (entry.valid && !entry.isString && entry.intValue == value)
        {
            return;
        }
        entry = Entry{ true, false, "", value };
        _writeCount++;
    }

    void putBool(const char* key, bool value) override
    {
        putInt(key, value ? 1 : 0);
    }

    void remove(const char* key) override
    {
        if (_entries.erase(std::string(key)) > 0)
        {
            _writeCount++;
        }
    }

    void clear() override
    {
        if (!_entries.empty())
        {
            _entries.clear();
            _writeCount++;
        }
    }

    // --- test inspection ---

    /// @return how many writes actually reached the "flash".
    unsigned writeCount() const { return _writeCount; }
    void resetWriteCount() { _writeCount = 0; }
    size_t size() const { return _entries.size(); }

private:
    struct Entry
    {
        bool valid = false;
        bool isString = false;
        std::string stringValue;
        int64_t intValue = 0;
    };

    std::map<std::string, Entry> _entries;
    unsigned _writeCount = 0;
};


// *****************************************************************************

/// IotClock whose time a test sets directly. sync() advances nothing by itself;
/// it reports syncResult and counts the calls.
class IotFakeClock : public IotClock
{
public:
    int64_t now() const override { return _now; }
    bool isPlausible() const override { return _now > IotClock::PLAUSIBLE_THRESHOLD; }
    int64_t lastSyncTime() const override { return _lastSyncTime; }

    bool sync(uint32_t timeout_ms) override
    {
        (void)timeout_ms;
        _syncCount++;
        if (syncResult)
        {
            _lastSyncTime = _now;
        }
        return syncResult;
    }

    // --- test control ---
    void setNow(int64_t unixSeconds) { _now = unixSeconds; }
    void advance(int64_t seconds) { _now += seconds; }
    void setLastSyncTime(int64_t unixSeconds) { _lastSyncTime = unixSeconds; }
    unsigned syncCount() const { return _syncCount; }

    bool syncResult = true;

private:
    int64_t _now = 0;
    int64_t _lastSyncTime = 0;
    unsigned _syncCount = 0;
};


// *****************************************************************************

/// IotNetwork that connects (or refuses to) on command.
class IotFakeNetwork : public IotNetwork
{
public:
    bool connect(uint32_t timeout_ms) override
    {
        (void)timeout_ms;
        _connectCount++;
        _connected = connectResult;
        return _connected;
    }

    void disconnect() override { _connected = false; }
    bool isConnected() const override { return _connected; }
    std::string macAddress() const override { return mac; }
    std::string localIp() const override { return _connected ? ip : std::string(); }
    int rssi() const override { return _connected ? rssiValue : 0; }
    uint32_t lastConnectDuration_ms() const override { return _connected ? connectDuration_ms : 0; }
    std::string lastError() const override { return _connected ? std::string() : connectError; }

    // --- test control ---
    unsigned connectCount() const { return _connectCount; }
    void setConnected(bool connected) { _connected = connected; }

    bool connectResult = true;
    std::string mac = "aabbccddeeff";
    std::string ip = "192.168.1.42";
    int rssiValue = -55;
    uint32_t connectDuration_ms = 350;
    std::string connectError = "ssid not found";

private:
    bool _connected = false;
    unsigned _connectCount = 0;
};


// *****************************************************************************

/// IotBattery returning a voltage a test sets. Default -1, "unavailable".
class IotFakeBattery : public IotBattery
{
public:
    int readMilliVolts() override
    {
        _readCount++;
        return milliVolts;
    }

    unsigned readCount() const { return _readCount; }

    int milliVolts = -1;

private:
    unsigned _readCount = 0;
};


// *****************************************************************************

/**
 * IotPower that records how the cycle ended instead of ending it: the exit
 * methods return, so a test can assert what was decided and carry on.
 */
class IotFakePower : public IotPower
{
public:
    enum class Action { None, DeepSleep, Restart, Shutdown };

    struct Call
    {
        Action action = Action::None;
        uint32_t duration_s = 0;
    };

    void deepSleep(uint32_t duration_s) override { _calls.push_back({ Action::DeepSleep, duration_s }); }
    void restart() override { _calls.push_back({ Action::Restart, 0 }); }
    void shutdown() override { _calls.push_back({ Action::Shutdown, 0 }); }

    IotResetReason resetReason() const override { return resetReasonValue; }
    IotWakeupCause wakeupCause() const override { return wakeupCauseValue; }
    uint32_t uptime_ms() const override { return uptimeValue_ms; }

    // --- test inspection ---
    const std::vector<Call>& calls() const { return _calls; }
    bool wasCalled() const { return !_calls.empty(); }
    Call lastCall() const { return _calls.empty() ? Call{} : _calls.back(); }
    void reset() { _calls.clear(); }

    // --- test control ---
    IotResetReason resetReasonValue = IotResetReason::PowerOn;
    IotWakeupCause wakeupCauseValue = IotWakeupCause::Unknown;
    uint32_t uptimeValue_ms = 0;

private:
    std::vector<Call> _calls;
};


// *****************************************************************************

/**
 * IotHttp that answers from a queue instead of a network.
 *
 * Records every request, so a test can assert what the transport sent - the
 * merged headers, the substituted URL, the retry after a 401.
 */
class IotFakeHttp : public IotHttp
{
public:
    void setTls(const IotTlsConfig& tls) override { _tls = &tls; }
    bool hasServerTrust() const override { return _tls != nullptr && _tls->hasServerTrust(); }
    void setConnectTimeout_ms(int32_t timeout_ms) override { connectTimeout_ms = timeout_ms; }
    void setRequestTimeout_ms(uint16_t timeout_ms) override { requestTimeout_ms = timeout_ms; }
    void close() override { closeCount++; }
    std::string errorToString(int status) const override { return "fake error " + std::to_string(status); }

    int send(const IotHttpRequest& request, IotHttpResponse& oResponse) override
    {
        requests.push_back(request);
        if (_replies.empty())
        {
            oResponse.status = defaultStatus;
        } else {
            oResponse = _replies.front();
            _replies.erase(_replies.begin());
        }
        return oResponse.status;
    }

    // --- test control ---

    /// Queue one reply; they are consumed in order.
    void reply(int status, const std::string& body = "",
        const std::map<std::string, std::string>& headers = {})
    {
        IotHttpResponse response;
        response.status = status;
        response.body = body;
        response.headers = headers;
        _replies.push_back(response);
    }

    /// Status used once the queue runs dry.
    int defaultStatus = 200;
    int32_t connectTimeout_ms = -1;
    uint16_t requestTimeout_ms = 0;
    unsigned closeCount = 0;

    // --- test inspection ---

    std::vector<IotHttpRequest> requests;
    size_t requestCount() const { return requests.size(); }
    const IotHttpRequest& lastRequest() const { return requests.back(); }
    const IotHttpRequest& request(size_t index) const { return requests.at(index); }

    /// @return the value of @p name in request @p index, or "" if absent.
    std::string header(size_t index, const char* name) const
    {
        auto it = requests.at(index).headers.find(name);
        return it == requests.at(index).headers.end() ? std::string() : it->second;
    }

private:
    const IotTlsConfig* _tls = nullptr;
    std::vector<IotHttpResponse> _replies;
};
