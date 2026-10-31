/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#include "iot_endpoints.h"

#include "Arduino.h"

#include "iot_api.h"
#include "iot_cache.h"
#include "iot_ota_internal.h"

// *****************************************************************************

static std::string forwardPath(const std::string& forwardingName, const std::string& remainingPath)
{
    std::string apiPath = std::string(iot_paths::FORWARD) + forwardingName;
    if (!remainingPath.empty())
    {
        apiPath += (remainingPath[0] == '/') ? remainingPath : "/" + remainingPath;
    }
    return apiPath;
}

IotApiResponse IotForwardEndpoint::get(const std::string& forwardingName, const std::string& remainingPath,
    const IotApiOptions& options) const
{
    return api.get(forwardPath(forwardingName, remainingPath), options);
}

IotCachedResponse IotForwardEndpoint::getCached(const std::string& forwardingName,
    const std::string& remainingPath, const IotCacheValidators& current, const IotApiOptions& options) const
{
    return api.getCached(forwardPath(forwardingName, remainingPath), current, options);
}

// *****************************************************************************

IotResult IotFileEndpoint::upload(const std::string& filename, const std::string& content,
    const std::string& contentType) const
{
    return api.put(std::string(iot_paths::FILE_DIR) + filename, content,
        { .headers = { { "Content-Type", contentType } } }).result;
}

IotCachedResponse IotFileEndpoint::getCached(const std::string& filename, const IotCacheValidators& current,
    const IotApiOptions& options) const
{
    return api.getCached(std::string(iot_paths::FILE_DIR) + filename, current, options);
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

IotCacheValidators IotOtaEndpoint::validators() const
{
    return IotStoredValidators(api.store(), iot_nvs_keys::FIRMWARE_ETAG, iot_nvs_keys::FIRMWARE_DATE).load();
}

IotResult IotOtaEndpoint::update(const std::string& apiPath,
    const std::map<std::string, std::string>& header) const
{
    IotStoredValidators cache(api.store(), iot_nvs_keys::FIRMWARE_ETAG, iot_nvs_keys::FIRMWARE_DATE);
    const IotCacheValidators current = cache.load();
    std::map<std::string, std::string> conditional = {
        { "If-None-Match", current.etag },
        { "If-Modified-Since", current.lastModified },
    };
    for (auto const& kv : header) { conditional[kv.first] = kv.second; }

    // HEAD first. send() may replace the token on a 401 retry, so the OTA
    // headers are built after this call.
    const IotResult check = api.head(apiPath, { .headers = conditional }).result;
    if (check.httpStatus != 200)
    {
        log_i("No firmware update available status=%d", check.httpStatus);
        return check;
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
    if (!ota.updateFirmwareFromUrl(newEtag, newDate, api.urlForPath(apiPath).c_str(), &effective))
    {
        // an update was available (HEAD 200) but the download or flash did not
        // complete - report a distinct, non-transient failure
        log_e("Firmware update failed");
        return IotResult(IotResult::STATUS_UPDATE_FAILED);
    }

    cache.commit({ newEtag, newDate });
    log_i("Firmware update successful");
    return IotResult(200);
}
