/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */


#include "Arduino.h"
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <map>

#pragma once

class IoT
{
public:
    int64_t lastSleepDuration_ms;
    int64_t bootTimestamp_ms;
    esp_sleep_wakeup_cause_t wakeupCause;
    RTC_DATA_ATTR static uint32_t bootCount;
    RTC_DATA_ATTR static int64_t activeDuration_ms;

    // disallow copying & assignment
    IoT(const IoT&) = delete;
    IoT& operator=(const IoT&) = delete;

    /**
     * Constructor, initialize all attributes.
     */
    IoT();

    /**
     * Connect to the given WiFi network and return true if successful.
     * The connection timeout can be specified in milliseconds.
     */
    bool connectWifi(String ssid, String password, unsigned long timeout_ms = 10000);

    /**
     * Return a unique device ID which is derived from the WiFi MAC address,
     * e.g. "e32-123456780abc"
     */
    String device_id();

    /**
     * Return the WiFi signal strength in dBm.
     */
    int rssi();

    // **********************************************************************
    // API configuration
    // **********************************************************************

    /**
     * Set the host and the base URL for API calls, e.g. "https://api.example.com/iot/api/".
     * As an option, a host header can be specified, e.g. "api.example.com".
     * This is useful if the API is behind a reverse proxy.
     * This information is stored in non-volatile NVRAM and restored on the
     * next startup.
     */
    void setApi(String host, String apiBaseurl, String hostHeader = "");

    /**
     * return the effective URL for a given apiPath, 
     * e.g. "/foo/{device_id}/bar" -> "https://api.example.com/iot/api/foo/e32-123/bar"
     */
    String getApiUrlForPath(String apiPath);

    /**
     * Set the project name, e.g. "my-project". This information could be 
     * used to build the URL for API calls. 
     * This information is stored in non-volatile NVRAM and restored on the
     * next startup.
     */
    void setProject(String project);

    /**
     * Set the provisioning key, e.g. "1234567890abcdef".
     * This information is stored in non-volatile NVRAM and restored on the
     * next startup.
     */
    void setProvisioningToken(String provisioningToken);

    /**
     * Set the provisioning key like @see setProvisioningToken() if no
     * other provisioning key is set yet. This is useful for first-time
     * provisioning. Return true if the key was set, false otherwise.
     */
    bool setProvisioningTokenIfEmpty(String provisioningToken);

    /**
     * Set the API key, e.g. "1234567890abcdef". This key is used for
     * authentication when sending API requests. Usually, this key is
     * obtained and automatically set during provisioning.
     * This information is stored in non-volatile NVRAM and restored on the
     * next startup.
     */
    void setApiToken(String apiToken);

    void setCACert(const char *serverCert);
    void setClientCertificateAndKey(const char *clientCert, const char *clientKey);
    void setInsecure();


    // **********************************************************************
    // NTP time
    // **********************************************************************

    bool syncNtpTime(const char * ntpServer1 = "pool.ntp.org", const char * ntpServer2 = "time.nist.gov", const char * ntpServer3 = nullptr, unsigned long timeout_ms = 10000);

    // **********************************************************************
    // HTTP requests
    // **********************************************************************

    /**
     * Send a GET request to the given URL and return the response body
     * if successful (200 <= status code < 300). Return an empty string
     * otherwise.
     */
    String get(String url, String body = "", std::map<String, String> header = {});

    /**
     * API version of @see get(), which automatically adds the API's base url
     *  and the API token
     */
    String apiGet(String apiPath, String body = "", std::map<String, String> header = {});

    /**
     * Send a POST request to the given URL and return the response body
     * if successful (200 <= status code < 300). Return an empty string
     * otherwise.
     * The "Content-Type" header defaults to "application/json" if not specified.
     */
    String post(String url, String body, std::map<String, String> header = {});
    String apiPost(String apiPath, String body, std::map<String, String> header = {});

    /**
     * Post telemetry data to the API. The body must be a valid JSON string.
     */
    String postTelemetry(String body, String kind, String apiPath = "telemetry/{project}/{device_id}/{kind}");

    /**
     * Post a log message to the API. The body consist of ESP32 formatted 
     * log lines. It is directly posted to the API with "Content-Type text/plain".
     */
    String postLog(String body, String apiPath = "log/{project}/{device_id}");


    // **********************************************************************
    // Logging
    // **********************************************************************

    // TODO implement logging by using the ESP32's log hooks; this might be a separate module
    // alternatively, use some nice c++ string formatting library


    // **********************************************************************
    // Provisioning
    // **********************************************************************

    // TODO think about periodic updates of provisioning and api tokens
    bool updateProvisioning(bool forceProvisioning, String apiPath = "auth/provision");


    // **********************************************************************
    // Configuration
    // **********************************************************************

    bool updateConfig(String apiPath = "files/{project}/{device_id}/config.json");
    String getConfigHttpEtag();
    String getConfigHttpDate();
    void setConfigString(const char *key, String value);
    String getConfigString(const char *key, String defaultValue = "");
    void setConfigInt(const char *key, int value);
    int getConfigInt(const char *key, int defaultValue = 0);


    // **********************************************************************
    // Firmware
    // **********************************************************************

    String getFirmwareVersion();
    String getFirmwareSha256();
    String getFirmwareHttpEtag();
    String getFirmwareHttpDate();
    bool updateFirmware(String apiPath = "files/{project}/{device_id}/firmware.bin");

    // **********************************************************************
    // System management: sleep, restart, shutdown
    // **********************************************************************

    void deepSleep(int64_t sleep_duration_ms);
    void restart();
    void shutdown();

private:
    const char * _nvram_api_host_key = "api_host";
    const char * _nvram_api_url_key = "api_url";
    const char * _nvram_api_host_header_key = "api_host_header";
    const char * _nvram_project_key = "project";
    const char * _nvram_provisioning_token_key = "prov_token";
    const char * _nvram_api_token_key = "api_token";
    const char * _nvram_config_etag_key = "config_etag";
    const char * _nvram_config_date_key = "config_date";
    const char * _nvram_firmware_etag_key = "firmware_etag";
    const char * _nvram_firmware_date_key = "firmware_date";

    String _apiHost;
    String _apiBaseUrl;
    String _apiHostHeader;
    String _apiProject;
    String _apiProvisioningToken;
    String _apiToken;
    WiFiClientSecure _wifiClient;

    /**
     * Replace variables known to the IoT system like {device_id} in a string, 
     * useful for generating URLs.
     */
    String _replaceVars(String str);

    /**
     * Send a HEAD request to the given URL and check if the server has an 
     * update, based on the ETag or Last-Modified headers. 
     * If availabe, the API token is used for authentication.
     */
    bool _apiCheckForUpdate(String url, const char *nvram_etag_key, const char *nvram_date_key);

    /**
     * Add request header
     */
    void _addRequestHeader(HTTPClient& http, std::map<String, String> &header);
};

extern IoT iot;
