/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#include "iot_endpoints.h"

#include "Arduino.h"

#include "iot_api.h"
#include "iot_ota_internal.h"

// *****************************************************************************

IotResult iotForward(std::string& oResponse, const std::string& forwardingName,
    const std::string& remainingPath, const std::string& body,
    const std::map<std::string, std::string>& headers)
{
    std::string apiPath = std::string(iot_paths::FORWARD) + forwardingName;
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
    return api.apiPut(response, std::string(iot_paths::FILE_DIR) + filename, content,
        {{"Content-Type", contentType}});
}

// *****************************************************************************
// Firmware
// *****************************************************************************

/// The OTA downloader opens its own connection, so it needs the TLS trust
/// separately from the API's HTTP client.
static class IotOtaInternal ota;

static void applyTlsToOta(const IotTlsConfig& tls)
{
    switch (tls.mode)
    {
        case IotTlsMode::Bundle:
            ota.setServerCertBundle(true);
            break;
        case IotTlsMode::Insecure:
            ota.setServerCert(nullptr, true);
            ota.setClientCert(nullptr, nullptr, nullptr);
            break;
        case IotTlsMode::CaPin:
            if (!tls.caCertPem.empty())
            {
                ota.setServerCert(tls.caCertPem.c_str(), false);
            }
            if (!tls.clientCertPem.empty() && !tls.clientKeyPem.empty())
            {
                ota.setClientCert(tls.clientCertPem.c_str(), tls.clientKeyPem.c_str(), nullptr);
            }
            break;
        case IotTlsMode::None:
            break;
    }
}

std::string iotFirmwareHttpEtag()
{
    return api.store().getString(iot_nvs_keys::FIRMWARE_ETAG, "");
}

std::string iotFirmwareHttpDate()
{
    return api.store().getString(iot_nvs_keys::FIRMWARE_DATE, "");
}

IotResult iotUpdateFirmware(const std::string& apiPath,
    const std::map<std::string, std::string>& header)
{
    std::map<std::string, std::string> conditional = {
        { "If-None-Match", iotFirmwareHttpEtag() },
        { "If-Modified-Since", iotFirmwareHttpDate() },
    };
    for (auto const& kv : header) { conditional[kv.first] = kv.second; }

    // HEAD first. apiRequest() may replace the token on a 401 retry, so the OTA
    // headers are built after this call.
    std::string response;
    std::map<std::string, std::string> responseHeader;
    const int httpStatusCode = api.apiRequest(response, responseHeader, "HEAD", apiPath, "", conditional);
    if (httpStatusCode != 200)
    {
        log_i("No firmware update available status=%d", httpStatusCode);
        return IotResult(httpStatusCode);
    }

    // an OTA download wants the cache validators and the token, but none of the
    // JSON content negotiation an API call gets
    std::map<std::string, std::string> otaHeader = api.mergeHeaders(
        { { "Authorization", api.deviceToken() } }, conditional);
    std::map<std::string, std::string> effective;
    for (auto const& kv : otaHeader)
    {
        if (!kv.second.empty()) { effective[kv.first] = kv.second; }
    }

    applyTlsToOta(api.tlsConfig());

    std::string newEtag;
    std::string newDate;
    if (!ota.updateFirmwareFromUrl(newEtag, newDate, api.getApiUrlForPath(apiPath).c_str(), &effective))
    {
        // an update was available (HEAD 200) but the download or flash did not
        // complete - report a distinct, non-transient failure
        log_e("Firmware update failed");
        return IotResult(IotResult::STATUS_UPDATE_FAILED);
    }

    api.store().putString(iot_nvs_keys::FIRMWARE_ETAG, newEtag);
    api.store().putString(iot_nvs_keys::FIRMWARE_DATE, newDate);
    log_i("Firmware update successful");
    return IotResult(200);
}
