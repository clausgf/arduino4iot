/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

#include <map>
#include <string>

#include "iot_api.h"
#include "iot_result.h"

// *****************************************************************************
// The nice4iot endpoints: every server path the library knows, and the wrappers
// for the ones outside the wakeup cycle. Free functions over api, so the
// transport deals in HTTP and does not carry the endpoint map; they are linked
// only when an application references them.
// *****************************************************************************

/// Server paths. {project}, {device} and {board} are substituted per request,
/// {kind} by postTelemetry(). Pass a different path to override one.
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

/**
 * Send a request through the server's forwarding endpoint
 * (GET forward/{project}/{device}/{forwardingName}/{remainingPath}).
 *
 * The server proxies it to the upstream URL configured for @p forwardingName in
 * the project settings and returns the upstream response.
 *
 * @param forwardingName name of the forwarding entry in the project config
 * @param remainingPath path suffix appended to the configured upstream URL
 * @return the upstream response, its headers as requested in @p options
 */
IotApiResponse iotForward(const std::string& forwardingName,
    const std::string& remainingPath = "", const IotApiOptions& options = {});

/**
 * Upload a file to the device-specific file storage
 * (PUT file/{project}/{device}/{filename}), e.g. for a diagnostic dump.
 *
 * The filename may only contain alphanumeric characters, '.', '_' and '-' and
 * must start with an alphanumeric character.
 *
 * @return Ok on 2xx, otherwise inspect .httpStatus (e.g. 413 if the file
 *         exceeds the server's size limit) or .transportError
 */
IotResult iotUploadFile(const std::string& filename, const std::string& content,
    const std::string& contentType = "application/octet-stream");

/**
 * Update the firmware from @p apiPath.
 *
 * Checks conditionally (ETag / Last-Modified) and streams a new image only when
 * one is available, using the same TLS trust as the API. The {board}
 * placeholder substitutes to IOT_BOARD_ID, which lets a project serve one image
 * per hardware variant.
 *
 * @return Ok if new firmware was flashed (it runs after the next restart);
 *         isNotModified() (304) if already up to date;
 *         .httpStatus == IotResult::STATUS_UPDATE_FAILED if an update was
 *         available but the download or flash did not complete;
 *         otherwise the HTTP or transport error.
 *         Use isOkOrNotModified() to test for "no error".
 */
IotResult iotUpdateFirmware(const std::string& apiPath = iot_paths::FIRMWARE,
    const std::map<std::string, std::string>& header = {});

/// @return the ETag of the running firmware image, for diagnostics.
std::string iotFirmwareHttpEtag();
/// @return the Last-Modified date of the running firmware image.
std::string iotFirmwareHttpDate();
