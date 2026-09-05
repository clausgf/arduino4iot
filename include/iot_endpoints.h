/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

#include <map>
#include <string>

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
 * @param oResponse the upstream response body
 * @param forwardingName name of the forwarding entry in the project config
 * @param remainingPath path suffix appended to the configured upstream URL
 * @param body an optional request body passed to the upstream service
 */
IotResult iotForward(std::string& oResponse, const std::string& forwardingName,
    const std::string& remainingPath = "", const std::string& body = "",
    const std::map<std::string, std::string>& headers = {});

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
