/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#include "iot_endpoints.h"

#include "iot_api.h"

// *****************************************************************************

IotResult iotForward(std::string& oResponse, const std::string& forwardingName,
    const std::string& remainingPath, const std::string& body,
    const std::map<std::string, std::string>& headers)
{
    std::string apiPath = "forward/{project}/{device}/" + forwardingName;
    if (!remainingPath.empty())
    {
        apiPath += (remainingPath[0] == '/') ? remainingPath : "/" + remainingPath;
    }
    return api.apiGet(oResponse, apiPath, body, headers);
}

// *****************************************************************************

IotResult iotUploadFile(const std::string& filename, const std::string& content, const std::string& contentType)
{
    std::string response;
    return api.apiPut(response, "file/{project}/{device}/" + filename, content,
        {{"Content-Type", contentType}});
}
