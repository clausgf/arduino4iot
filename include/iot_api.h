/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

#include <map>

#include "Arduino.h"
#include <HTTPClient.h>
#include <WiFiClient.h>

// *****************************************************************************

class IotApi
{
public:
    // disallow copying & assignment
    IotApi(const IotApi&) = delete;
    IotApi& operator=(const IotApi&) = delete;

    IotApi();
    void begin();
    void end();


    // **********************************************************************
    // API configuration
    // **********************************************************************

    /**
     * Set the host and the base URL for API calls.
     * 
     * Base URLs starting with "https://" are considered secure and
     * use TLS via WiFiClientSecure. 
     * Base URLs starting with "http://" are considered insecure
     * and use plain HTTP via WifiClient.
     * 
     * This method must be called before any other API call.
     * 
     * @param host the host name, e.g. "api.example.com"
     * @param apiBaseurl the full base URL, e.g. "https://api.example.com/iot/api/""
     */
    void setApiUrl(String apiBaseurl);
    
    /**
     * Set the project name, e.g. "my-project". 
     * 
     * The project name is replaced for {project} in actual urls generated 
     * for API calls, @see getApiUrlForPath().
     */
    void setProjectName(String project);

    /**
     * Set the device name, e.g. "my-device". 
     * 
     * The device name is replaced for {device} in actual urls generated 
     * for API calls, @see getApiUrlForPath().
     */
    void setDeviceName(String device);

    /**
     * Optionally set additional HTTP headers to be used in any all requests.
     * 
     * Headers like the HTTP host header are useful if the API is behind 
     * a reverse proxy. 
     * The headers given here are used in any API request. For
     * a specific request, they can be overwritten on a by-header basis.
     * 
     * @param header a map of header names and values, e.g. {"Host", "api.example.com"}
     */
    void setApiHeader(std::map<String, String> header = {});

    /**
     * Provide the CA certificate for checking server certificates
     * in TLS connections.
     * This method silently fails if the WiFi client is not an instance 
     * of WiFiClientSecure.
     */
    void setCACert(const char *serverCert);

    /**
     * Optionally provide a client certificate and key for TLS 
     * connections.
     * This method silently fails if the WiFi client is not an instance 
     * of WiFiClientSecure.
     */
    void setClientCertificateAndKey(const char *clientCert, const char *clientKey);

    /**
     * Deactivate checking the server certificate for TLS connections.
     * This method silently fails if the WiFi client is not an instance 
     * of WiFiClientSecure.
     */
    void setCertInsecure();


    // **********************************************************************
    // Provisioning
    // **********************************************************************

    /**
     * Set the provisioning token, e.g. "1234567890abcdef".
     * 
     * This information is stored in non-volatile NVRAM and restored on the
     * next startup.
     */
    void setProvisioningToken(String provisioningToken);

    /**
     * Set the provisioning key like @see setProvisioningToken() if no
     * other provisioning key is set yet.
     * 
     * The method does not overwrite an existing provisioning token.
     * This is useful to have a default for first-time provisioning
     * which can be overwritten at runtime.
     * 
     * @return true if new token was set, if an existing
     * provisioning token was not replaced.
     */
    bool setProvisioningTokenIfEmpty(String provisioningToken);

    /**
     * Clear the provisioning token
     */
    void clearProvisioningToken();

    /**
     * Set the device token for API access. 
     * 
     * The API device token is used for authentication in API requests.
     * Usually, this key is automatically obtained and updated 
     * during provisioning.
     * 
     * This information is stored in non-volatile NVRAM and restored on the
     * next startup.
     * 
     * @param apiToken the API token, e.g. "1234567890abcdef"
     */
    void setDeviceToken(String apiToken);

    /**
     * Clear the API token. This is useful for forcing provisioning.
     */
    void clearDeviceToken();

    // TODO think about periodic updates of provisioning and api tokens
    bool updateProvisioning(String apiPath = "provision");


    // **********************************************************************
    // HTTP requests
    // **********************************************************************

    /**
     * Return the effective URL for a given apiPath.
     * 
     * This function replaces variables known to the IoT system like
     * {project} and {device} in the given path. 
     * It also adds the API base URL.
     * 
     * @param apiPath the path relative to the API base URL, e.g. "/foo/{device}/bar"
     * @return the full URL, e.g. "https://api.example.com/iot/api/foo/e32-123/bar"
     */
    String getApiUrlForPath(String apiPath);

    /**
     * Send a request to the given URL and return the status code and response body
     */
    int apiRequest(String& oResponse, std::map<String, String>& oResponseHeader, 
        const char * requestType, String apiPath, String requestBody = "", std::map<String, String> requestHeader = {}, 
        const char * collectResponseHeaderKeys[] = {}, const size_t collectResponseHeaderKeysCount = 0);

    /**
     * Send a GET request to the API using the given API path 
     * and return the response body if successful (200 <= status code < 300). 
     * Return an empty string otherwise.
     * 
     * The actual URL is generated by @see getApiUrlForPath.
     * Add default headers like "Accept: application/json", 
     * which can be overwritten by @see setApiHeaders,
     * which can be overwritten in the header parameter.
     * 
     * @param oResponse the response body
     * @param apiPath the API path relative to the API base URL, e.g. "/foo/{device}/bar"
     * @param body the request body, e.g. a JSON string, defaults to ""
     * @param headers additional HTTP headers which overrides the defaults, defaults to {}
     * @return the HTTP response status code
     */
    int apiGet(String& oResponse, String apiPath, String body = "", std::map<String, String> headers = {});

    /**
     * Send a HEAD request to the API using the given API path and return
     * the response status code, the etag and the date header.
     * 
     * @return the HTTP response status code
     */
    int apiHead(String apiPath, std::map<String, String> headers = {});

    /**
     * Send a POST request to the given API path and return the response body
     * if successful (200 <= status code < 300). Return an empty string
     * otherwise. This method is similar to @see apiGet.
     * 
     * The "Content-Type" header defaults to "application/json" if not specified.
     */
    int apiPost(String& oResponse, String apiPath, String body, std::map<String, String> headers = {});

    /**
     * Send a HEAD request to the given URL and check if the server has an
     * update, based on the ETag or Last-Modified headers. The ETag and
     * Last-Modified headers are stored in NVRAM under the given keys.
     */
    bool apiCheckForUpdate(String apiPath, const char *nvram_etag_key, const char *nvram_date_key);


    // **********************************************************************
    // Firmware
    // **********************************************************************

    String getFirmwareHttpEtag();
    String getFirmwareHttpDate();
    bool updateFirmware(String apiPath = "file/{project}/{device}/firmware.bin", std::map<String, String> header = {});


    // **********************************************************************
    // P r i v a t e
    // **********************************************************************

private:
    const char * _nvram_provisioning_token_key = "provToken";
    const char * _nvram_device_token_key = "deviceToken";
    const char * _nvram_firmware_etag_key = "firmwareEtag";
    const char * _nvram_firmware_date_key = "firmwareDate";

    String _baseUrl;
    std::map<String, String> _defaultRequestHeader;
    String _projectName;
    String _deviceName;
    String _provisioningToken;
    String _deviceToken;

    WiFiClientSecure * _wifiClientSecurePtr;
    WiFiClient * _wifiClientPtr;
    HTTPClient * _httpClientPtr;

    /**
     * @return a WiFiClient instance, either secure or insecure, depending
     * on the API base URL. The instance is created on the first call.
     */
    WiFiClient * _getWiFiClientPtr();

    /**
     * @return wheter the WiFiClient instance is secure or insecure.
     */
    bool _isWiFiClientSecure();

    /**
     * @return a HTTPClient instance. The instance is created on the first call.
     */
    HTTPClient & _getHttpClient();

    /**
     * Replace variables known to the IoT system like {project} 
     * and {device} in a string. 
     * This is useful for generating URLs.
     */
    String _replaceVars(String str);

    /**
     * Add request header
     */
    void _addRequestHeader(HTTPClient& http, std::map<String, String> &header);
    /**
     * Send a HEAD request to the given URL and check if the server has an 
     * update, based on the ETag or Last-Modified headers. 
     * If availabe, the API token is used for authentication.
     */
};

// *****************************************************************************

extern IotApi api;
