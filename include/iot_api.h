/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

#include <ctime>
#include <map>
#include <string>
#include <vector>

#include "iot_ports.h"
#include "iot_result.h"
#include "iot_seed.h"
#include "iot_token.h"

// *****************************************************************************

class IotApi
{
public:
    // disallow copying & assignment
    IotApi(const IotApi&) = delete;
    IotApi& operator=(const IotApi&) = delete;

    IotApi();

    /**
     * Load the API configuration (endpoint, project, provisioning/device token,
     * TLS trust) from @p store and send requests through @p http, applying the
     * loaded TLS trust to it. Both must outlive this object.
     */
    void begin(IotStore& store, IotHttp& http, IotClock& clock);
    void end();

    /// Hardware variant substituted for {board} in a path; empty if unset.
    void setBoardId(const std::string& boardId) { _boardId = boardId; }

    /// @return the store the API configuration and tokens live in.
    IotStore& store() { return *_store; }

    /// @return the device token, i.e. the Authorization header value.
    const std::string& deviceToken() const { return _tokens.deviceToken(); }

    /// @return the seeded TLS trust, e.g. for an OTA download that opens its
    /// own connection.
    const IotTlsConfig& tlsConfig() const { return _tls; }

    /// Merge @p base with the setApiHeader() defaults and @p requestHeader,
    /// later sources winning. Public because an operation that opens its own
    /// connection (OTA) needs the same header set.
    std::map<std::string, std::string> mergeHeaders(std::map<std::string, std::string> base,
        const std::map<std::string, std::string>& requestHeader) const;


    // **********************************************************************
    // API configuration
    // **********************************************************************

    /**
     * Set the device name, e.g. "my-device".
     *
     * The device name is replaced for {device} in actual urls generated
     * for API calls, @see getApiUrlForPath().
     */
    void setDeviceName(const std::string& device);

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
    void setApiHeader(const std::map<std::string, std::string>& header = {});

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
    void setDeviceToken(const std::string& apiToken, time_t expiresAt = 0);

    /**
     * Clear the API token. This is useful for forcing provisioning.
     */
    void clearDeviceToken();

    /**
     * @return the expiry time of the device token as unix timestamp or
     * 0 if unknown
     */
    time_t getDeviceTokenExpiresAt() { return (time_t)_tokens.deviceTokenExpiresAt(); }

    /**
     * Set the margin before the device token expiry at which
     * updateProvisioningOk() proactively requests a new token.
     * The default is 3600 s. Choose a margin larger than your sleep
     * interval to avoid waking up with an expired token - but also smaller
     * than the lifetime the server issues, or every wakeup re-provisions.
     */
    void setDeviceTokenExpiryMargin_s(int margin_s) { _tokens.setExpiryMargin_s(margin_s); }

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
    IotResult updateProvisioning(const std::string& apiPath = "provision");

    /**
     * Convenience wrapper around updateProvisioning() for callers that only
     * need a yes/no answer.
     *
     * @return true if provisioning information is current (with or without update)
     */
    bool updateProvisioningOk(const std::string& apiPath = "provision")
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
    std::string getApiUrlForPath(const std::string& apiPath);

    /**
     * Send a request to the given URL and return the status code and response body.
     *
     * On HTTP 401, the device token is cleared; if a provisioning token is
     * available, the device is re-provisioned and the request is retried once.
     * On HTTP 403 (project inactive, device inactive or not approved), the
     * device token is kept - re-provisioning would not help in this case.
     */
    int apiRequest(std::string& oResponse, std::map<std::string, std::string>& oResponseHeader,
        const char * requestType, const std::string& apiPath, const std::string& requestBody = "",
        const std::map<std::string, std::string>& requestHeader = {},
        const std::vector<std::string>& collectResponseHeaderKeys = {});

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
    int apiGet(std::string& oResponse, const std::string& apiPath, const std::string& body = "", const std::map<std::string, std::string>& headers = {});

    /**
     * Send a GET request like the other apiGet() overload, additionally
     * returning selected response headers.
     *
     * @param oResponseHeader receives the collected response headers
     * @param collectResponseHeaderKeys the response header names to collect,
     *        e.g. {"ETag", "Last-Modified"}
     */
    int apiGet(std::string& oResponse, std::map<std::string, std::string>& oResponseHeader, const std::string& apiPath,
        const std::vector<std::string>& collectResponseHeaderKeys, const std::string& body = "", const std::map<std::string, std::string>& headers = {});

    /**
     * Send a HEAD request to the API using the given API path and return
     * the response status code.
     *
     * @return the HTTP response status code
     */
    int apiHead(const std::string& apiPath, const std::map<std::string, std::string>& headers = {});

    /**
     * Send a POST request to the given API path and return the response body
     * if successful (200 <= status code < 300). Return an empty string
     * otherwise. This method is similar to @see apiGet.
     *
     * The "Content-Type" header defaults to "application/json" if not specified.
     */
    int apiPost(std::string& oResponse, const std::string& apiPath, const std::string& body, const std::map<std::string, std::string>& headers = {});

    /**
     * Send a PUT request to the given API path and return the response body
     * if successful (200 <= status code < 300). This method is similar to
     * @see apiGet.
     */
    int apiPut(std::string& oResponse, const std::string& apiPath, const std::string& body, const std::map<std::string, std::string>& headers = {});

    // **********************************************************************
    // P r i v a t e
    // **********************************************************************

private:
    IotStore* _store = nullptr;
    IotHttp* _http = nullptr;
    IotClock* _clock = nullptr;
    std::string _boardId;

    /// Provisioning token, device token, expiry and the renewal decision.
    /// The transport below knows nothing about any of it.
    IotTokenManager _tokens;

    /// Kept for the object lifetime: the TLS stack stores the PEM pointers
    /// rather than copying them.
    IotTlsConfig _tls;
    bool _tlsTrustWarningLogged = false;

    std::string _baseUrl;
    std::map<std::string, std::string> _defaultRequestHeader;
    std::string _projectName;
    std::string _deviceName;

    /// Set the base URL, normalizing a trailing slash.
    void _setApiUrl(const std::string& apiBaseurl);

    /// Log one clear error if the endpoint is https but no TLS trust was
    /// seeded; without it the handshake fails with an opaque transport error.
    void _warnIfTlsTrustMissing();

    /// @return the merged header set for an ordinary JSON API request.
    /// @param authenticated add the device token as the Authorization header
    std::map<std::string, std::string> _requestHeaders(const std::map<std::string, std::string>& requestHeader,
        bool authenticated) const;

    /**
     * Execute a single HTTP request with the headers it is given. No token
     * knowledge, no 401 handling, no re-provisioning.
     * @see apiRequest(), which adds the authentication policy on top.
     */
    int _performRequest(std::string& oResponse, std::map<std::string, std::string>& oResponseHeader,
        const char * requestType, const std::string& apiPath, const std::string& requestBody,
        const std::map<std::string, std::string>& requestHeader,
        const std::vector<std::string>& collectResponseHeaderKeys);

    /**
     * Perform the /provision exchange and adopt the resulting token. Goes
     * through _performRequest(), not apiRequest(), so a 401 from /provision
     * itself is reported rather than triggering another attempt.
     */
    IotResult _requestDeviceToken(const std::string& apiPath);
};

// *****************************************************************************

extern IotApi api;
