/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

#include <map>
#include <string>

#include "iot_api.h"
#include "iot_cache.h"
#include "iot_result.h"

// *****************************************************************************
// The nice4iot endpoints: every server path the library knows, and the
// endpoint groups over api, so the transport deals in HTTP and does not carry
// the endpoint map.
// *****************************************************************************

/// Server paths. {project}, {device} and {board} are substituted per request,
/// {kind} by iot.telemetry.post(). Pass a different path to override one.
namespace iot_paths
{
    inline constexpr const char* PROVISION = "provision";
    inline constexpr const char* TELEMETRY = "telemetry/{project}/{device}/{kind}";
    inline constexpr const char* LOG       = "log/{project}/{device}";
    inline constexpr const char* CONFIG    = "file/{project}/{device}/config.json";
    inline constexpr const char* FIRMWARE  = "file/{project}/{device}/firmware.bin";
    inline constexpr const char* FILE_DIR  = "file/{project}/{device}/";
    inline constexpr const char* FORWARD   = "forward/{project}/{device}/";
}

class IotTelemetry;

// *****************************************************************************
// Endpoint groups, reached as members of the facade: iot.telemetry, iot.files,
// iot.forward, iot.ota. Stateless - each forwards to the global api - and a
// method an application never calls is not linked.
// *****************************************************************************

/// telemetry/{project}/{device}/{kind}
struct IotTelemetryEndpoint
{
    /**
     * Post telemetry built with an IotTelemetry builder, which guarantees the
     * flat JSON object of numbers the server expects.
     *
     *   IotTelemetry t;
     *   t.add("temperature", 22.5);
     *   if (!iot.telemetry.post("sensors", t)) { ... }
     */
    IotResult post(const std::string& kind, const IotTelemetry& telemetry,
        const std::string& apiPath = iot_paths::TELEMETRY) const;

    /// Post a JSON body as is. Bodies larger than IOT_MAX_TELEMETRY_SIZE log a
    /// warning: the server rejects them by default (HTTP 413).
    IotResult post(const std::string& kind, const std::string& jsonData,
        const std::string& apiPath = iot_paths::TELEMETRY) const;

    /// Post the system telemetry: battery voltage if a battery is attached,
    /// RSSI, boot count, active and sleep durations, time and firmware.
    IotResult postSystem(const std::string& kind = "system",
        const std::string& apiPath = iot_paths::TELEMETRY) const;
};

/// file/{project}/{device}/
struct IotFileEndpoint
{
    /**
     * Upload a file to the device's file storage, e.g. a diagnostic dump. The
     * name may only contain alphanumeric characters, '.', '_' and '-' and must
     * start with an alphanumeric character.
     *
     * @return Ok on 2xx, otherwise inspect .httpStatus (e.g. 413 if the file
     *         exceeds the server's size limit) or .transportError
     */
    IotResult upload(const std::string& filename, const std::string& content,
        const std::string& contentType = "application/octet-stream") const;

    /**
     * Fetch a file of the device's file storage if it changed, @see
     * IotApi::getCached(). The server falls back to the project's file of the
     * same name.
     */
    IotCachedResponse getCached(const std::string& filename, const IotCacheValidators& current,
        const IotApiOptions& options = {}) const;
};

/// forward/{project}/{device}/{forwardingName}/{remainingPath}
struct IotForwardEndpoint
{
    /**
     * GET through the server's forwarding endpoint. The server proxies it to
     * the upstream URL configured for @p forwardingName in the project
     * settings and returns the upstream response.
     *
     * @param remainingPath path suffix appended to the configured upstream URL
     * @param options extra headers and the response headers to collect
     */
    IotApiResponse get(const std::string& forwardingName, const std::string& remainingPath = "",
        const IotApiOptions& options = {}) const;

    /// The same as a conditional GET, @see IotApi::getCached().
    IotCachedResponse getCached(const std::string& forwardingName, const std::string& remainingPath,
        const IotCacheValidators& current, const IotApiOptions& options = {}) const;
};

/// Firmware update over file/{project}/{device}/firmware.bin
struct IotOtaEndpoint
{
    /**
     * Update the firmware from @p apiPath.
     *
     * Checks conditionally (ETag / Last-Modified) and streams a new image only
     * when one is available, using the same TLS trust as the API. The {board}
     * placeholder substitutes to IOT_BOARD_ID, which lets a project serve one
     * image per hardware variant.
     *
     * @return Ok if new firmware was flashed (it runs after the next restart);
     *         isNotModified() (304) if already up to date;
     *         .httpStatus == IotResult::STATUS_UPDATE_FAILED if an update was
     *         available but the download or flash did not complete;
     *         otherwise the HTTP or transport error.
     *         Use isOkOrNotModified() to test for "no error".
     */
    IotResult update(const std::string& apiPath = iot_paths::FIRMWARE,
        const std::map<std::string, std::string>& header = {}) const;

    /// @return the ETag of the installed firmware image, for diagnostics.
    std::string etag() const;
    /// @return the Last-Modified date of the installed firmware image.
    std::string lastModified() const;
};
