/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>

#include <ArduinoJson.h>

// *****************************************************************************

/**
 * Default maximum body size accepted by a nice4iot server for telemetry
 * and log requests (app_config.max_telemetry_size / max_log_size).
 * Larger requests are rejected with HTTP 413.
 */
static constexpr size_t IOT_MAX_TELEMETRY_SIZE = 8192;

/**
 * Builder for telemetry payloads.
 *
 * The nice4iot server expects a flat JSON object whose values are numbers;
 * non-numeric values are silently ignored by the server's telemetry backend.
 * This builder guarantees a flat, valid JSON object and avoids manual
 * string concatenation.
 *
 * Example:
 *   IotTelemetry telemetry;
 *   telemetry.add("temperature", 22.5).add("humidity", 40);
 *   iot.telemetry.post("sensors", telemetry);
 *
 * This class intentionally only depends on ArduinoJson, so it can be
 * unit-tested on the native platform.
 */
class IotTelemetry
{
public:
    IotTelemetry() {}

    /**
     * Add a measurement under the given key. Numeric values are recommended;
     * string values are accepted but ignored by the nice4iot telemetry backend.
     * Adding an existing key overwrites its value.
     */
    template <typename T>
    IotTelemetry& add(const char *key, T value)
    {
        _doc[key] = value;
        return *this;
    }

    /// Remove all measurements.
    void clear() { _doc.clear(); }

    /// @return true if no measurement was added yet
    bool isEmpty() const { return _doc.isNull() || _doc.as<JsonObjectConst>().size() == 0; }

    /// @return the size of the serialized JSON document in bytes
    size_t measure() const { return measureJson(_doc); }

    /**
     * Serialize the telemetry data as JSON into the given destination
     * (e.g. Arduino String or std::string).
     * @return the number of bytes written
     */
    template <typename TDestination>
    size_t serializeTo(TDestination& destination) const
    {
        return serializeJson(_doc, destination);
    }

    /// Access the underlying JSON document, e.g. for nested data (not supported by nice4iot)
    JsonDocument& json() { return _doc; }

private:
    JsonDocument _doc;
};

// *****************************************************************************

/**
 * Measurements for the next cycle's telemetry, kept in RTC RAM.
 *
 * Some values are only known after the network is gone - a display refresh
 * that runs after iot.radioOff(), the length of the whole cycle. Staying awake
 * with the radio on just to send them costs more than they are worth. Keep
 * them here instead; the next cycle's post of the same kind adds them as
 * last_<key>. The prefix is honest about it: nice4iot stamps a measurement
 * with its arrival time, so the values belong to the previous cycle.
 *
 * Trivially copyable and without a constructor, so it is a plain member of
 * the struct in an IotRtcSlot; the application chooses the size and pays the
 * RTC RAM only when it uses it:
 *
 *   struct AppState { IotRtcTelemetry<8> next; };
 *   static RTC_NOINIT_ATTR IotRtcSlot<AppState> rtc;
 *
 *   rtc->next.add("refresh_ms", refreshDuration_ms);   // end of this cycle
 *   iot.telemetry.post("epaper", t, rtc->next);         // next cycle: last_refresh_ms
 *
 * @tparam N the number of values it holds
 */
template <size_t N>
struct IotRtcTelemetry
{
    static_assert(N > 0 && N < 256, "IotRtcTelemetry: N must be 1..255");

    /// Keys longer than this minus 1 are rejected.
    static constexpr size_t KEY_CAPACITY = 24;

    struct Entry
    {
        char key[KEY_CAPACITY];
        bool isInteger;
        int64_t integer;
        double real;
    };

    uint8_t count;
    Entry entries[N];

    /**
     * Keep @p value under @p key; the same key again overwrites it.
     * @return false if the key is too long or all N places are taken
     */
    template <typename T>
    bool add(const char* key, T value)
    {
        static_assert(std::is_arithmetic<T>::value, "IotRtcTelemetry holds numbers only");
        if (key == nullptr || std::strlen(key) >= KEY_CAPACITY) { return false; }
        Entry* entry = _find(key);
        if (entry == nullptr)
        {
            if (count >= N) { return false; }
            entry = &entries[count++];
            std::memcpy(entry->key, key, std::strlen(key) + 1);
        }
        entry->isInteger = std::is_integral<T>::value;
        entry->integer = std::is_integral<T>::value ? (int64_t)value : 0;
        entry->real = std::is_integral<T>::value ? 0.0 : (double)value;
        return true;
    }

    bool isEmpty() const { return count == 0; }
    void clear() { count = 0; }

    /// Add every value to @p telemetry as last_<key>, then empty this.
    void moveInto(IotTelemetry& telemetry)
    {
        const uint8_t n = count <= N ? count : (uint8_t)N;   // defensive: outside a slot, count may be garbage
        for (uint8_t i = 0; i < n; i++)
        {
            const Entry& entry = entries[i];
            if (std::memchr(entry.key, '\0', KEY_CAPACITY) == nullptr) { continue; }
            const std::string key = std::string("last_") + entry.key;
            if (entry.isInteger) { telemetry.add(key.c_str(), entry.integer); }
            else { telemetry.add(key.c_str(), entry.real); }
        }
        clear();
    }

private:
    Entry* _find(const char* key)
    {
        for (uint8_t i = 0; i < count && i < N; i++)
        {
            if (std::strncmp(entries[i].key, key, KEY_CAPACITY) == 0) { return &entries[i]; }
        }
        return nullptr;
    }
};
