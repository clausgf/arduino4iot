/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

#include <map>
#include <vector>

#include "Arduino.h"
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

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
     * @param apiBaseurl the full base URL, e.g. "https://api.example.com/api/"
     */
    void setApiUrl(const String& apiBaseurl);

    /**
     * Set the project name, e.g. "my-project".
     *
     * The project name is replaced for {project} in actual urls generated
     * for API calls, @see getApiUrlForPath().
     */
    void setProjectName(const String& project);

    /**
     * Set the device name, e.g. "my-device".
     *
     * The device name is replaced for {device} in actual urls generated
     * for API calls, @see getApiUrlForPath().
     */
    void setDeviceName(const String& device);

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
    void setApiHeader(const std::map<String, String>& header = {});

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

    /**
     * Set a timeout for establishing the TCP connection to the server.
     * @param timeout_ms the timeout in milliseconds
     */
    void setConnectionTimeout_ms(int32_t timeout_ms);

    /**
     * Set a timeout for waiting for data in an HTTP request.
     * @param timeout_ms the timeout in milliseconds
     */
    void setRequestTimeout_ms(uint16_t timeout_ms);

    /// @deprecated use setConnectionTimeout_ms()
    __attribute__((deprecated("use setConnectionTimeout_ms()")))
    void apiSetConnectionTimeout(int32_t timeout) { setConnectionTimeout_ms(timeout); }

    /// @deprecated use setRequestTimeout_ms()
    __attribute__((deprecated("use setRequestTimeout_ms()")))
    void apiSetRequestTimeout(uint16_t timeout) { setRequestTimeout_ms(timeout); }


    // **********************************************************************
    // Provisioning
    // **********************************************************************

    /**
     * Set the provisioning token, e.g. "1234567890abcdef".
     *
     * This information is stored in non-volatile NVRAM and restored on the
     * next startup.
     */
    void setProvisioningToken(const String& provisioningToken);

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
    bool setProvisioningTokenIfEmpty(const String& provisioningToken);

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
     * @param apiToken the API token including the scheme, e.g. "bearer 1234567890abcdef"
     * @param expiresAt the expiry time of the token as unix timestamp,
     *        0 if unknown
     */
    void setDeviceToken(const String& apiToken, time_t expiresAt = 0);

    /**
     * Clear the API token. This is useful for forcing provisioning.
     */
    void clearDeviceToken();

    /**
     * @return the expiry time of the device token as unix timestamp or
     * 0 if unknown
     */
    time_t getDeviceTokenExpiresAt() { return (time_t)_deviceTokenExpiresAt; }

    /**
     * Set the margin before the device token expiry at which
     * updateProvisioningOk() proactively requests a new token.
     * The default is 3600 s. Choose a margin larger than your sleep
     * interval to avoid waking up with an expired token.
     */
    void setDeviceTokenExpiryMargin_s(int margin_s) { _deviceTokenExpiryMargin_s = margin_s; }

    /**
     * Update the provisioning information from the API.
     *
     * If the device already has an API token, it is kept unless it is
     * expired or expires soon (see setDeviceTokenExpiryMargin_s()).
     * In that case, or if no token is available, a new token is requested
     * using the provisioning token.
     * The token expiry is determined from the *expiresIn* field of the
     * provisioning response and stored in NVRAM.
     *
     * @return true if provisioning information is current (with or without update)
     */
    bool updateProvisioningOk(const String& apiPath = "provision");


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
     * @return the full URL, e.g. "https://api.example.com/api/foo/e32-123/bar"
     */
    String getApiUrlForPath(const String& apiPath);

    /**
     * Send a request to the given URL and return the status code and response body.
     *
     * On HTTP 401, the device token is cleared; if a provisioning token is
     * available, the device is re-provisioned and the request is retried once.
     * On HTTP 403 (project inactive, device inactive or not approved), the
     * device token is kept - re-provisioning would not help in this case.
     */
    int apiRequest(String& oResponse, std::map<String, String>& oResponseHeader,
        const char * requestType, const String& apiPath, const String& requestBody = "",
        const std::map<String, String>& requestHeader = {},
        const std::vector<String>& collectResponseHeaderKeys = {});

    /**
     * Send a GET request to the API using the given API path
     * and return the response body if successful (200 <= status code < 300).
     * Return an empty string otherwise.
     *
     * The actual URL is generated by @see getApiUrlForPath.
     * Add default headers like "Accept: application/json",
     * which can be overwritten by @see setApiHeader,
     * which can be overwritten in the header parameter.
     *
     * @param oResponse the response body
     * @param apiPath the API path relative to the API base URL, e.g. "/foo/{device}/bar"
     * @param body the request body, e.g. a JSON string, defaults to ""
     * @param headers additional HTTP headers which overrides the defaults, defaults to {}
     * @return the HTTP response status code
     */
    int apiGet(String& oResponse, const String& apiPath, const String& body = "", const std::map<String, String>& headers = {});

    /**
     * Send a HEAD request to the API using the given API path and return
     * the response status code.
     *
     * @return the HTTP response status code
     */
    int apiHead(const String& apiPath, const std::map<String, String>& headers = {});

    /**
     * Send a POST request to the given API path and return the response body
     * if successful (200 <= status code < 300). Return an empty string
     * otherwise. This method is similar to @see apiGet.
     *
     * The "Content-Type" header defaults to "application/json" if not specified.
     */
    int apiPost(String& oResponse, const String& apiPath, const String& body, const std::map<String, String>& headers = {});

    /**
     * Send a PUT request to the given API path and return the response body
     * if successful (200 <= status code < 300). This method is similar to
     * @see apiGet.
     */
    int apiPut(String& oResponse, const String& apiPath, const String& body, const std::map<String, String>& headers = {});

    /**
     * Send a request through the server's forwarding endpoint
     * (GET forward/{project}/{device}/{forwardingName}/{remainingPath}).
     *
     * The server proxies the request to the upstream URL configured for
     * forwardingName in the project settings and returns the upstream
     * response.
     *
     * @param oResponse the upstream response body
     * @param forwardingName the name of the forwarding entry in the project config
     * @param remainingPath path suffix appended to the configured upstream URL, may be empty
     * @param body an optional request body passed to the upstream service
     * @return the HTTP response status code
     */
    int apiForward(String& oResponse, const String& forwardingName, const String& remainingPath = "",
        const String& body = "", const std::map<String, String>& headers = {});

    /**
     * Upload a file to the device-specific file storage on the server
     * (PUT file/{project}/{device}/{filename}), e.g. for diagnostic dumps.
     *
     * The filename may only contain alphanumeric characters, '.', '_' and '-'
     * and must start with an alphanumeric character.
     *
     * @param filename the target filename on the server
     * @param content the file content
     * @param contentType the content type, defaults to "application/octet-stream"
     * @return true if the upload was successful
     */
    bool uploadFile(const String& filename, const String& content, const String& contentType = "application/octet-stream");

    /**
     * Send a HEAD request to the given URL and check if the server has an
     * update, based on the ETag or Last-Modified headers. The ETag and
     * Last-Modified headers are stored in NVRAM under the given keys.
     */
    bool apiCheckForUpdate(const String& apiPath, const char *nvram_etag_key, const char *nvram_date_key);


    // **********************************************************************
    // Firmware
    // **********************************************************************

    String getFirmwareHttpEtag();
    String getFirmwareHttpDate();

    /**
     * Update the firmware from the given API path.
     * @return true if firmware was updated
     */
    bool updateFirmware(const String& apiPath = "file/{project}/{device}/firmware.bin", const std::map<String, String>& header = {});


    // **********************************************************************
    // P r i v a t e
    // **********************************************************************

private:
    const char * _nvram_provisioning_token_key = "provToken";
    const char * _nvram_device_token_key = "deviceToken";
    const char * _nvram_device_token_expiry_key = "deviceTokExp";
    const char * _nvram_firmware_etag_key = "firmwareEtag";
    const char * _nvram_firmware_date_key = "firmwareDate";

    String _baseUrl;
    std::map<String, String> _defaultRequestHeader;
    String _projectName;
    String _deviceName;
    String _provisioningToken;
    String _deviceToken;
    int64_t _deviceTokenExpiresAt;
    int _deviceTokenExpiryMargin_s;
    bool _inProvisioning;

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
    void _addRequestHeader(HTTPClient& http, const std::map<String, String> &header);

    /**
     * Execute a single HTTP request without the 401 re-provisioning logic,
     * @see apiRequest().
     */
    int _performRequest(String& oResponse, std::map<String, String>& oResponseHeader,
        const char * requestType, const String& apiPath, const String& requestBody,
        const std::map<String, String>& requestHeader,
        const std::vector<String>& collectResponseHeaderKeys);
};

// *****************************************************************************

extern IotApi api;
