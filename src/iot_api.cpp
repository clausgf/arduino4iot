/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#include "iot_api.h"

#include <esp_ota_ops.h>
#include <WiFi.h>
#include <Preferences.h>
#include <HttpsOTAUpdate.h>
#include <ArduinoJson.h>

#include "iot.h"

// *****************************************************************************

IotApi api;

// *****************************************************************************

IotApi::IotApi()
{
    _baseUrl = "";
    _defaultRequestHeader = {};
    _projectName = "";
    _deviceName = "";
    _provisioningToken = "";
    _deviceToken = "";

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
    // log_d("provisioningToken=%s", _provisioningToken.c_str());
    _deviceToken = preferences.getString(_nvram_device_token_key, "");
    // log_d("deviceToken=%s", _deviceToken.c_str());
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

void IotApi::setApiUrl(String apiBaseurl)
{
    _baseUrl = apiBaseurl;
    if (!_baseUrl.endsWith("/"))
    {
        _baseUrl += "/";
    }
}

void IotApi::setProjectName(String project)
{
    _projectName = project;
}

void IotApi::setDeviceName(String device)
{
    _deviceName = device;
}

void IotApi::setApiHeader(std::map<String, String> header)
{
    _defaultRequestHeader = header;
}

void IotApi::setCACert(const char *server_certificate)
{
    if (_isWiFiClientSecure())
    {
        _wifiClientSecurePtr->setCACert(server_certificate);
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
    } else {
        log_e("setCACert: WiFiClientSecure not used");
    }
}

void IotApi::setCertInsecure()
{
    if (_isWiFiClientSecure())
    {
        _wifiClientSecurePtr->setInsecure();
    } else {
        log_e("setCACert: WiFiClientSecure not used");
    }
}


// *****************************************************************************
// Provisioning
// *****************************************************************************

void IotApi::setProvisioningToken(String provisioningToken)
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

bool IotApi::setProvisioningTokenIfEmpty(String provisioningToken)
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

void IotApi::setDeviceToken(String deviceToken)
{
    if (_deviceToken == deviceToken)
    {
        return;
    }
    _deviceToken = deviceToken;

    Preferences preferences;
    preferences.begin("iot", false);
    preferences.putString(_nvram_device_token_key, _deviceToken.c_str());
    preferences.end();
}

void IotApi::clearDeviceToken()
{
    setDeviceToken("");
}

// *****************************************************************************

bool IotApi::updateProvisioning(String apiPath)
{
    if (!_deviceToken.isEmpty())
    {
        log_i("updateProvisioning: already provisioned");
        return false;
    }

    // execute HTTP POST request
    String request = 
            "{\"projectName\":\"" + _projectName + "\","
            "\"deviceName\":\"" + _deviceName + "\","
            "\"provisioningToken\":\"" + _provisioningToken + "\"}";
    String response;
    int httpStatusCode = apiPost(response, apiPath, request, {{"Authorization", ""}});
    if (response.isEmpty() || httpStatusCode < HTTP_CODE_OK || httpStatusCode >= HTTP_CODE_BAD_REQUEST)
    {
        log_i("updateProvisioning: status=%d or no response", httpStatusCode);
        return false;
    }

    // parse response
    DynamicJsonDocument doc(2048);
    DeserializationError error = deserializeJson(doc, response);
    if (error)
    {
        log_i("updateProvisioning: JSON deserialization failed: %s", error.c_str());
        return false;
    }
    if (!doc.containsKey("accessToken"))
    {
        log_i("updateProvisioning: no accessToken");
        return false;
    }
    if (!doc.containsKey("tokenType"))
    {
        log_i("updateProvisioning: no tokenType");
        return false;
    }
    String deviceToken = doc["tokenType"].as<String>() + " " + doc["accessToken"].as<String>();
    setDeviceToken(deviceToken);
    log_i("updateProvisioning: new device token for api access");
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

String IotApi::getApiUrlForPath(String path)
{
    if (path.startsWith("/"))
    {
        path = path.substring(1);
    }
    return _replaceVars(_baseUrl + path);
}

// *****************************************************************************

void IotApi::_addRequestHeader(HTTPClient& http, std::map<String, String> &header)
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

bool IotApi::_apiCheckForUpdate(String url, const char *nvram_etag_key, const char *nvram_date_key)
{
    url = _replaceVars(url);
    // get etag and date from preferences
    Preferences preferences;
    preferences.begin("iot", true);
    String etag = preferences.getString(nvram_etag_key, "");
    String date = preferences.getString(nvram_date_key, "");
    preferences.end();

    log_i("HTTP HEAD url=%s etag=%s date=%s", url.c_str(), etag.c_str(), date.c_str());
    _getHttpClient().begin(*(_getWiFiClientPtr()), url);
    std::map<String, String> request_header = {
        {"If-None-Match", etag},
        {"If-Modified-Since", date}
    };
    _addRequestHeader(_getHttpClient(), request_header);
    int httpStatusCode = _getHttpClient().sendRequest("HEAD");

    if (httpStatusCode < 0)
    {
        log_e("HTTP HEAD -> status=%d error %s", 
            httpStatusCode, _getHttpClient().errorToString(httpStatusCode).c_str());
    } else {
        log_i("HTTP HEAD -> %d", httpStatusCode);
    }
    _getHttpClient().end();
    return (httpStatusCode >= 200) && (httpStatusCode < 300);
}

// *****************************************************************************

int IotApi::apiRequest(String& oResponse, std::map<String, String>& oResponseHeader, const char * requestType, String apiPath, String requestBody, std::map<String, String> requestHeader, const char* collectResponseHeaderKeys[], const size_t collectResponseHeaderKeysCount)
{
    String url = getApiUrlForPath(apiPath);
    log_i("HTTP %s url=%s", requestType, url.c_str());

    // prepare HTTP request
    _getHttpClient().begin(*(_getWiFiClientPtr()), url);
    _addRequestHeader(_getHttpClient(), requestHeader);
    if (collectResponseHeaderKeys != nullptr && collectResponseHeaderKeysCount > 0) {
        _getHttpClient().collectHeaders(collectResponseHeaderKeys, collectResponseHeaderKeysCount);
    }

    // execute HTTP request
    int httpStatusCode = _getHttpClient().sendRequest(requestType, (uint8_t*)requestBody.c_str(), requestBody.length());
    for (int i=0; i<collectResponseHeaderKeysCount; i++)
    {
        const char * key = collectResponseHeaderKeys[i];
        if (_getHttpClient().hasHeader(key))
        {
            oResponseHeader[key] = _getHttpClient().header(key);
        }
    }
    //log_i("  HTTP response status: %d", httpStatusCode);
    //log_i("  HTTP response size: %d", getHttpClient().getSize());
    oResponse = httpStatusCode != 304 ? _getHttpClient().getString() : "";
    //log_d("  HTTP response length: %d", oResponse.length());

    // evaluate HTTP response
    if (httpStatusCode < 0)
    {
        log_e("HTTP %s url=%s -> status=%d error=%s", 
            requestType, url.c_str(), httpStatusCode, _getHttpClient().errorToString(httpStatusCode).c_str());
    } else if (httpStatusCode == 403) { // 403 FORBIDDEN
            log_e("HTTP %s url=%s -> status=%d FORBIDDEN - clearing api token to force provisioning",
                requestType, url.c_str(), httpStatusCode);
            clearDeviceToken();
    } else if (httpStatusCode < 200 || httpStatusCode >= 400) {
        log_e("HTTP %s url=%s -> status=%d requestBody=%s", 
            requestType, url.c_str(), httpStatusCode, requestBody.c_str());
    } else {
        log_i("HTTP %s url=%s -> status=%d", requestType, url.c_str(), httpStatusCode);
    }
    _getHttpClient().end();
    return httpStatusCode;
}

// *****************************************************************************

int IotApi::apiGet(String& response, String apiPath, String body, std::map<String, String> header)
{
    std::map<String, String> responseHeader;
    return apiRequest(response, responseHeader, "GET", apiPath, body, header);
}

// *****************************************************************************

int IotApi::apiHead(String apiPath, std::map<String, String> header)
{
    String response = "";
    std::map<String, String> responseHeader;
    String body = "";
    return apiRequest(response, responseHeader, "HEAD", apiPath, body, header);
}

// *****************************************************************************

int IotApi::apiPost(String& response, String apiPath, String body, std::map<String, String> header)
{
    std::map<String, String> responseHeader;
    return apiRequest(response, responseHeader, "POST", apiPath, body, header);
}

// *****************************************************************************

bool IotApi::apiCheckForUpdate(String apiPath, const char *nvram_etag_key, const char *nvram_date_key)
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
    return config.getConfigString(_nvram_firmware_etag_key, "");
}

String IotApi::getFirmwareHttpDate()
{
    return config.getConfigString(_nvram_firmware_date_key, "");
}

// *****************************************************************************

String updateFirmwareNewEtag = "";
String updateFirmwareNewDate = "";

void updateFirmwareHttpEvent(HttpEvent_t *event)
{
    switch(event->event_id) {
        case HTTP_EVENT_ERROR:
            Serial.println("Http Event Error");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            // Serial.println("Http Event On Connected");
            break;
        case HTTP_EVENT_HEADER_SENT:
            // Serial.println("Http Event Header Sent");
            break;
        case HTTP_EVENT_ON_HEADER:
            {
                String key = event->header_key;
                Serial.printf("Http Event On Header, key=%s, value=%s\n", key.c_str(), event->header_value);
                if (key.equalsIgnoreCase("etag"))
                {
                    updateFirmwareNewEtag = event->header_value;
                } else if (key.equalsIgnoreCase("last-modified"))
                {
                    updateFirmwareNewDate = event->header_value;
                }
            }
            break;
        case HTTP_EVENT_ON_DATA:
            break;
        case HTTP_EVENT_ON_FINISH:
            Serial.println("Http Event On Finish");
            break;
        case HTTP_EVENT_DISCONNECTED:
            Serial.println("Http Event Disconnected");
            break;
    }
}

// TODO: add support for HTTP instead of HTTPS
// TODO: add support for server certificate
// TODO: add support for skip_cert_common_name_check
// TODO: add Authentication header
bool IotApi::updateFirmware(String apiPath)
{
    String url = getApiUrlForPath(apiPath);
    bool updateAvailable = apiCheckForUpdate(url, _nvram_firmware_etag_key, _nvram_firmware_date_key);
    if (!updateAvailable)
    {
        log_i("No firmware update available");
        return false;
    }

    log_i("Updating firmware from %s", url.c_str());
    updateFirmwareNewEtag = "";
    updateFirmwareNewDate = "";
    HttpsOTA.onHttpEvent(updateFirmwareHttpEvent); // HttpsOTA.begin(url, server_certificate); 
    HttpsOTA.begin(url.c_str(), NULL, true);
    while (1)
    {
        HttpsOTAStatus_t ota_status = HttpsOTA.status();
        if (ota_status == HTTPS_OTA_SUCCESS)
        {
            log_i("Firmware update successful");
            break;
        } else if (ota_status == HTTPS_OTA_FAIL)
        {
            log_i("Firmware update failed");
            return false;
        }
        log_d("... updating ...");
        delay(1000);
    }
    Preferences preferences;
    preferences.begin("iot", false);
    preferences.putString(_nvram_firmware_etag_key, updateFirmwareNewEtag);
    preferences.putString(_nvram_firmware_date_key, updateFirmwareNewDate);
    preferences.end();
    return true;
}

// *****************************************************************************