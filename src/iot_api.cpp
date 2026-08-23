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

IotApi::IotApi()
{
    _baseUrl = "";
    _defaultRequestHeader = {};
    _projectName = "";
    _deviceName = "";
    _provisioningToken = "";
    _deviceToken = "";
    _deviceTokenExpiresAt = 0;
    _deviceTokenExpiryMargin_s = 3600;
    _inProvisioning = false;

    _wifiClientSecurePtr = nullptr;
    _wifiClientPtr = nullptr;
    _httpClientPtr = nullptr;

    _tlsServerTrustConfigured = false;
    _tlsTrustWarningLogged = false;
}

void IotApi::begin()
{
    // load the seeded API configuration and the runtime tokens from NVS
    Preferences preferences;
    preferences.begin("iot", true);
    _setApiUrl(nvramGetString(preferences, _nvram_api_url_key, ""));
    _projectName = nvramGetString(preferences, _nvram_project_key, "");
    _provisioningToken = nvramGetString(preferences, _nvram_provisioning_token_key, "");
    _deviceToken = nvramGetString(preferences, _nvram_device_token_key, "");
    _deviceTokenExpiresAt = nvramGetLong64(preferences, _nvram_device_token_expiry_key, 0);
    uint8_t tlsMode = nvramGetUChar(preferences, _nvram_tls_mode_key, (uint8_t)IotTlsMode::None);
    _caCertPem = nvramGetString(preferences, _nvram_ca_cert_key, "");
    _clientCertPem = nvramGetString(preferences, _nvram_client_cert_key, "");
    _clientKeyPem = nvramGetString(preferences, _nvram_client_key_key, "");
    preferences.end();

    // apply TLS after the base URL is known (the secure client is created lazily
    // from the https:// URL)
    _applyTls((IotTlsMode)tlsMode);
}

void IotApi::_seedFromConfig(Preferences& preferences, const IotSeedConfig& cfg, bool force)
{
    String oldApiUrl = nvramGetString(preferences, _nvram_api_url_key, "");
    String oldProjectName = nvramGetString(preferences, _nvram_project_key, "");
    String oldProvisioningToken = nvramGetString(preferences, _nvram_provisioning_token_key, "");

    iotSeedString(preferences, _nvram_api_url_key, cfg.apiUrl, force);
    iotSeedString(preferences, _nvram_project_key, cfg.projectName, force);
    iotSeedString(preferences, _nvram_provisioning_token_key, cfg.provisioningToken, force);
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
        nvramGetString(preferences, _nvram_provisioning_token_key, "") != oldProvisioningToken;
    if (identityChanged)
    {
        preferences.remove(_nvram_device_token_key);
        preferences.remove(_nvram_device_token_expiry_key);
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
    if (_deviceToken == deviceToken && _deviceTokenExpiresAt == (int64_t)expiresAt)
    {
        return;
    }
    _deviceToken = deviceToken;
    _deviceTokenExpiresAt = expiresAt;

    Preferences preferences;
    preferences.begin("iot", false);
    preferences.putString(_nvram_device_token_key, _deviceToken.c_str());
    preferences.putLong64(_nvram_device_token_expiry_key, _deviceTokenExpiresAt);
    preferences.end();
}

void IotApi::clearDeviceToken()
{
    setDeviceToken("", 0);
}

// *****************************************************************************

IotResult IotApi::updateProvisioning(const String& apiPath)
{
    if (!_deviceToken.isEmpty())
    {
        if (_deviceTokenExpiresAt <= 0)
        {
            // token lifetime unknown (e.g. provisioned by an older library version)
            log_i("updateProvisioning: already provisioned, token lifetime unknown");
            return IotResult(HTTP_CODE_OK);
        }
        int64_t now = (int64_t)time(nullptr);
        if (!iot.isTimePlausible() || now + _deviceTokenExpiryMargin_s < _deviceTokenExpiresAt)
        {
            log_i("updateProvisioning: already provisioned, token valid for %lld s",
                _deviceTokenExpiresAt - now);
            return IotResult(HTTP_CODE_OK);
        }
        log_i("updateProvisioning: device token expired or expiring soon, re-provisioning");
    }

    if (_provisioningToken.isEmpty())
    {
        log_e("updateProvisioning: no provisioning token available");
        return IotResult(IotResult::STATUS_NO_PROVISIONING_TOKEN);
    }

    // execute HTTP POST request
    JsonDocument requestDoc;
    requestDoc["projectName"] = _projectName;
    requestDoc["deviceName"] = _deviceName;
    requestDoc["provisioningToken"] = _provisioningToken;
    String request;
    serializeJson(requestDoc, request);

    String response;
    _inProvisioning = true;
    int httpStatusCode = apiPost(response, apiPath, request, {{"Authorization", ""}});
    _inProvisioning = false;
    if (httpStatusCode != HTTP_CODE_OK || response.isEmpty())
    {
        log_i("updateProvisioning: status=%d or no response", httpStatusCode);
        // a 2xx with an empty body is not a usable success - flag it as malformed;
        // otherwise pass the transport/HTTP status through unchanged
        if (httpStatusCode >= 200 && httpStatusCode < 300)
        {
            return IotResult(IotResult::STATUS_MALFORMED_RESPONSE);
        }
        return IotResult(httpStatusCode);
    }

    // parse response
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, response);
    if (error)
    {
        log_i("updateProvisioning: JSON deserialization failed: %s", error.c_str());
        return IotResult(IotResult::STATUS_MALFORMED_RESPONSE);
    }
    if (!doc["accessToken"].is<const char*>())
    {
        log_i("updateProvisioning: no accessToken");
        return IotResult(IotResult::STATUS_MALFORMED_RESPONSE);
    }
    if (!doc["tokenType"].is<const char*>())
    {
        log_i("updateProvisioning: no tokenType");
        return IotResult(IotResult::STATUS_MALFORMED_RESPONSE);
    }

    // determine token expiry from expiresIn (seconds); requires plausible system time
    time_t expiresAt = 0;
    if (doc["expiresIn"].is<long>() && iot.isTimePlausible())
    {
        expiresAt = time(nullptr) + doc["expiresIn"].as<long>();
    }

    String deviceToken = doc["tokenType"].as<String>() + " " + doc["accessToken"].as<String>();
    setDeviceToken(deviceToken, expiresAt);
    if (expiresAt > 0)
    {
        log_i("updateProvisioning: new device token for api access, expires at %s",
            iot.getTimeIso(expiresAt).c_str());
    } else {
        log_i("updateProvisioning: new device token for api access");
    }
    return IotResult(httpStatusCode);
}


// *****************************************************************************
// HTTP requests
// *****************************************************************************

String IotApi::getApiUrlForPath(const String& apiPath)
{
    std::string url = iot_text::buildApiUrl(
        _baseUrl.c_str(), apiPath.c_str(), _projectName.c_str(), _deviceName.c_str());
    return String(url.c_str());
}

// *****************************************************************************

void IotApi::_addRequestHeader(HTTPClient& http, const std::map<String, String> &header)
{
    // start with default header, then merge base header, then merge request header
    std::map<String, String> h = {
        { "Accept", "application/json" },
        { "Content-Type", "application/json" },
        { "Authorization", _deviceToken }
    };
    for (auto const& kv : _defaultRequestHeader) { h[kv.first] = kv.second; }
    for (auto const& kv : header) { h[kv.first] = kv.second; }

    // write headers to HTTPClient
    for (auto const& kv : h)
    {
        if ( !kv.second.isEmpty() )
        {
            log_d("  HTTP header: %s=%s", kv.first.c_str(), kv.second.c_str());
            http.addHeader(kv.first.c_str(), kv.second.c_str());
        }
    }
}

// *****************************************************************************

int IotApi::_performRequest(String& oResponse, std::map<String, String>& oResponseHeader,
    const char * requestType, const String& apiPath, const String& requestBody,
    const std::map<String, String>& requestHeader,
    const std::vector<String>& collectResponseHeaderKeys)
{
    String url = getApiUrlForPath(apiPath);
    log_i("HTTP %s url=%s", requestType, url.c_str());

    // prepare HTTP request
    _warnIfTlsTrustMissing();
    _getHttpClient().begin(*(_getWiFiClientPtr()), url);
    _addRequestHeader(_getHttpClient(), requestHeader);
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
    int httpStatusCode = _performRequest(oResponse, oResponseHeader,
        requestType, apiPath, requestBody, requestHeader, collectResponseHeaderKeys);

    // on 401, re-provision and retry the request once
    if (httpStatusCode == HTTP_CODE_UNAUTHORIZED && !_inProvisioning)
    {
        clearDeviceToken();
        if (!_provisioningToken.isEmpty() && updateProvisioningOk())
        {
            log_i("HTTP %s url=%s retrying after re-provisioning", requestType, apiPath.c_str());
            oResponse = "";
            oResponseHeader.clear();
            httpStatusCode = _performRequest(oResponse, oResponseHeader,
                requestType, apiPath, requestBody, requestHeader, collectResponseHeaderKeys);
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
    // get etag and date from preferences
    Preferences preferences;
    preferences.begin("iot", true);
    String etag = nvramGetString(preferences, nvram_etag_key, "");
    String date = nvramGetString(preferences, nvram_date_key, "");
    preferences.end();

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
    Preferences preferences;
    preferences.begin("iot", true);
    String etag = nvramGetString(preferences, _nvram_firmware_etag_key, "");
    preferences.end();
    return etag;
}

String IotApi::getFirmwareHttpDate()
{
    Preferences preferences;
    preferences.begin("iot", true);
    String date = nvramGetString(preferences, _nvram_firmware_date_key, "");
    preferences.end();
    return date;
}

// *****************************************************************************

IotResult IotApi::updateFirmware(const String& apiPath, const std::map<String, String>& header)
{
    // get etag and date from preferences
    Preferences preferences;
    preferences.begin("iot", true);
    String etag = nvramGetString(preferences, _nvram_firmware_etag_key, "");
    String date = nvramGetString(preferences, _nvram_firmware_date_key, "");
    preferences.end();

    // prepare header
    std::map<String, String> h = {
        { "If-None-Match", etag },
        { "If-Modified-Since", date },
        { "Authorization", _deviceToken }
    };
    for (auto const& kv : _defaultRequestHeader) { h[kv.first] = kv.second; }
    for (auto const& kv : header) { h[kv.first] = kv.second; }

    // HEAD request to check if update is available; a 401 triggers
    // re-provisioning in apiRequest(), so refresh the Authorization header afterwards
    String response = "";
    std::map<String, String> responseHeader;
    int httpStatusCode = apiRequest(response, responseHeader, "HEAD", apiPath, "", h);
    h["Authorization"] = _deviceToken;

    // return if no update available: 304 -> up to date (isNotModified), any
    // other non-2xx -> pass the transport/HTTP status through as an error
    if (httpStatusCode != HTTP_CODE_OK)
    {
        log_i("No firmware update available status=%d", httpStatusCode);
        return IotResult(httpStatusCode);
    }

    std::map<std::string, std::string> hh;
    for (auto const& kv : h)
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
        Preferences preferences;
        preferences.begin("iot", false);
        preferences.putString(_nvram_firmware_etag_key, newEtag.c_str());
        preferences.putString(_nvram_firmware_date_key, newDate.c_str());
        preferences.end();
        log_i("Firmware update successful");
        return IotResult(HTTP_CODE_OK);
    }
    // an update was available (HEAD 200) but the download or flash did not
    // complete - report a distinct, non-transient failure
    log_e("Firmware update failed");
    return IotResult(IotResult::STATUS_UPDATE_FAILED);
}

// *****************************************************************************
