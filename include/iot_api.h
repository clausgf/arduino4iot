/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

#include <ctime>
#include <map>
#include <string>
#include <vector>

#include "iot_cache.h"
#include "iot_ports.h"
#include "iot_result.h"
#include "iot_seed.h"
#include "iot_token.h"

// *****************************************************************************

/// A request to the API. Designated initializers name the fields at the call
/// site: `{ .method = "PUT", .path = "file/{project}/{device}/a.txt", .body = s }`.
struct IotApiRequest
{
    const char* method = "GET";
    /// Relative to the API base URL; {project}, {device} and {board} are substituted.
    std::string path;
    std::string body;
    /// Win over the library's and setApiHeader()'s; an empty value suppresses one.
    std::map<std::string, std::string> headers;
    /// Response headers to return. The transport can only capture headers it
    /// was asked for in advance.
    std::vector<std::string> collectHeaders;
};

/// The optional parts of a request, for get()/head()/post()/put().
struct IotApiOptions
{
    std::map<std::string, std::string> headers;
    std::vector<std::string> collectHeaders;
};

struct IotApiResponse
{
    /// Ok (2xx), HttpError or TransportError, @see IotResult.
    IotResult result;
    /// The response body; empty for HEAD and 304.
    std::string body;
    /// The collected headers, @see IotApiRequest::collectHeaders.
    std::map<std::string, std::string> headers;

    /// @return true if header @p name was received (case-insensitive).
    bool hasHeader(const char* name) const { return _find(name) != nullptr; }
    /// @return the value of header @p name (case-insensitive), "" if absent.
    std::string header(const char* name) const
    {
        const std::string* value = _find(name);
        return value != nullptr ? *value : std::string();
    }

private:
    const std::string* _find(const char* name) const;
};

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

    /// @return true between begin() and end(), i.e. when requests can be made.
    bool isInitialized() const { return _http != nullptr; }

    /// Hardware variant substituted for {board} in a path; empty if unset.
    void setBoardId(const std::string& boardId) { _boardId = boardId; }



    // **********************************************************************
    // API configuration
    // **********************************************************************

    /**
     * Set the device name, e.g. "my-device".
     *
     * The device name is replaced for {device} in the paths of API calls.
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
    void setConnectTimeout_ms(uint32_t timeout_ms);

    /**
     * Set a timeout for waiting for data in an HTTP request.
     * @param timeout_ms the timeout in milliseconds
     */
    void setRequestTimeout_ms(uint32_t timeout_ms);



    // **********************************************************************
    // Provisioning
    // **********************************************************************

    // Note: the provisioning token is seeded from a build-time default into NVS
    // via Iot::seedCredentials() and loaded by begin(); it is no longer set at
    // runtime.

    /**
     * Clear the API token. This is useful for forcing provisioning.
     */
    void clearDeviceToken();

    /**
     * @return the expiry time of the device token as unix timestamp or
     * 0 if unknown
     */
    int64_t deviceTokenExpiresAt() const { return _tokens.deviceTokenExpiresAt(); }

    /**
     * Set the margin before the device token expiry at which
     * updateProvisioning() proactively requests a new token.
     * The default is 3600 s. Choose a margin larger than your sleep
     * interval to avoid waking up with an expired token - but also smaller
     * than the lifetime the server issues, or every wakeup re-provisions.
     */
    void setDeviceTokenExpiryMargin_s(int32_t margin_s) { _tokens.setExpiryMargin_s(margin_s); }

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
    IotResult updateProvisioning(const std::string& apiPath = iot_paths::PROVISION);



    // **********************************************************************
    // HTTP requests
    // **********************************************************************

    /**
     * Send @p request and return status, body and the collected headers.
     *
     *   IotApiResponse r = api.send({ .method = "PATCH", .path = "x/{device}",
     *                                 .body = "{}", .collectHeaders = { "ETag" } });
     *
     * The URL is the base URL plus the path, with {project}, {device} and
     * {board} (IOT_BOARD_ID) substituted. Headers merge, later winning:
     * "Accept"/"Content-Type: application/json" and the device token, then
     * setApiHeader(), then request.headers; an empty value suppresses a header.
     *
     * On HTTP 401, the device token is cleared; if a provisioning token is
     * available, the device is re-provisioned and the request is retried once.
     * On HTTP 403 (project inactive, device inactive or not approved), the
     * device token is kept - re-provisioning would not help in this case.
     */
    IotApiResponse send(const IotApiRequest& request);

    /// GET @p apiPath, @see send().
    IotApiResponse get(const std::string& apiPath, const IotApiOptions& options = {});

    /// HEAD @p apiPath, @see send().
    IotApiResponse head(const std::string& apiPath, const IotApiOptions& options = {});

    /// POST @p body to @p apiPath, @see send(). Content-Type defaults to JSON.
    IotApiResponse post(const std::string& apiPath, const std::string& body, const IotApiOptions& options = {});

    /// PUT @p body to @p apiPath, @see send(). Content-Type defaults to JSON.
    IotApiResponse put(const std::string& apiPath, const std::string& body, const IotApiOptions& options = {});

    /**
     * GET @p apiPath, conditional on @p current (If-None-Match /
     * If-Modified-Since). Stores nothing: commit the returned validators once
     * the body was used, @see iot_cache.h.
     *
     *   IotCachedResponse r = api.getCached("forward/{project}/{device}/img/latest.png",
     *       { rtcEtag, "" }, { .headers = { { "Accept", "image/png" } } });
     *   if (r.result.isOk() && render(r.body)) { saveToRtc(r.validators.etag); }
     *
     * @param options extra headers; they win over the conditional ones
     */
    IotCachedResponse getCached(const std::string& apiPath, const IotCacheValidators& current,
        const IotApiOptions& options = {});

    // **********************************************************************
    // P r i v a t e
    // **********************************************************************

private:
    // --- for the OTA download, which opens its own connection ---
    friend struct IotOtaEndpoint;

    /// @return the store the API configuration and tokens live in.
    IotStore& store() { return *_store; }
    /// @return the device token, i.e. the Authorization header value.
    const std::string& deviceToken() const { return _tokens.deviceToken(); }
    /// @return the seeded TLS trust.
    const IotTlsConfig& tlsConfig() const { return _tls; }
    /// Merge @p base with the setApiHeader() defaults and @p requestHeader,
    /// later sources winning.
    std::map<std::string, std::string> mergeHeaders(std::map<std::string, std::string> base,
        const std::map<std::string, std::string>& requestHeader) const;
    /// @return the URL of @p apiPath: base URL plus the path with {project},
    /// {device} and {board} substituted.
    std::string urlForPath(const std::string& apiPath);

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
     * Execute a single HTTP request with @p headers, the fully merged set. No
     * token knowledge, no 401 handling, no re-provisioning.
     * @see send(), which adds the authentication policy on top.
     */
    IotApiResponse _perform(const char* method, const std::string& apiPath, const std::string& body,
        const std::map<std::string, std::string>& headers, const std::vector<std::string>& collectHeaders);

    /// send() over the parts, so get()/post()/... copy no header map.
    IotApiResponse _send(const char* method, const std::string& apiPath, const std::string& body,
        const std::map<std::string, std::string>& headers, const std::vector<std::string>& collectHeaders);

    /**
     * Perform the /provision exchange and adopt the resulting token. Goes
     * through _perform(), not send(), so a 401 from /provision
     * itself is reported rather than triggering another attempt.
     */
    IotResult _requestDeviceToken(const std::string& apiPath);
};

// *****************************************************************************

extern IotApi api;
