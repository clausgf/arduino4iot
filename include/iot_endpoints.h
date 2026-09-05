/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

#include <map>

#include "Arduino.h"

#include "iot_result.h"

// *****************************************************************************
// Convenience wrappers for nice4iot endpoints that are not part of the wakeup
// cycle. Free functions over api.apiRequest(), so the transport itself deals in
// HTTP and does not carry the server's endpoint map; they are linked only when
// an application references them.
// *****************************************************************************

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
IotResult iotForward(String& oResponse, const String& forwardingName,
    const String& remainingPath = "", const String& body = "",
    const std::map<String, String>& headers = {});

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
IotResult iotUploadFile(const String& filename, const String& content,
    const String& contentType = "application/octet-stream");
