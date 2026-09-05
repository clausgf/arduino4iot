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
    // one NVS handle for the whole wakeup cycle instead of an open/close pair
    // per value, and every read guarded against the unfilterable "NOT_FOUND"
    // error Preferences logs for absent keys
    _store.begin();
    _setApiUrl(String(_store.getString(_nvram_api_url_key, "").c_str()));
    _projectName = _store.getString(_nvram_project_key, "").c_str();
    _caCertPem = _store.getString(_nvram_ca_cert_key, "").c_str();
    _clientCertPem = _store.getString(_nvram_client_cert_key, "").c_str();
    _clientKeyPem = _store.getString(_nvram_client_key_key, "").c_str();
    uint8_t tlsMode = (uint8_t)_store.getInt(_nvram_tls_mode_key, (int64_t)IotTlsMode::None);

    // the credentials load themselves from the same store
    _tokens.begin(_store);

    // apply TLS after the base URL is known (the secure client is created lazily
    // from the https:// URL)
    _applyTls((IotTlsMode)tlsMode);
}

void IotApi::_seedFromConfig(Preferences& preferences, const IotSeedConfig& cfg, bool force)
{
    // seeding still goes through the shared Preferences handle opened by
    // Iot::seedCredentials(); it runs before begin(), i.e. before _store exists.
    // Moving the seed path onto IotStore belongs with the config rework.
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
            if (!_caCertPem.isEmpty())
            {
                _applyCACert(_caCertPem.c_str());
            }
            if (!_clientCertPem.isEmpty() && !_clientKeyPem.isEmpty())
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
        if (_baseUrl.startsWith("https://"))
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

void IotApi::_setApiUrl(const String& apiBaseurl)
{
    _baseUrl = apiBaseurl;
    if (!_baseUrl.isEmpty() && !_baseUrl.endsWith("/"))
    {
        _baseUrl += "/";
    }
}

void IotApi::setDeviceName(const String& device)
{
    _deviceName = device;
}

void IotApi::setApiHeader(const std::map<String, String>& header)
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

void IotApi::setDeviceToken(const String& deviceToken, time_t expiresAt)
{
    _tokens.setDeviceToken(std::string(deviceToken.c_str()), (int64_t)expiresAt);
}

void IotApi::clearDeviceToken()
{
    _tokens.clearDeviceToken();
}

// *****************************************************************************

IotResult IotApi::updateProvisioning(const String& apiPath)
{
    // whether the current token still does the job is a decision, not I/O:
    // IotTokenManager::needsNewToken() (unit tested in test/test_token)
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

IotResult IotApi::_requestDeviceToken(const String& apiPath)
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
    String request;
    serializeJson(requestDoc, request);

    // the unauthenticated single-shot path: /provision authenticates with the
    // provisioning token in the body, and a 401 from it must be reported rather
    // than trigger another provisioning attempt
    String response;
    std::map<String, String> responseHeader;
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

String IotApi::getApiUrlForPath(const String& apiPath)
{
    std::string url = iot_text::buildApiUrl(
        _baseUrl.c_str(), apiPath.c_str(), _projectName.c_str(), _deviceName.c_str(),
        iot.getBoardId().c_str());
    return String(url.c_str());
}

// *****************************************************************************

std::map<String, String> IotApi::_mergeHeaders(std::map<String, String> base,
    const std::map<String, String>& requestHeader) const
{
    for (auto const& kv : _defaultRequestHeader) { base[kv.first] = kv.second; }
    for (auto const& kv : requestHeader) { base[kv.first] = kv.second; }
    return base;
}

std::map<String, String> IotApi::_requestHeaders(const std::map<String, String>& requestHeader,
    bool authenticated) const
{
    std::map<String, String> base = {
        { "Accept", "application/json" },
        { "Content-Type", "application/json" },
    };
    if (authenticated)
    {
        base["Authorization"] = String(_tokens.deviceToken().c_str());
    }
    return _mergeHeaders(std::move(base), requestHeader);
}

// *****************************************************************************

int IotApi::_performRequest(String& oResponse, std::map<String, String>& oResponseHeader,
    const char * requestType, const String& apiPath, const String& requestBody,
    const std::map<String, String>& requestHeader,
    const std::vector<String>& collectResponseHeaderKeys)
{
    String url = getApiUrlForPath(apiPath);
    log_i("HTTP %s url=%s", requestType, url.c_str());

    // prepare HTTP request; requestHeader is already the fully merged set
    _warnIfTlsTrustMissing();
    _getHttpClient().begin(*(_getWiFiClientPtr()), url);
    for (auto const& kv : requestHeader)
    {
        if (!kv.second.isEmpty())
        {
            log_d("  HTTP header: %s=%s", kv.first.c_str(), kv.second.c_str());
            _getHttpClient().addHeader(kv.first.c_str(), kv.second.c_str());
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
            oResponseHeader[key] = _getHttpClient().header(key.c_str());
        }
    }
    if ((strcasecmp("HEAD", requestType) != 0) && httpStatusCode != HTTP_CODE_NOT_MODIFIED)
    {
        oResponse = _getHttpClient().getString();
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

int IotApi::apiRequest(String& oResponse, std::map<String, String>& oResponseHeader,
    const char * requestType, const String& apiPath, const String& requestBody,
    const std::map<String, String>& requestHeader,
    const std::vector<String>& collectResponseHeaderKeys)
{
    // The authentication policy on top of the transport: attach the device
    // token, and on a rejection get a new one and try once more. _performRequest()
    // itself stays unaware of all this, so there is no way for the retry to
    // re-enter the request path (which is what _inProvisioning used to guard).
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

int IotApi::apiGet(String& response, const String& apiPath, const String& body, const std::map<String, String>& header)
{
    std::map<String, String> responseHeader;
    return apiRequest(response, responseHeader, "GET", apiPath, body, header);
}

int IotApi::apiGet(String& response, std::map<String, String>& oResponseHeader, const String& apiPath,
    const std::vector<String>& collectResponseHeaderKeys, const String& body, const std::map<String, String>& header)
{
    return apiRequest(response, oResponseHeader, "GET", apiPath, body, header, collectResponseHeaderKeys);
}

// *****************************************************************************

int IotApi::apiHead(const String& apiPath, const std::map<String, String>& header)
{
    String response = "";
    std::map<String, String> responseHeader;
    return apiRequest(response, responseHeader, "HEAD", apiPath, "", header);
}

// *****************************************************************************

int IotApi::apiPost(String& response, const String& apiPath, const String& body, const std::map<String, String>& header)
{
    std::map<String, String> responseHeader;
    return apiRequest(response, responseHeader, "POST", apiPath, body, header);
}

// *****************************************************************************

int IotApi::apiPut(String& response, const String& apiPath, const String& body, const std::map<String, String>& header)
{
    std::map<String, String> responseHeader;
    return apiRequest(response, responseHeader, "PUT", apiPath, body, header);
}

// *****************************************************************************

IotResult IotApi::apiForward(String& oResponse, const String& forwardingName, const String& remainingPath,
    const String& body, const std::map<String, String>& headers)
{
    String apiPath = "forward/{project}/{device}/" + forwardingName;
    if (!remainingPath.isEmpty())
    {
        apiPath += remainingPath.startsWith("/") ? remainingPath : "/" + remainingPath;
    }
    return apiGet(oResponse, apiPath, body, headers);
}

// *****************************************************************************

IotResult IotApi::uploadFile(const String& filename, const String& content, const String& contentType)
{
    String response;
    // apiPut returns a raw status code; IotResult classifies it (negative ->
    // TransportError, 2xx -> Ok, else HttpError, e.g. 413 for an oversized file)
    return apiPut(response, "file/{project}/{device}/" + filename, content,
        {{"Content-Type", contentType}});
}

// *****************************************************************************

IotResult IotApi::apiCheckForUpdate(const String& apiPath, const char *nvram_etag_key, const char *nvram_date_key)
{
    String etag = _store.getString(nvram_etag_key, "").c_str();
    String date = _store.getString(nvram_date_key, "").c_str();

    String response = "";
    std::map<String, String> responseHeader;
    // Ok (2xx) means an update is available, isNotModified() (304) means the
    // resource is unchanged; both are non-errors
    return apiRequest(response, responseHeader, "HEAD", apiPath, "", {
        {"If-None-Match", etag},
        {"If-Modified-Since", date}
    });
}


// *****************************************************************************
// Firmware
// *****************************************************************************

String IotApi::getFirmwareHttpEtag()
{
    return String(_store.getString(_nvram_firmware_etag_key, "").c_str());
}

String IotApi::getFirmwareHttpDate()
{
    return String(_store.getString(_nvram_firmware_date_key, "").c_str());
}

// *****************************************************************************

IotResult IotApi::updateFirmware(const String& apiPath, const std::map<String, String>& header)
{
    // conditional request: only download what we do not already run
    std::map<String, String> conditional = {
        { "If-None-Match", getFirmwareHttpEtag() },
        { "If-Modified-Since", getFirmwareHttpDate() },
    };
    for (auto const& kv : header) { conditional[kv.first] = kv.second; }

    // HEAD to check whether an update is available. apiRequest() adds the
    // Authorization header and may replace the token on a 401 retry - which is
    // why the OTA headers below are built *after* this call rather than patched
    // up afterwards as they were up to 3.x.
    String response = "";
    std::map<String, String> responseHeader;
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
    std::map<String, String> otaHeader = _mergeHeaders(
        { { "Authorization", String(_tokens.deviceToken().c_str()) } }, conditional);

    std::map<std::string, std::string> hh;
    for (auto const& kv : otaHeader)
    {
        if (!kv.second.isEmpty())
        {
            hh[kv.first.c_str()] = kv.second.c_str();
        }
    }

    String url = getApiUrlForPath(apiPath);
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
