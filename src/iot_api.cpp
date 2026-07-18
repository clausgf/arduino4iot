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
}

void IotApi::begin()
{
    // read preferences
    Preferences preferences;
    preferences.begin("iot", true);
    _provisioningToken = preferences.getString(_nvram_provisioning_token_key, "");
    _deviceToken = preferences.getString(_nvram_device_token_key, "");
    _deviceTokenExpiresAt = preferences.getLong64(_nvram_device_token_expiry_key, 0);
    preferences.end();
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

void IotApi::setApiUrl(const String& apiBaseurl)
{
    _baseUrl = apiBaseurl;
    if (!_baseUrl.endsWith("/"))
    {
        _baseUrl += "/";
    }
}

void IotApi::setProjectName(const String& project)
{
    _projectName = project;
}

void IotApi::setDeviceName(const String& device)
{
    _deviceName = device;
}

void IotApi::setApiHeader(const std::map<String, String>& header)
{
    _defaultRequestHeader = header;
}

void IotApi::setCACert(const char *server_certificate)
{
    if (_isWiFiClientSecure())
    {
        _wifiClientSecurePtr->setCACert(server_certificate);
        ota.setServerCert(server_certificate, false);
    } else {
        log_e("setCACert: WiFiClientSecure not used");
    }
}

void IotApi::setClientCertificateAndKey(const char *client_certificate, const char *client_key)
{
    if (_isWiFiClientSecure())
    {
        _wifiClientSecurePtr->setCertificate(client_certificate);
        _wifiClientSecurePtr->setPrivateKey(client_key);
        ota.setClientCert(client_certificate, client_key, nullptr);
    } else {
        log_e("setClientCertificateAndKey: WiFiClientSecure not used");
    }
}

void IotApi::setCertInsecure()
{
    if (_isWiFiClientSecure())
    {
        _wifiClientSecurePtr->setInsecure();
    } else {
        log_e("setCertInsecure: WiFiClientSecure not used");
    }
    ota.setServerCert(nullptr, true);
    ota.setClientCert(nullptr, nullptr, nullptr);
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

void IotApi::setProvisioningToken(const String& provisioningToken)
{
    if (_provisioningToken == provisioningToken)
    {
        return;
    }
    _provisioningToken = provisioningToken;

    Preferences preferences;
    preferences.begin("iot", false);
    preferences.putString(_nvram_provisioning_token_key, _provisioningToken.c_str());
    preferences.end();
}

bool IotApi::setProvisioningTokenIfEmpty(const String& provisioningToken)
{
    if (!_provisioningToken.isEmpty())
    {
        return false;
    }
    setProvisioningToken(provisioningToken);
    return true;
}

void IotApi::clearProvisioningToken()
{
    setProvisioningToken("");
}

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

bool IotApi::updateProvisioningOk(const String& apiPath)
{
    if (!_deviceToken.isEmpty())
    {
        if (_deviceTokenExpiresAt <= 0)
        {
            // token lifetime unknown (e.g. provisioned by an older library version)
            log_i("updateProvisioningOk: already provisioned, token lifetime unknown");
            return true;
        }
        int64_t now = (int64_t)time(nullptr);
        if (!iot.isTimePlausible() || now + _deviceTokenExpiryMargin_s < _deviceTokenExpiresAt)
        {
            log_i("updateProvisioningOk: already provisioned, token valid for %lld s",
                _deviceTokenExpiresAt - now);
            return true;
        }
        log_i("updateProvisioningOk: device token expired or expiring soon, re-provisioning");
    }

    if (_provisioningToken.isEmpty())
    {
        log_e("updateProvisioningOk: no provisioning token available");
        return false;
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
        log_i("updateProvisioningOk: status=%d or no response", httpStatusCode);
        return false;
    }

    // parse response
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, response);
    if (error)
    {
        log_i("updateProvisioningOk: JSON deserialization failed: %s", error.c_str());
        return false;
    }
    if (!doc["accessToken"].is<const char*>())
    {
        log_i("updateProvisioningOk: no accessToken");
        return false;
    }
    if (!doc["tokenType"].is<const char*>())
    {
        log_i("updateProvisioningOk: no tokenType");
        return false;
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
        log_i("updateProvisioningOk: new device token for api access, expires at %s",
            iot.getTimeIso(expiresAt).c_str());
    } else {
        log_i("updateProvisioningOk: new device token for api access");
    }
    return true;
}


// *****************************************************************************
// HTTP requests
// *****************************************************************************

String IotApi::_replaceVars(String str)
{
    String ret = str;
    ret.replace("{device}", _deviceName);
    ret.replace("{project}", _projectName);
    return ret;
}

String IotApi::getApiUrlForPath(const String& apiPath)
{
    String path = apiPath;
    if (path.startsWith("/"))
    {
        path = path.substring(1);
    }
    return _replaceVars(_baseUrl + path);
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
        log_e("HTTP %s url=%s -> status=%d error=%s",
            requestType, url.c_str(), httpStatusCode, _getHttpClient().errorToString(httpStatusCode).c_str());
    } else if (httpStatusCode == HTTP_CODE_UNAUTHORIZED) {
        log_e("HTTP %s url=%s -> status=%d UNAUTHORIZED - device token invalid or expired",
            requestType, url.c_str(), httpStatusCode);
    } else if (httpStatusCode == HTTP_CODE_FORBIDDEN) {
        // 403 signals a configuration problem (project inactive, device inactive
        // or not approved) - re-provisioning would not help, keep the token
        log_e("HTTP %s url=%s -> status=%d FORBIDDEN - check project/device configuration on the server",
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

int IotApi::apiForward(String& oResponse, const String& forwardingName, const String& remainingPath,
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

bool IotApi::uploadFile(const String& filename, const String& content, const String& contentType)
{
    String response;
    int httpStatusCode = apiPut(response, "file/{project}/{device}/" + filename, content,
        {{"Content-Type", contentType}});
    return (httpStatusCode >= 200) && (httpStatusCode < 300);
}

// *****************************************************************************

bool IotApi::apiCheckForUpdate(const String& apiPath, const char *nvram_etag_key, const char *nvram_date_key)
{
    // get etag and date from preferences
    Preferences preferences;
    preferences.begin("iot", true);
    String etag = preferences.getString(nvram_etag_key, "");
    String date = preferences.getString(nvram_date_key, "");
    preferences.end();

    String response = "";
    std::map<String, String> responseHeader;
    int httpStatusCode = apiRequest(response, responseHeader, "HEAD", apiPath, "", {
        {"If-None-Match", etag},
        {"If-Modified-Since", date}
    });

    return (httpStatusCode >= 200) && (httpStatusCode < 300);
}


// *****************************************************************************
// Firmware
// *****************************************************************************

String IotApi::getFirmwareHttpEtag()
{
    Preferences preferences;
    preferences.begin("iot", true);
    String etag = preferences.getString(_nvram_firmware_etag_key, "");
    preferences.end();
    return etag;
}

String IotApi::getFirmwareHttpDate()
{
    Preferences preferences;
    preferences.begin("iot", true);
    String date = preferences.getString(_nvram_firmware_date_key, "");
    preferences.end();
    return date;
}

// *****************************************************************************

bool IotApi::updateFirmware(const String& apiPath, const std::map<String, String>& header)
{
    // get etag and date from preferences
    Preferences preferences;
    preferences.begin("iot", true);
    String etag = preferences.getString(_nvram_firmware_etag_key, "");
    String date = preferences.getString(_nvram_firmware_date_key, "");
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

    // return if no update available
    if (httpStatusCode != HTTP_CODE_OK)
    {
        log_i("No firmware update available status=%d", httpStatusCode);
        return false;
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
    } else {
        log_e("Firmware update failed");
    }
    return success;
}

// *****************************************************************************
