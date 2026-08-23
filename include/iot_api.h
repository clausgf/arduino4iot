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

#include "iot_result.h"
#include "iot_seed.h"

// *****************************************************************************

class Iot; // for friend access to the seeding internals

class IotApi
{
public:
    // disallow copying & assignment
    IotApi(const IotApi&) = delete;
    IotApi& operator=(const IotApi&) = delete;

    IotApi();

    /**
     * Load the API configuration (endpoint, project, provisioning/device token,
     * TLS trust) from NVS, where it was placed by Iot::seedCredentials().
     */
    void begin();
    void end();


    // **********************************************************************
    // API configuration
    // **********************************************************************

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

    // Note: the API endpoint, project name, provisioning token and TLS trust
    // (CA cert / bundle / insecure / client cert) are no longer set at runtime.
    // They are seeded from build-time defaults into NVS via
    // Iot::seedCredentials() and loaded by begin(). See docs/concepts.md.

    /**
     * Close the HTTP connection to the server.
     *
     * All requests within a wakeup cycle share a single keep-alive TCP/TLS
     * connection. This method closes it cleanly. It is called automatically
     * before deep sleep, restart and shutdown; call it manually to release the
     * connection (and its TLS buffers) earlier.
     */
    void closeConnection();

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

    // Note: the provisioning token is seeded from a build-time default into NVS
    // via Iot::seedCredentials() and loaded by begin(); it is no longer set at
    // runtime.

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
     * @return a typed result. Ok if provisioning information is current (with
     *         or without a new token). On failure the cause is distinguishable:
     *         - isTransportError(): TLS/connectivity problem (e.g. no CA cert
     *           configured, server unreachable); see .transportError;
     *         - .httpStatus == 403: server rejected provisioning (project/device
     *           inactive or not approved, HTTP API disabled);
     *         - .httpStatus == 401: provisioning token invalid;
     *         - .httpStatus == IotResult::STATUS_NO_PROVISIONING_TOKEN: no
     *           provisioning token configured on the device;
     *         - .httpStatus == IotResult::STATUS_MALFORMED_RESPONSE: 2xx but the
     *           response body lacked a valid accessToken/tokenType.
     */
    IotResult updateProvisioning(const String& apiPath = "provision");

    /**
     * Convenience wrapper around updateProvisioning() for callers that only
     * need a yes/no answer.
     *
     * @return true if provisioning information is current (with or without update)
     */
    bool updateProvisioningOk(const String& apiPath = "provision")
    {
        return updateProvisioning(apiPath).isOk();
    }


    // **********************************************************************
    // HTTP requests
    // **********************************************************************

    /**
     * Return the effective URL for a given apiPath.
     *
     * This function replaces variables known to the IoT system like
     * {project}, {device} and {board} (the IOT_BOARD_ID build default, empty
     * if undefined - see Iot::getBoardId()) in the given path.
     * It also adds the API base URL.
     *
     * @param apiPath the path relative to the API base URL, e.g. "/foo/{device}/bar"
     * @return the full URL, e.g. "https://api.example.com/api/foo/e32_123/bar"
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
     * Send a GET request like the other apiGet() overload, additionally
     * returning selected response headers.
     *
     * @param oResponseHeader receives the collected response headers
     * @param collectResponseHeaderKeys the response header names to collect,
     *        e.g. {"ETag", "Last-Modified"}
     */
    int apiGet(String& oResponse, std::map<String, String>& oResponseHeader, const String& apiPath,
        const std::vector<String>& collectResponseHeaderKeys, const String& body = "", const std::map<String, String>& headers = {});

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
     * @return the result of the request, wrapping the HTTP status code
     */
    IotResult apiForward(String& oResponse, const String& forwardingName, const String& remainingPath = "",
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
     * @return a typed result: Ok on 2xx, otherwise inspect .httpStatus (e.g. 413
     *         if the file exceeds the server's size limit) or .transportError
     */
    IotResult uploadFile(const String& filename, const String& content, const String& contentType = "application/octet-stream");

    /**
     * Send a HEAD request to the given URL and check if the server has an
     * update, based on the ETag or Last-Modified headers. The ETag and
     * Last-Modified headers are stored in NVRAM under the given keys.
     *
     * @return a typed result: Ok (2xx) if an update is available,
     *         isNotModified() (304) if the resource is unchanged, otherwise an
     *         HTTP or transport error.
     */
    IotResult apiCheckForUpdate(const String& apiPath, const char *nvram_etag_key, const char *nvram_date_key);


    // **********************************************************************
    // Firmware
    // **********************************************************************

    String getFirmwareHttpEtag();
    String getFirmwareHttpDate();

    /**
     * Update the firmware from the given API path.
     *
     * The default path's {board} placeholder is substituted with the
     * IOT_BOARD_ID build default (see Iot::getBoardId()), letting the server
     * host per-hardware-variant firmware images side by side. If IOT_BOARD_ID
     * is undefined, {board} substitutes to an empty string - pass an explicit
     * apiPath without {board} in that case.
     *
     * @return a typed result:
     *         - Ok: new firmware was downloaded and flashed (runs on next boot);
     *         - isNotModified() (304): firmware already up to date, nothing done;
     *         - .httpStatus == IotResult::STATUS_UPDATE_FAILED: an update was
     *           available but the download/flash did not complete;
     *         - other HttpError / TransportError: the update check failed.
     *         Use isOkOrNotModified() to test for "no error".
     */
    IotResult updateFirmware(const String& apiPath = "file/{project}/{device}/firmware-{board}.bin", const std::map<String, String>& header = {});


    // **********************************************************************
    // P r i v a t e
    // **********************************************************************

    friend class Iot; // Iot::seedCredentials() writes the seeded API config

private:
    const char * _nvram_provisioning_token_key = "provToken";
    const char * _nvram_device_token_key = "deviceToken";
    const char * _nvram_device_token_expiry_key = "deviceTokExp";
    const char * _nvram_firmware_etag_key = "firmwareEtag";
    const char * _nvram_firmware_date_key = "firmwareDate";
    const char * _nvram_api_url_key = "apiUrl";
    const char * _nvram_project_key = "project";
    const char * _nvram_tls_mode_key = "tlsMode";
    const char * _nvram_ca_cert_key = "caCert";
    const char * _nvram_client_cert_key = "cliCert";
    const char * _nvram_client_key_key = "cliKey";

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

    // seeded TLS material kept alive for the object lifetime: the core stores the
    // cert pointers (it does not copy them), so these members must outlive use
    String _caCertPem;
    String _clientCertPem;
    String _clientKeyPem;

    bool _tlsServerTrustConfigured; ///< set when a TLS trust was applied in begin()
    bool _tlsTrustWarningLogged;    ///< guard so the missing-trust warning is logged once

    /// Write the seeded API config (endpoint, project, token, TLS) to NVS.
    /// Called by Iot::seedCredentials() with a shared open "iot" handle.
    void _seedFromConfig(Preferences& preferences, const IotSeedConfig& cfg, bool force);

    /// Set the base URL in RAM, normalizing a trailing slash.
    void _setApiUrl(const String& apiBaseurl);

    /// Apply the seeded TLS trust to the WiFiClientSecure and the OTA client.
    void _applyTls(IotTlsMode mode);
    void _applyCACert(const char *serverCert);
    void _applyCACertBundle();
    void _applyCertInsecure();
    void _applyClientCertificateAndKey(const char *clientCert, const char *clientKey);

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
     * Log a single clear error if the API URL is https but no TLS server trust
     * was seeded (tlsMode None with an https endpoint). Without it the TLS
     * handshake fails with an opaque transport error that is hard to attribute.
     */
    void _warnIfTlsTrustMissing();

    /**
     * @return a HTTPClient instance. The instance is created on the first call.
     */
    HTTPClient & _getHttpClient();

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
