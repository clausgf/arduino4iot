/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#include "iot_api.h"

#include <esp_ota_ops.h>
#include <WiFi.h>
#include <Preferences.h>
#include <ArduinoJson.h>

#include "iot_ota_internal.h"
#include "iot_text.h"
#include "iot.h"

// *****************************************************************************

IotApi api;
static class IotOtaInternal ota;

// *****************************************************************************

IotApi::IotApi():
    _store("iot")
{
    _baseUrl = "";
    _defaultRequestHeader = {};
    _projectName = "";
    _deviceName = "";

    _wifiClientSecurePtr = nullptr;
    _wifiClientPtr = nullptr;
    _httpClientPtr = nullptr;

    _tlsServerTrustConfigured = false;
    _tlsTrustWarningLogged = false;
}

void IotApi::begin()
{
    // one NVS handle for the whole wakeup cycle, and every read guarded
    // against the unfilterable "NOT_FOUND" error Preferences logs
    _store.begin();
    _setApiUrl(_store.getString(_nvram_api_url_key, ""));
    _projectName = _store.getString(_nvram_project_key, "");
    _caCertPem = _store.getString(_nvram_ca_cert_key, "");
    _clientCertPem = _store.getString(_nvram_client_cert_key, "");
    _clientKeyPem = _store.getString(_nvram_client_key_key, "");
    uint8_t tlsMode = (uint8_t)_store.getInt(_nvram_tls_mode_key, (int64_t)IotTlsMode::None);

    // the credentials load themselves from the same store
    _tokens.begin(_store);

    // apply TLS after the base URL is known (the secure client is created lazily
    // from the https:// URL)
    _applyTls((IotTlsMode)tlsMode);
}

void IotApi::_seedFromConfig(Preferences& preferences, const IotSeedConfig& cfg, bool force)
{
    // seeding uses the shared Preferences handle opened by
    // Iot::seedCredentials(), which runs before begin() opens _store
    String oldApiUrl = nvramGetString(preferences, _nvram_api_url_key, "");
    String oldProjectName = nvramGetString(preferences, _nvram_project_key, "");
    String oldProvisioningToken = nvramGetString(preferences, IotTokenManager::KEY_PROVISIONING_TOKEN, "");

    iotSeedString(preferences, _nvram_api_url_key, cfg.apiUrl, force);
    iotSeedString(preferences, _nvram_project_key, cfg.projectName, force);
    iotSeedString(preferences, IotTokenManager::KEY_PROVISIONING_TOKEN, cfg.provisioningToken, force);
    iotSeedString(preferences, _nvram_ca_cert_key, cfg.caCertPem, force);
    iotSeedString(preferences, _nvram_client_cert_key, cfg.clientCertPem, force);
    iotSeedString(preferences, _nvram_client_key_key, cfg.clientKeyPem, force);
    if (cfg.tlsMode != IotTlsMode::None && (force || !preferences.isKey(_nvram_tls_mode_key)))
    {
        preferences.putUChar(_nvram_tls_mode_key, (uint8_t)cfg.tlsMode);
    }

    // a device token issued for the old project/endpoint/provisioning token is
    // invalid under the newly seeded identity - drop it so updateProvisioning()
    // re-provisions immediately instead of waiting for the stale token to expire
    bool identityChanged =
        nvramGetString(preferences, _nvram_api_url_key, "") != oldApiUrl ||
        nvramGetString(preferences, _nvram_project_key, "") != oldProjectName ||
        nvramGetString(preferences, IotTokenManager::KEY_PROVISIONING_TOKEN, "") != oldProvisioningToken;
    if (identityChanged)
    {
        preferences.remove(IotTokenManager::KEY_DEVICE_TOKEN);
        preferences.remove(IotTokenManager::KEY_DEVICE_TOKEN_EXPIRY);
        log_w("_seedFromConfig: apiUrl/project/provisioningToken changed, invalidating device token");
    }
}

void IotApi::_applyTls(IotTlsMode mode)
{
    switch (mode)
    {
        case IotTlsMode::Bundle:
            _applyCACertBundle();
            break;
        case IotTlsMode::Insecure:
            _applyCertInsecure();
            break;
        case IotTlsMode::CaPin:
            if (!_caCertPem.empty())
            {
                _applyCACert(_caCertPem.c_str());
            }
            if (!_clientCertPem.empty() && !_clientKeyPem.empty())
            {
                _applyClientCertificateAndKey(_clientCertPem.c_str(), _clientKeyPem.c_str());
            }
            break;
        case IotTlsMode::None:
        default:
            break;
    }
}

void IotApi::end()
{
    _tokens.end();
    _store.end();
}


// *****************************************************************************

WiFiClient * IotApi::_getWiFiClientPtr()
{
    // create WiFiClient or WiFiClientSecure if needed
    if (_wifiClientPtr == nullptr)
    {
        if (_baseUrl.rfind("https://", 0) == 0)
        {
            log_d("Create WiFiClientSecure");
            _wifiClientSecurePtr = new WiFiClientSecure();
            _wifiClientPtr = _wifiClientSecurePtr;
        } else {
            log_d("Create WiFiClient");
            _wifiClientPtr = new WiFiClient();
        }
    }

    // ensure successful wifiClient creation
    if (_wifiClientPtr == nullptr)
    {
        iot.panicEarly("WiFiClient creation failed");
    }

    return _wifiClientPtr;
}

bool IotApi::_isWiFiClientSecure()
{
    _getWiFiClientPtr(); // ensure some _wifiClient is initialized
    return _wifiClientSecurePtr != nullptr;
}

// *****************************************************************************

HTTPClient & IotApi::_getHttpClient()
{
    // create HTTPClient if needed
    if (_httpClientPtr == nullptr)
    {
        log_d("Create HTTPClient");
        _httpClientPtr = new HTTPClient();
    }

    // ensure successful HTTPClient creation
    if (_httpClientPtr == nullptr)
    {
        iot.panicEarly("HTTPClient creation failed");
    }

    _httpClientPtr->setReuse(true);
    return *_httpClientPtr;
}


// *****************************************************************************
// API configuration
// *****************************************************************************

void IotApi::_setApiUrl(const std::string& apiBaseurl)
{
    _baseUrl = apiBaseurl;
    if (!_baseUrl.empty() && _baseUrl.back() != '/')
    {
        _baseUrl += "/";
    }
}

void IotApi::setDeviceName(const std::string& device)
{
    _deviceName = device;
}

void IotApi::setApiHeader(const std::map<std::string, std::string>& header)
{
    _defaultRequestHeader = header;
}

void IotApi::_applyCACert(const char *server_certificate)
{
    if (_isWiFiClientSecure())
    {
        _wifiClientSecurePtr->setCACert(server_certificate);
        ota.setServerCert(server_certificate, false);
        _tlsServerTrustConfigured = true;
    } else {
        log_e("_applyCACert: WiFiClientSecure not used");
    }
}

void IotApi::_applyCACertBundle()
{
    // symbols of the certificate bundle embedded by the build
    // (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y, default for arduino-esp32)
    extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
    extern const uint8_t rootca_crt_bundle_end[]   asm("_binary_x509_crt_bundle_end");
    if (_isWiFiClientSecure())
    {
        _wifiClientSecurePtr->setCACertBundle(rootca_crt_bundle_start,
            rootca_crt_bundle_end - rootca_crt_bundle_start);
        ota.setServerCertBundle(true);
        _tlsServerTrustConfigured = true;
    } else {
        log_e("_applyCACertBundle: WiFiClientSecure not used");
    }
}

void IotApi::_applyClientCertificateAndKey(const char *client_certificate, const char *client_key)
{
    if (_isWiFiClientSecure())
    {
        _wifiClientSecurePtr->setCertificate(client_certificate);
        _wifiClientSecurePtr->setPrivateKey(client_key);
        ota.setClientCert(client_certificate, client_key, nullptr);
    } else {
        log_e("_applyClientCertificateAndKey: WiFiClientSecure not used");
    }
}

void IotApi::_applyCertInsecure()
{
    log_w("TLS server authentication is DISABLED (seeded tlsMode Insecure) - the "
          "connection is encrypted but the server identity is not verified. Do not "
          "use in production; seed a CA certificate (tlsMode CaPin) instead.");
    if (_isWiFiClientSecure())
    {
        _wifiClientSecurePtr->setInsecure();
        _tlsServerTrustConfigured = true;
    } else {
        log_e("_applyCertInsecure: WiFiClientSecure not used");
    }
    ota.setServerCert(nullptr, true);
    ota.setClientCert(nullptr, nullptr, nullptr);
}

void IotApi::_warnIfTlsTrustMissing()
{
    if (_isWiFiClientSecure() && !_tlsServerTrustConfigured && !_tlsTrustWarningLogged)
    {
        _tlsTrustWarningLogged = true;
        log_e("TLS: https API URL but no server trust seeded - the handshake will "
              "fail with an opaque transport error. Seed a TLS mode via "
              "iot.seedCredentials() (CaPin with your CA, Bundle for public CAs, "
              "or - development only - Insecure).");
    }
}

void IotApi::closeConnection()
{
    // requests within a wakeup cycle share a single keep-alive connection
    // (setReuse(true)); this closes it cleanly, e.g. before deep sleep
    if (_httpClientPtr != nullptr)
    {
        _httpClientPtr->setReuse(false);
        _httpClientPtr->end();
    }
}

void IotApi::setConnectionTimeout_ms(int32_t timeout_ms)
{
    _getHttpClient().setConnectTimeout(timeout_ms);
}

void IotApi::setRequestTimeout_ms(uint16_t timeout_ms)
{
    _getHttpClient().setTimeout(timeout_ms);
}


// *****************************************************************************
// Provisioning
// *****************************************************************************

// The provisioning token is seeded into NVS by Iot::seedCredentials() and loaded
// by begin(); it is no longer set at runtime.

void IotApi::setDeviceToken(const std::string& deviceToken, time_t expiresAt)
{
    _tokens.setDeviceToken(std::string(deviceToken.c_str()), (int64_t)expiresAt);
}

void IotApi::clearDeviceToken()
{
    _tokens.clearDeviceToken();
}

// *****************************************************************************

IotResult IotApi::updateProvisioning(const std::string& apiPath)
{
    // whether the current token still does the job is a decision, not I/O
    if (!_tokens.needsNewToken((int64_t)time(nullptr), iot.isTimePlausible()))
    {
        log_i("updateProvisioning: already provisioned, token still valid");
        return IotResult(HTTP_CODE_OK);
    }
    if (_tokens.hasDeviceToken())
    {
        log_i("updateProvisioning: device token expired or expiring soon, re-provisioning");
    }
    return _requestDeviceToken(apiPath);
}

IotResult IotApi::_requestDeviceToken(const std::string& apiPath)
{
    if (!_tokens.hasProvisioningToken())
    {
        log_e("updateProvisioning: no provisioning token available");
        return IotResult(IotResult::STATUS_NO_PROVISIONING_TOKEN);
    }

    JsonDocument requestDoc;
    requestDoc["projectName"] = _projectName;
    requestDoc["deviceName"] = _deviceName;
    requestDoc["provisioningToken"] = _tokens.provisioningToken();
    std::string request;
    serializeJson(requestDoc, request);

    // unauthenticated single-shot: /provision authenticates with the
    // provisioning token in the body
    std::string response;
    std::map<std::string, std::string> responseHeader;
    int httpStatusCode = _performRequest(response, responseHeader, "POST", apiPath, request,
        _requestHeaders({}, /*authenticated=*/false), {});

    if (httpStatusCode < 200 || httpStatusCode >= 300)
    {
        log_i("updateProvisioning: status=%d", httpStatusCode);
        return IotResult(httpStatusCode);
    }

    IotProvisioningResponse parsed;
    if (!iotParseProvisioningResponse(response.c_str(), parsed))
    {
        // 2xx, but nothing usable in the body
        log_i("updateProvisioning: malformed response body");
        return IotResult(IotResult::STATUS_MALFORMED_RESPONSE);
    }

    int64_t expiresAt = _tokens.applyProvisioningResponse(
        parsed, (int64_t)time(nullptr), iot.isTimePlausible());
    if (expiresAt > 0)
    {
        log_i("updateProvisioning: new device token for api access, expires at %s",
            iot.getTimeIso((time_t)expiresAt).c_str());
    } else {
        log_i("updateProvisioning: new device token for api access, expiry unknown");
    }
    return IotResult(httpStatusCode);
}


// *****************************************************************************
// HTTP requests
// *****************************************************************************

std::string IotApi::getApiUrlForPath(const std::string& apiPath)
{
    return iot_text::buildApiUrl(
        _baseUrl.c_str(), apiPath.c_str(), _projectName.c_str(), _deviceName.c_str(),
        iot.getBoardId().c_str());
}

// *****************************************************************************

std::map<std::string, std::string> IotApi::_mergeHeaders(std::map<std::string, std::string> base,
    const std::map<std::string, std::string>& requestHeader) const
{
    for (auto const& kv : _defaultRequestHeader) { base[kv.first] = kv.second; }
    for (auto const& kv : requestHeader) { base[kv.first] = kv.second; }
    return base;
}

std::map<std::string, std::string> IotApi::_requestHeaders(const std::map<std::string, std::string>& requestHeader,
    bool authenticated) const
{
    std::map<std::string, std::string> base = {
        { "Accept", "application/json" },
        { "Content-Type", "application/json" },
    };
    if (authenticated)
    {
        base["Authorization"] = _tokens.deviceToken();
    }
    return _mergeHeaders(std::move(base), requestHeader);
}

// *****************************************************************************

int IotApi::_performRequest(std::string& oResponse, std::map<std::string, std::string>& oResponseHeader,
    const char * requestType, const std::string& apiPath, const std::string& requestBody,
    const std::map<std::string, std::string>& requestHeader,
    const std::vector<std::string>& collectResponseHeaderKeys)
{
    std::string url = getApiUrlForPath(apiPath);
    log_i("HTTP %s url=%s", requestType, url.c_str());

    // prepare HTTP request; requestHeader is already the fully merged set
    _warnIfTlsTrustMissing();
    _getHttpClient().begin(*(_getWiFiClientPtr()), String(url.c_str()));
    for (auto const& kv : requestHeader)
    {
        if (!kv.second.empty())
        {
            log_d("  HTTP header: %s=%s", kv.first.c_str(), kv.second.c_str());
            _getHttpClient().addHeader(String(kv.first.c_str()), String(kv.second.c_str()));
        }
    }
    std::vector<const char *> headerKeys;
    for (auto const& key : collectResponseHeaderKeys)
    {
        headerKeys.push_back(key.c_str());
    }
    if (!headerKeys.empty())
    {
        _getHttpClient().collectHeaders(headerKeys.data(), headerKeys.size());
    }

    // execute HTTP request
    int httpStatusCode = _getHttpClient().sendRequest(requestType, (uint8_t*)requestBody.c_str(), requestBody.length());
    for (auto const& key : collectResponseHeaderKeys)
    {
        if (_getHttpClient().hasHeader(key.c_str()))
        {
            oResponseHeader[key] = _getHttpClient().header(key.c_str()).c_str();
        }
    }
    if ((strcasecmp("HEAD", requestType) != 0) && httpStatusCode != HTTP_CODE_NOT_MODIFIED)
    {
        oResponse = _getHttpClient().getString().c_str();
    } else {
        oResponse = "";
    }

    // evaluate HTTP response
    if (httpStatusCode < 0)
    {
        // a missing TLS server trust is a common, hard-to-attribute cause of
        // transport errors on https - point at it explicitly
        const char *tlsHint = (_isWiFiClientSecure() && !_tlsServerTrustConfigured)
            ? " (no TLS server trust seeded: set tlsMode via iot.seedCredentials())"
            : "";
        log_e("HTTP %s url=%s -> status=%d error=%s%s",
            requestType, url.c_str(), httpStatusCode,
            _getHttpClient().errorToString(httpStatusCode).c_str(), tlsHint);
    } else if (httpStatusCode == HTTP_CODE_UNAUTHORIZED) {
        log_e("HTTP %s url=%s -> status=%d UNAUTHORIZED - device token invalid or expired",
            requestType, url.c_str(), httpStatusCode);
    } else if (httpStatusCode == HTTP_CODE_FORBIDDEN) {
        // 403 is not an auth-token problem (nice4iot returns 401 for those on the
        // device endpoints) - re-provisioning would not help, so keep the token
        log_e("HTTP %s url=%s -> status=%d FORBIDDEN - access refused by the server",
            requestType, url.c_str(), httpStatusCode);
    } else if (httpStatusCode < 200 || httpStatusCode >= 400) {
        log_e("HTTP %s url=%s requestBody=%s -> status=%d responseBody=%s",
            requestType, url.c_str(), requestBody.c_str(), httpStatusCode, oResponse.c_str());
    } else {
        log_i("HTTP %s url=%s -> status=%d", requestType, url.c_str(), httpStatusCode);
    }
    _getHttpClient().end();
    return httpStatusCode;
}

// *****************************************************************************

int IotApi::apiRequest(std::string& oResponse, std::map<std::string, std::string>& oResponseHeader,
    const char * requestType, const std::string& apiPath, const std::string& requestBody,
    const std::map<std::string, std::string>& requestHeader,
    const std::vector<std::string>& collectResponseHeaderKeys)
{
    // the authentication policy on top of the transport: attach the device
    // token, and on a rejection get a new one and try once more
    int httpStatusCode = _performRequest(oResponse, oResponseHeader,
        requestType, apiPath, requestBody,
        _requestHeaders(requestHeader, /*authenticated=*/true), collectResponseHeaderKeys);

    if (httpStatusCode == HTTP_CODE_UNAUTHORIZED && _tokens.hasProvisioningToken())
    {
        _tokens.clearDeviceToken();
        if (_requestDeviceToken("provision").isOk())
        {
            log_i("HTTP %s url=%s retrying after re-provisioning", requestType, apiPath.c_str());
            oResponse = "";
            oResponseHeader.clear();
            // rebuild the headers: they now carry the freshly issued token
            httpStatusCode = _performRequest(oResponse, oResponseHeader,
                requestType, apiPath, requestBody,
                _requestHeaders(requestHeader, /*authenticated=*/true), collectResponseHeaderKeys);
        }
    }
    return httpStatusCode;
}

// *****************************************************************************

int IotApi::apiGet(std::string& response, const std::string& apiPath, const std::string& body, const std::map<std::string, std::string>& header)
{
    std::map<std::string, std::string> responseHeader;
    return apiRequest(response, responseHeader, "GET", apiPath, body, header);
}

int IotApi::apiGet(std::string& response, std::map<std::string, std::string>& oResponseHeader, const std::string& apiPath,
    const std::vector<std::string>& collectResponseHeaderKeys, const std::string& body, const std::map<std::string, std::string>& header)
{
    return apiRequest(response, oResponseHeader, "GET", apiPath, body, header, collectResponseHeaderKeys);
}

// *****************************************************************************

int IotApi::apiHead(const std::string& apiPath, const std::map<std::string, std::string>& header)
{
    std::string response;
    std::map<std::string, std::string> responseHeader;
    return apiRequest(response, responseHeader, "HEAD", apiPath, "", header);
}

// *****************************************************************************

int IotApi::apiPost(std::string& response, const std::string& apiPath, const std::string& body, const std::map<std::string, std::string>& header)
{
    std::map<std::string, std::string> responseHeader;
    return apiRequest(response, responseHeader, "POST", apiPath, body, header);
}

// *****************************************************************************

int IotApi::apiPut(std::string& response, const std::string& apiPath, const std::string& body, const std::map<std::string, std::string>& header)
{
    std::map<std::string, std::string> responseHeader;
    return apiRequest(response, responseHeader, "PUT", apiPath, body, header);
}

// *****************************************************************************

// *****************************************************************************
// Firmware
// *****************************************************************************

std::string IotApi::getFirmwareHttpEtag()
{
    return _store.getString(_nvram_firmware_etag_key, "");
}

std::string IotApi::getFirmwareHttpDate()
{
    return _store.getString(_nvram_firmware_date_key, "");
}

// *****************************************************************************

IotResult IotApi::updateFirmware(const std::string& apiPath, const std::map<std::string, std::string>& header)
{
    // conditional request: only download what we do not already run
    std::map<std::string, std::string> conditional = {
        { "If-None-Match", getFirmwareHttpEtag() },
        { "If-Modified-Since", getFirmwareHttpDate() },
    };
    for (auto const& kv : header) { conditional[kv.first] = kv.second; }

    // HEAD to check whether an update is available. apiRequest() may replace
    // the token on a 401 retry, so the OTA headers are built after this call.
    std::string response;
    std::map<std::string, std::string> responseHeader;
    int httpStatusCode = apiRequest(response, responseHeader, "HEAD", apiPath, "", conditional);

    // return if no update available: 304 -> up to date (isNotModified), any
    // other non-2xx -> pass the transport/HTTP status through as an error
    if (httpStatusCode != HTTP_CODE_OK)
    {
        log_i("No firmware update available status=%d", httpStatusCode);
        return IotResult(httpStatusCode);
    }

    // an OTA download wants the cache validators and the token, but none of the
    // JSON content negotiation an API call gets
    std::map<std::string, std::string> otaHeader = _mergeHeaders(
        { { "Authorization", _tokens.deviceToken() } }, conditional);

    std::map<std::string, std::string> hh;
    for (auto const& kv : otaHeader)
    {
        if (!kv.second.empty())
        {
            hh[kv.first] = kv.second;
        }
    }

    std::string url = getApiUrlForPath(apiPath);
    // ota.setTimeout(10000); is the default
    std::string newEtag;
    std::string newDate;
    bool success = ota.updateFirmwareFromUrl(newEtag, newDate, url.c_str(), &hh);

    if (success)
    {
        _store.putString(_nvram_firmware_etag_key, newEtag);
        _store.putString(_nvram_firmware_date_key, newDate);
        log_i("Firmware update successful");
        return IotResult(HTTP_CODE_OK);
    }
    // an update was available (HEAD 200) but the download or flash did not
    // complete - report a distinct, non-transient failure
    log_e("Firmware update failed");
    return IotResult(IotResult::STATUS_UPDATE_FAILED);
}

// *****************************************************************************
