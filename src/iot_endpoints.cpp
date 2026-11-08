/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#include "iot_endpoints.h"

#include "Arduino.h"
#include "sdkconfig.h"

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

/// @return why the OTA downloader cannot fetch @p url with @p tls in this
/// firmware, or nullptr if it can. esp_https_ota only accepts what the
/// framework's sdkconfig enables, and would fail with an unexplained error.
static const char* otaUnsupportedReason(const std::string& url, const IotTlsConfig& tls)
{
    if (url.rfind("https://", 0) != 0)
    {
#ifdef CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP
        return nullptr;
#else
        return "the URL is not https:// and the framework is built without "
               "CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP";
#endif
    }
    switch (tls.mode)
    {
        case IotTlsMode::Bundle:
            return nullptr;
        case IotTlsMode::CaPin:
            return tls.caCertPem.empty() ? "tlsMode CaPin without a CA certificate" : nullptr;
        case IotTlsMode::Insecure:
            // esp_https_ota refuses a config without a certificate or bundle
            // unless it allows plain http, and esp-tls then needs to be told
            // to skip the verification
#if defined(CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP) && defined(CONFIG_ESP_TLS_INSECURE) \
    && defined(CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY)
            return nullptr;
#else
            return "tlsMode Insecure, but the framework is built without "
                   "CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP/CONFIG_ESP_TLS_INSECURE - seed Bundle or CaPin";
#endif
        case IotTlsMode::None:
            break;
    }
    return "no TLS server trust seeded";
}

IotCacheValidators IotOtaEndpoint::validators() const
{
    if (!api.isInitialized()) { return IotCacheValidators{}; }
    return IotStoredValidators(api.store(), iot_nvs_keys::FIRMWARE_ETAG, iot_nvs_keys::FIRMWARE_DATE).load();
}

IotResult IotOtaEndpoint::update(const std::string& apiPath,
    const std::map<std::string, std::string>& header) const
{
    if (!api.isInitialized())
    {
        log_e("Firmware update: IotApi not initialized - call iot.begin() first");
        return IotResult(IotResult::STATUS_NOT_CONFIGURED);
    }
    // checked before the HEAD: an update this firmware cannot download would
    // otherwise be found, and fail, every cycle
    const std::string url = api.urlForPath(apiPath);
    if (const char* reason = otaUnsupportedReason(url, api.tlsConfig()))
    {
        log_e("Firmware update from %s impossible: %s", url.c_str(), reason);
        return IotResult(IotResult::STATUS_NOT_CONFIGURED);
    }

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
        if (check.isNotModified() || check.httpStatus == 404)
        {
            log_i("No firmware update available status=%d", check.httpStatus);
        } else {
            log_w("Firmware update check failed status=%d", check.code());
        }
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
    if (!ota.updateFirmwareFromUrl(newEtag, newDate, url.c_str(), &effective))
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
