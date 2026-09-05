/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#include "iot_endpoints.h"

#include "iot_api.h"

// *****************************************************************************

IotResult iotForward(String& oResponse, const String& forwardingName,
    const String& remainingPath, const String& body,
    const std::map<String, String>& headers)
{
    String apiPath = "forward/{project}/{device}/" + forwardingName;
    if (!remainingPath.isEmpty())
    {
        apiPath += remainingPath.startsWith("/") ? remainingPath : "/" + remainingPath;
    }
    return api.apiGet(oResponse, apiPath, body, headers);
}

// *****************************************************************************

IotResult iotUploadFile(const String& filename, const String& content, const String& contentType)
{
    String response;
    return api.apiPut(response, "file/{project}/{device}/" + filename, content,
        {{"Content-Type", contentType}});
}
