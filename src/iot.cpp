/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#include "iot.h"

#include <WiFi.h>
#include <nvs_flash.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <esp_ota_ops.h>
#include <HttpsOTAUpdate.h>
#include <ArduinoJson.h>

// *****************************************************************************

IoT iot;
static const char *TAG = "iot";

uint32_t IoT::bootCount = 0;
int64_t IoT::activeDuration_ms = 0;

// *****************************************************************************

IoT::IoT()
{
    lastSleepDuration_ms = millis() - bootTimestamp_ms;
    bootTimestamp_ms = millis();
    wakeupCause = esp_sleep_get_wakeup_cause();
    bootCount++;
    // activeDuration_ms is set on shutdown

    // initialize NVRAM
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK)
    {
        Serial.printf("nvs_flash_init failed: %s\n", esp_err_to_name(err));
        return;
    }

    // read preferences
    Preferences preferences;
    preferences.begin("iot", true);
    _apiHost = preferences.getString(_nvram_api_host_key, "");
    _apiBaseUrl = preferences.getString(_nvram_api_url_key, "");
    _apiHostHeader = preferences.getString(_nvram_api_host_header_key, "");
    _apiProject = preferences.getString(_nvram_project_key, "");
    _apiProvisioningToken = preferences.getString(_nvram_provisioning_token_key, "");
    _apiToken = preferences.getString(_nvram_api_token_key, "");
    preferences.end();

    // activate certificate bundle
    _wifiClient = WiFiClientSecure();
}

// *****************************************************************************

bool IoT::connectWifi(String ssid, String password, unsigned long timeout_ms)
{
    Serial.printf("Connecting to WiFi network ssid=%s timeout=%d\n", ssid.c_str(), timeout_ms);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(device_id().c_str());
    WiFi.begin(ssid.c_str(), password.c_str());

    // wait for connection
    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < timeout_ms)
    {
        delay(50);
    }
    // check for failed connection due to timeout
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("WiFi connection failed");
        return false;
    }

    Serial.printf("WiFi connected. IP address: %s\n", WiFi.localIP().toString().c_str());
    return true;
}

// *****************************************************************************

String IoT::device_id()
{
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);

    const int ID_MAXLEN = 17;
    char id_buf[ID_MAXLEN];
    snprintf(id_buf, ID_MAXLEN, "e32-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    return id_buf;
}

int IoT::rssi()
{
    return WiFi.RSSI();
}


// *****************************************************************************
// API configuration
// *****************************************************************************

String IoT::_replaceVars(String str)
{
    String ret = str;
    ret.replace("{device_id}", device_id());
    ret.replace("{api_url}", _apiBaseUrl);
    ret.replace("{project}", _apiProject);
    return ret;
}

void IoT::setApi(String apiHost, String apiBaseurl, String hostHeader)
{
    if (_apiHost != apiHost)
    {
        _apiHost = apiHost;
        setConfigString(_nvram_api_host_key, apiHost);
    }
    if (_apiBaseUrl != apiBaseurl)
    {
        _apiBaseUrl = apiBaseurl;
        if (!_apiBaseUrl.endsWith("/"))
        {
            _apiBaseUrl += "/";
        }
        setConfigString(_nvram_api_url_key, _apiBaseUrl);
    }

    if (_apiHostHeader != hostHeader)
    {
        _apiHostHeader = hostHeader;
        setConfigString(_nvram_api_host_header_key, _apiHostHeader);
    }
}

String IoT::getApiUrlForPath(String path)
{
    if (path.startsWith("/"))
    {
        path = path.substring(1);
    }
    return _replaceVars(_apiBaseUrl + path);
}

void IoT::setProject(String project)
{
    if (_apiProject == project)
    {
        return;
    }
    _apiProject = project;
    setConfigString(_nvram_project_key, project);
}

void IoT::setProvisioningToken(String provisioningToken)
{
    if (_apiProvisioningToken == provisioningToken)
    {
        return;
    }
    _apiProvisioningToken = provisioningToken;
    setConfigString(_nvram_provisioning_token_key, provisioningToken);
}

bool IoT::setProvisioningTokenIfEmpty(String provisioningToken)
{
    if (!_apiProvisioningToken.isEmpty())
    {
        return false;
    }
    setProvisioningToken(provisioningToken);
    return true;
}

void IoT::setApiToken(String apiToken)
{
    if (_apiToken == apiToken)
    {
        return;
    }
    _apiToken = apiToken;
    setConfigString(_nvram_api_token_key, apiToken);
}

void IoT::setCACert(const char *server_certificate)
{
    _wifiClient.setCACert(server_certificate);
}

void IoT::setClientCertificateAndKey(const char *client_certificate, const char *client_key)
{
    _wifiClient.setCertificate(client_certificate);
    _wifiClient.setPrivateKey(client_key);
}

void IoT::setInsecure()
{
    _wifiClient.setInsecure();
}


// *****************************************************************************
// NTP time
// *****************************************************************************

bool IoT::syncNtpTime(const char* ntpServer1, const char *ntpServer2, const char *ntpServer3, unsigned long timeout_ms)
{
    time_t wrongTimeThreshold = 365 * 24 * 3600;
    configTime(0, 0, ntpServer1, ntpServer2, ntpServer3);
    Serial.print("Waiting for NTP time sync ");
    time_t now = time(nullptr);

    unsigned long startTime = millis();
    while (now < wrongTimeThreshold && (millis() - startTime) < timeout_ms)
    {
        delay(500);
        Serial.print(".");
        now = time(nullptr);
    }

    if (now < wrongTimeThreshold)
    {
        Serial.printf("\nNTP time sync failed: %s\n", asctime(gmtime(&now)));
        return false;
    }

    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);
    Serial.printf("\nNTP time sync success, time=%s\n", asctime(&timeinfo));
    return true;
}


// *****************************************************************************
// HTTP requests
// *****************************************************************************

void IoT::_addRequestHeader(HTTPClient& http, std::map<String, String> &header)
{
    if (header.find("Host") == header.end() && !_apiHostHeader.isEmpty())
    {
        Serial.println("  header Host=" + _apiHostHeader);
        http.addHeader("Host", _apiHostHeader);
    }
    if (header.find("Accept") == header.end())
    {
        Serial.println("  header Accept=application/json");
        http.addHeader("Accept", "application/json");
    }
    for (auto const& kv : header)
    {
        Serial.println("  header " + kv.first + "=" + kv.second);
        http.addHeader(kv.first.c_str(), kv.second.c_str());
    }
    if (!_apiToken.isEmpty())
    {
        Serial.println("  header Authorization=" + _apiToken);
        http.addHeader("Authorization", _apiToken);
    }
}

// *****************************************************************************

bool IoT::_apiCheckForUpdate(String url, const char *nvram_etag_key, const char *nvram_date_key)
{
    url = _replaceVars(url);
    // get etag and date from preferences
    Preferences preferences;
    preferences.begin("iot", true);
    String etag = preferences.getString(nvram_etag_key, "");
    String date = preferences.getString(nvram_date_key, "");
    preferences.end();

    Serial.println("Checking url for updates: url=" + url + " etag=" + etag + " date=" + date);
    HTTPClient http;
    http.begin(_wifiClient, url);
    std::map<String, String> request_header = {
        {"If-None-Match", etag},
        {"If-Modified-Since", date}
    };
    _addRequestHeader(http, request_header);
    int httpStatusCode = http.sendRequest("HEAD");

    Serial.printf("Checking url for updates: HTTP HEAD: %d\n", httpStatusCode);
    if (httpStatusCode < 0) {
        Serial.printf("Checking url for updates: error %s\n", http.errorToString(httpStatusCode).c_str());
        http.end();
        return false;
    }
    return (httpStatusCode >= 200) && (httpStatusCode < 300);
}

// *****************************************************************************

String IoT::get(String url, String body, std::map<String, String> header)
{
    // TODO: implement
    Serial.println("get: not implemented");
    return "not implemented";
}

String IoT::apiGet(String apiPath, String body, std::map<String, String> header)
{
    String url = getApiUrlForPath(apiPath);
    url = _replaceVars(url);
    Serial.printf("HTTP GET request: url=%s\n", url.c_str());

    HTTPClient http;
    http.begin(_wifiClient, url);
    _addRequestHeader(http, header);

    int httpStatusCode = http.sendRequest("GET", (uint8_t*)body.c_str(), body.length());
    Serial.printf("HTTP GET request: status code %d\n", httpStatusCode);
    if (httpStatusCode < 0) {
        Serial.printf("HTTP GET request: error %s\n", http.errorToString(httpStatusCode).c_str());
        http.end();
        return "";
    }
    String payload = http.getString();
    if (httpStatusCode < 200 || httpStatusCode >= 300)
    {
        Serial.printf("HTTP GET request: error payload %s\n", payload.c_str());
        http.end();
        return "";
    }
    http.end();
    return payload;
}

// *****************************************************************************

String IoT::post(String url, String body, std::map<String, String> header)
{
    // TODO: implement
    Serial.println("post: not implemented");
    return "not implemented";
}

String IoT::apiPost(String apiPath, String body, std::map<String, String> header)
{
    String url = getApiUrlForPath(apiPath);
    url = _replaceVars(url);
    Serial.printf("HTTP POST request: url=%s\n", url.c_str());

    HTTPClient http;
    http.begin(_wifiClient, url);
    if (header.find("Content-Type") == header.end())
    {
        header.insert(std::pair<String, String>("Content-Type", "application/json"));
    }
    _addRequestHeader(http, header);

    int httpStatusCode = http.POST(body);
    Serial.printf("HTTP POST request: status code %d\n", httpStatusCode);
    if (httpStatusCode < 0) {
        Serial.printf("HTTP POST request: error %s\n", http.errorToString(httpStatusCode).c_str());
        http.end();
        return "";
    }
    String payload = http.getString();
    if (httpStatusCode < 200 || httpStatusCode >= 300)
    {
        Serial.printf("HTTP POST request: error payload %s\n", payload.c_str());
        http.end();
        return "";
    }
    http.end();
    return payload;
}

String IoT::postTelemetry(String body, String kind, String apiPath)
{
    apiPath.replace("{kind}", kind);
    // other variables are replaced in apiPost()
    return apiPost(apiPath, body);
}

String IoT::postLog(String body, String apiPath)
{
    return apiPost(apiPath, body, {{"Content-Type", "text/plain"}});
}


// *****************************************************************************
// Provisioning
// *****************************************************************************

bool IoT::updateProvisioning(bool forceProvisioning, String apiPath)
{
    if (!forceProvisioning && !_apiToken.isEmpty())
    {
        Serial.println("updateProvisioning: already provisioned");
        return false;
    }

    // execute HTTP POST request
    String request = 
            "{\"project_name\":\"" + _apiProject + "\","
            "\"device_name\":\"" + device_id() + "\","
            "\"provisioning_token\":\"" + _apiProvisioningToken + "\"}";
    String response = apiPost(apiPath, request);
    if (response.isEmpty())
    {
        Serial.println("updateProvisioning: no response");
        return false;
    }

    // parse response
    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, response);
    if (error)
    {
        Serial.printf("updateProvisioning: JSON deserialization failed: %s\n", error.c_str());
        return false;
    }
    if (!doc.containsKey("access_token"))
    {
        Serial.println("updateProvisioning: no access_token");
        return false;
    }
    if (!doc.containsKey("token_type"))
    {
        Serial.println("updateProvisioning: no token_type");
        return false;
    }
    String api_token = doc["token_type"].as<String>() + " " + doc["access_token"].as<String>();
    setApiToken(api_token);
    Serial.println("updateProvisioning: new api_token");
    return true;
}


// *****************************************************************************
// Configuration
// *****************************************************************************

bool IoT::updateConfig(String apiPath)
{
    // check for update
    String url = getApiUrlForPath(apiPath);
    bool updateAvailable = _apiCheckForUpdate(url, _nvram_config_etag_key, _nvram_config_date_key);
    if (!updateAvailable)
    {
        Serial.println("updateConfig: no update available");
        return false;
    }

    // execute HTTP GET request
    HTTPClient http;
    http.begin(_wifiClient, url);
    http.addHeader("Accept", "application/json");
    const char *headerKeys[] = {"ETag", "Last-Modified"};
    http.collectHeaders(headerKeys, 2);
    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK)
    {
        Serial.printf("HTTP GET failed: %d\n", httpCode);
        return false;
    }

    // decode JSON payload
    String payload = http.getString();
    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, payload);
    if (error)
    {
        Serial.printf("updateConfig: JSON deserialization failed: %s\n", error.c_str());
        return false;
    }

    // store config + etag/date in preferences
    Preferences preferences;
    preferences.begin("iot", false);
    for (JsonPair kv : doc.as<JsonObject>())
    {
        String key = kv.key().c_str();
        if (kv.value().is<int>())
        {
            int value = kv.value().as<int>();
            if (value != preferences.getInt(key.c_str(), -1))
            {
                Serial.println("Config: " + key + "=" + value);
                preferences.putInt(key.c_str(), value);
            }
        } else if (kv.value().is<String>())
        {
            String value = kv.value().as<String>();
            if (value != preferences.getString(key.c_str(), ""))
            {
                Serial.println("Config: " + key + "=" + value);
                preferences.putString(key.c_str(), value);
            }
        } else
        {
            Serial.println("Config: " + key + "=<unknown type>");
        }
    }
    String new_etag = http.header("ETag");
    String new_date = http.header("Last-Modified");
    preferences.putString(_nvram_config_etag_key, new_etag);
    preferences.putString(_nvram_config_date_key, new_date);
    preferences.end();

    // cleanup
    http.end();
    Serial.println("updateConfig: Configuration data update finished etag=" + new_etag + " date=" + new_date);
    return true;
}

// *****************************************************************************

String IoT::getConfigHttpEtag()
{
    return getConfigString(_nvram_config_etag_key, "");
}

String IoT::getConfigHttpDate()
{
    return getConfigString(_nvram_config_date_key, "");
}

// *****************************************************************************

void IoT::setConfigString(const char *key, String value)
{
    Preferences preferences;
    preferences.begin("iot", false);
    preferences.putString(key, value.c_str());
    preferences.end();
}

String IoT::getConfigString(const char *key, String defaultValue)
{
    Preferences preferences;
    preferences.begin("iot", true);
    String value = preferences.getString(key, defaultValue);
    preferences.end();
    return value;
}

void IoT::setConfigInt(const char *key, int value)
{
    Preferences preferences;
    preferences.begin("iot", false);
    preferences.putInt(key, value);
    preferences.end();
}

int IoT::getConfigInt(const char *key, int defaultValue)
{
    Preferences preferences;
    preferences.begin("iot", true);
    int value = preferences.getInt(key, defaultValue);
    preferences.end();
    return value;
}


// *****************************************************************************
// Firmware
// *****************************************************************************

String IoT::getFirmwareVersion()
{
    String ret = "";
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_app_desc_t app_info;
    if (esp_ota_get_partition_description(running, &app_info) == ESP_OK)
    {
        ret = String(app_info.project_name) 
            + " " + String(app_info.version) 
            + " " + String(app_info.date) 
            + " " + String(app_info.time) 
            + " IDF " + String(app_info.idf_ver) 
            + " sec " + String(app_info.secure_version);
    }
    return ret;
}

String IoT::getFirmwareSha256()
{
    String ret = "";
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_app_desc_t app_info;
    if (esp_ota_get_partition_description(running, &app_info) == ESP_OK)
    {
        char buf[sizeof(app_info.app_elf_sha256) * 2 + 1];
        for (int i = 0; i < sizeof(app_info.app_elf_sha256); i++)
        {
            sprintf(buf + i * 2, "%02x", app_info.app_elf_sha256[i]);
        }
        buf[sizeof(buf) - 1] = '\0';
        ret = String(buf);
    }
    return ret;
}

String IoT::getFirmwareHttpEtag()
{
    return getConfigString(_nvram_firmware_etag_key, "");
}

String IoT::getFirmwareHttpDate()
{
    return getConfigString(_nvram_firmware_date_key, "");
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
bool IoT::updateFirmware(String apiPath)
{
    String url = getApiUrlForPath(apiPath);
    bool updateAvailable = _apiCheckForUpdate(url, _nvram_firmware_etag_key, _nvram_firmware_date_key);
    if (!updateAvailable)
    {
        Serial.println("No update available");
        return false;
    }

    Serial.printf("Updating firmware from %s\n", url.c_str());
    updateFirmwareNewEtag = "";
    updateFirmwareNewDate = "";
    HttpsOTA.onHttpEvent(updateFirmwareHttpEvent); // HttpsOTA.begin(url, server_certificate); 
    HttpsOTA.begin(url.c_str(), NULL, true);
    while (1)
    {
        HttpsOTAStatus_t ota_status = HttpsOTA.status();
        if (ota_status == HTTPS_OTA_SUCCESS)
        {
            Serial.println("Firmware update successful");
            break;
        } else if (ota_status == HTTPS_OTA_FAIL)
        {
            Serial.println("Firmware update failed");
            return false;
        }
        Serial.print(".");
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
// System management: sleep, restart, shutdown
// *****************************************************************************

void IoT::deepSleep(int64_t sleep_duration_ms)
{
    lastSleepDuration_ms = sleep_duration_ms;
    activeDuration_ms = millis() - bootTimestamp_ms;
    Serial.printf("Active for %lld ms, going to deep sleep for %lld ms\n", activeDuration_ms, sleep_duration_ms);
    esp_deep_sleep(sleep_duration_ms * 1000);
}

// *****************************************************************************

void IoT::restart()
{
    activeDuration_ms = millis() - bootTimestamp_ms;
    Serial.printf("Active for %lld ms, restarting\n", activeDuration_ms);
    esp_restart();
}

// *****************************************************************************

void IoT::shutdown()
{
    activeDuration_ms = millis() - bootTimestamp_ms;
    Serial.printf("Active for %lld ms, shutting down\n", activeDuration_ms);
    esp_deep_sleep_start();
}
