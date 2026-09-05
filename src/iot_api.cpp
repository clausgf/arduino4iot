/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#include "iot_api.h"

#include <ctime>

#include <ArduinoJson.h>

#include "iot_log.h"
#include "iot_text.h"

// *****************************************************************************

IotApi api;

// *****************************************************************************

IotApi::IotApi()
{
}

void IotApi::begin(IotStore& store, IotHttp& http, IotClock& clock)
{
    using namespace iot_nvs_keys;

    _store = &store;
    _http = &http;
    _clock = &clock;

    _setApiUrl(store.getString(API_URL, ""));
    _projectName = store.getString(PROJECT, "");
    _tls.mode = (IotTlsMode)store.getInt(TLS_MODE, (int64_t)IotTlsMode::None);
    _tls.caCertPem = store.getString(CA_CERT, "");
    _tls.clientCertPem = store.getString(CLIENT_CERT, "");
    _tls.clientKeyPem = store.getString(CLIENT_KEY, "");

    // the credentials load themselves from the same store
    _tokens.begin(store);

    // _tls outlives the http adapter's use of it: both live in this object's
    // and the application's lifetime
    _http->setTls(_tls);
}

void IotApi::end()
{
    _tokens.end();
    _http = nullptr;
    _store = nullptr;
}


// *****************************************************************************
// API configuration
// *****************************************************************************

void IotApi::_setApiUrl(const std::string& apiBaseurl)
{
    _baseUrl = apiBaseurl;
    if (!_baseUrl.empty() && _baseUrl.back() != '/')
    {
        _baseUrl += "/";
    }
}

void IotApi::setDeviceName(const std::string& device)
{
    _deviceName = device;
}

void IotApi::setApiHeader(const std::map<std::string, std::string>& header)
{
    _defaultRequestHeader = header;
}

void IotApi::_warnIfTlsTrustMissing()
{
    const bool isHttps = _baseUrl.rfind("https://", 0) == 0;
    if (isHttps && _http != nullptr && !_http->hasServerTrust() && !_tlsTrustWarningLogged)
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
    if (_http != nullptr)
    {
        _http->close();
    }
}

void IotApi::setConnectionTimeout_ms(int32_t timeout_ms)
{
    if (_http != nullptr) { _http->setConnectTimeout_ms(timeout_ms); }
}

void IotApi::setRequestTimeout_ms(uint16_t timeout_ms)
{
    if (_http != nullptr) { _http->setRequestTimeout_ms(timeout_ms); }
}


// *****************************************************************************
// Provisioning
// *****************************************************************************

// The provisioning token is seeded into NVS by Iot::seedCredentials() and loaded
// by begin(); it is no longer set at runtime.

void IotApi::setDeviceToken(const std::string& deviceToken, time_t expiresAt)
{
    _tokens.setDeviceToken(std::string(deviceToken.c_str()), (int64_t)expiresAt);
}

void IotApi::clearDeviceToken()
{
    _tokens.clearDeviceToken();
}

// *****************************************************************************

IotResult IotApi::updateProvisioning(const std::string& apiPath)
{
    // whether the current token still does the job is a decision, not I/O
    if (!_tokens.needsNewToken(_clock->now(), _clock->isPlausible()))
    {
        log_i("updateProvisioning: already provisioned, token still valid");
        return IotResult(200);
    }
    if (_tokens.hasDeviceToken())
    {
        log_i("updateProvisioning: device token expired or expiring soon, re-provisioning");
    }
    return _requestDeviceToken(apiPath);
}

IotResult IotApi::_requestDeviceToken(const std::string& apiPath)
{
    if (!_tokens.hasProvisioningToken())
    {
        log_e("updateProvisioning: no provisioning token available");
        return IotResult(IotResult::STATUS_NO_PROVISIONING_TOKEN);
    }

    JsonDocument requestDoc;
    requestDoc["projectName"] = _projectName;
    requestDoc["deviceName"] = _deviceName;
    requestDoc["provisioningToken"] = _tokens.provisioningToken();
    std::string request;
    serializeJson(requestDoc, request);

    // unauthenticated single-shot: /provision authenticates with the
    // provisioning token in the body
    std::string response;
    std::map<std::string, std::string> responseHeader;
    int httpStatusCode = _performRequest(response, responseHeader, "POST", apiPath, request,
        _requestHeaders({}, /*authenticated=*/false), {});

    if (httpStatusCode < 200 || httpStatusCode >= 300)
    {
        log_i("updateProvisioning: status=%d", httpStatusCode);
        return IotResult(httpStatusCode);
    }

    IotProvisioningResponse parsed;
    if (!iotParseProvisioningResponse(response.c_str(), parsed))
    {
        // 2xx, but nothing usable in the body
        log_i("updateProvisioning: malformed response body");
        return IotResult(IotResult::STATUS_MALFORMED_RESPONSE);
    }

    int64_t expiresAt = _tokens.applyProvisioningResponse(
        parsed, _clock->now(), _clock->isPlausible());
    if (expiresAt > 0)
    {
        log_i("updateProvisioning: new device token valid for %lld s",
            (long long)(expiresAt - _clock->now()));
    } else {
        log_i("updateProvisioning: new device token for api access, expiry unknown");
    }
    return IotResult(httpStatusCode);
}


// *****************************************************************************
// HTTP requests
// *****************************************************************************

std::string IotApi::getApiUrlForPath(const std::string& apiPath)
{
    return iot_text::buildApiUrl(
        _baseUrl.c_str(), apiPath.c_str(), _projectName.c_str(), _deviceName.c_str(),
        _boardId.c_str());
}

// *****************************************************************************

std::map<std::string, std::string> IotApi::mergeHeaders(std::map<std::string, std::string> base,
    const std::map<std::string, std::string>& requestHeader) const
{
    for (auto const& kv : _defaultRequestHeader) { base[kv.first] = kv.second; }
    for (auto const& kv : requestHeader) { base[kv.first] = kv.second; }
    return base;
}

std::map<std::string, std::string> IotApi::_requestHeaders(const std::map<std::string, std::string>& requestHeader,
    bool authenticated) const
{
    std::map<std::string, std::string> base = {
        { "Accept", "application/json" },
        { "Content-Type", "application/json" },
    };
    if (authenticated)
    {
        base["Authorization"] = _tokens.deviceToken();
    }
    return mergeHeaders(std::move(base), requestHeader);
}

// *****************************************************************************

int IotApi::_performRequest(std::string& oResponse, std::map<std::string, std::string>& oResponseHeader,
    const char * requestType, const std::string& apiPath, const std::string& requestBody,
    const std::map<std::string, std::string>& requestHeader,
    const std::vector<std::string>& collectResponseHeaderKeys)
{
    if (_http == nullptr)
    {
        log_e("IotApi not initialized - call begin() first");
        return -1;
    }

    IotHttpRequest request;
    request.method = requestType;
    request.url = getApiUrlForPath(apiPath);
    request.body = requestBody;
    request.headers = requestHeader;   // already the fully merged set
    request.collectResponseHeaders = collectResponseHeaderKeys;

    log_i("HTTP %s url=%s", requestType, request.url.c_str());
    _warnIfTlsTrustMissing();

    IotHttpResponse response;
    const int httpStatusCode = _http->send(request, response);
    oResponse = std::move(response.body);
    oResponseHeader = std::move(response.headers);

    if (httpStatusCode < 0)
    {
        // a missing TLS server trust is a common, hard-to-attribute cause of
        // transport errors on https - point at it explicitly
        const char *tlsHint = (!_http->hasServerTrust() && request.url.rfind("https://", 0) == 0)
            ? " (no TLS server trust seeded: set tlsMode via iot.seedCredentials())"
            : "";
        log_e("HTTP %s url=%s -> status=%d error=%s%s",
            requestType, request.url.c_str(), httpStatusCode,
            _http->errorToString(httpStatusCode).c_str(), tlsHint);
    } else if (httpStatusCode == 401) {
        log_e("HTTP %s url=%s -> status=401 UNAUTHORIZED - device token invalid or expired",
            requestType, request.url.c_str());
    } else if (httpStatusCode == 403) {
        // 403 is not an auth-token problem (nice4iot returns 401 for those on the
        // device endpoints) - re-provisioning would not help, so keep the token
        log_e("HTTP %s url=%s -> status=403 FORBIDDEN - access refused by the server",
            requestType, request.url.c_str());
    } else if (httpStatusCode < 200 || httpStatusCode >= 400) {
        log_e("HTTP %s url=%s requestBody=%s -> status=%d responseBody=%s",
            requestType, request.url.c_str(), requestBody.c_str(), httpStatusCode, oResponse.c_str());
    } else {
        log_i("HTTP %s url=%s -> status=%d", requestType, request.url.c_str(), httpStatusCode);
    }
    return httpStatusCode;
}

// *****************************************************************************

// *****************************************************************************

int IotApi::apiRequest(std::string& oResponse, std::map<std::string, std::string>& oResponseHeader,
    const char * requestType, const std::string& apiPath, const std::string& requestBody,
    const std::map<std::string, std::string>& requestHeader,
    const std::vector<std::string>& collectResponseHeaderKeys)
{
    // the authentication policy on top of the transport: attach the device
    // token, and on a rejection get a new one and try once more
    int httpStatusCode = _performRequest(oResponse, oResponseHeader,
        requestType, apiPath, requestBody,
        _requestHeaders(requestHeader, /*authenticated=*/true), collectResponseHeaderKeys);

    if (httpStatusCode == 401 && _tokens.hasProvisioningToken())
    {
        _tokens.clearDeviceToken();
        if (_requestDeviceToken("provision").isOk())
        {
            log_i("HTTP %s url=%s retrying after re-provisioning", requestType, apiPath.c_str());
            oResponse = "";
            oResponseHeader.clear();
            // rebuild the headers: they now carry the freshly issued token
            httpStatusCode = _performRequest(oResponse, oResponseHeader,
                requestType, apiPath, requestBody,
                _requestHeaders(requestHeader, /*authenticated=*/true), collectResponseHeaderKeys);
        }
    }
    return httpStatusCode;
}

// *****************************************************************************

int IotApi::apiGet(std::string& response, const std::string& apiPath, const std::string& body, const std::map<std::string, std::string>& header)
{
    std::map<std::string, std::string> responseHeader;
    return apiRequest(response, responseHeader, "GET", apiPath, body, header);
}

int IotApi::apiGet(std::string& response, std::map<std::string, std::string>& oResponseHeader, const std::string& apiPath,
    const std::vector<std::string>& collectResponseHeaderKeys, const std::string& body, const std::map<std::string, std::string>& header)
{
    return apiRequest(response, oResponseHeader, "GET", apiPath, body, header, collectResponseHeaderKeys);
}

// *****************************************************************************

int IotApi::apiHead(const std::string& apiPath, const std::map<std::string, std::string>& header)
{
    std::string response;
    std::map<std::string, std::string> responseHeader;
    return apiRequest(response, responseHeader, "HEAD", apiPath, "", header);
}

// *****************************************************************************

int IotApi::apiPost(std::string& response, const std::string& apiPath, const std::string& body, const std::map<std::string, std::string>& header)
{
    std::map<std::string, std::string> responseHeader;
    return apiRequest(response, responseHeader, "POST", apiPath, body, header);
}

// *****************************************************************************

int IotApi::apiPut(std::string& response, const std::string& apiPath, const std::string& body, const std::map<std::string, std::string>& header)
{
    std::map<std::string, std::string> responseHeader;
    return apiRequest(response, responseHeader, "PUT", apiPath, body, header);
}

// *****************************************************************************

