/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

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
 *   iot.postTelemetry("sensors", telemetry);
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
