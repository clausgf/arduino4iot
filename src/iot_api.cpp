/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#include "iot_api.h"

#include <cctype>
#include <cstdio>
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

    if (_connectTimeout_ms > 0) { _http->setConnectTimeout_ms(_connectTimeout_ms); }
    if (_requestTimeout_ms > 0) { _http->setRequestTimeout_ms(_requestTimeout_ms); }
    if (_handshakeTimeout_ms > 0) { _http->setHandshakeTimeout_ms(_handshakeTimeout_ms); }
    if (_responseTimeout_ms > 0) { _http->setResponseTimeout_ms(_responseTimeout_ms); }
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

void IotApi::setConnectTimeout_ms(uint32_t timeout_ms)
{
    _connectTimeout_ms = timeout_ms;
    if (_http != nullptr && timeout_ms > 0) { _http->setConnectTimeout_ms(timeout_ms); }
}

void IotApi::setRequestTimeout_ms(uint32_t timeout_ms)
{
    _requestTimeout_ms = timeout_ms;
    if (_http != nullptr && timeout_ms > 0) { _http->setRequestTimeout_ms(timeout_ms); }
}

void IotApi::setHandshakeTimeout_ms(uint32_t timeout_ms)
{
    _handshakeTimeout_ms = timeout_ms;
    if (_http != nullptr && timeout_ms > 0) { _http->setHandshakeTimeout_ms(timeout_ms); }
}

void IotApi::setResponseTimeout_ms(uint32_t timeout_ms)
{
    _responseTimeout_ms = timeout_ms;
    if (_http != nullptr && timeout_ms > 0) { _http->setResponseTimeout_ms(timeout_ms); }
}

std::string IotApi::errorText(const IotResult& result) const
{
    char text[64];
    switch (result.kind)
    {
        case IotResult::Ok:
            return std::string();
        case IotResult::HttpError:
            switch (result.httpStatus)
            {
                case IotResult::STATUS_NO_PROVISIONING_TOKEN: return "no provisioning token";
                case IotResult::STATUS_MALFORMED_RESPONSE:    return "malformed response";
                case IotResult::STATUS_UPDATE_FAILED:         return "update failed";
                case IotResult::STATUS_NOT_CONFIGURED:        return "not configured";
                default: break;
            }
            snprintf(text, sizeof(text), "HTTP %d", result.httpStatus);
            return text;
        case IotResult::TransportError:
            break;
    }

    // a failure while setting up the connection is described by its phase:
    // the adapters report all of them with one generic code (HTTPClient's
    // "connection refused"), which would be wrong for a timeout or a TLS error
    std::string out;
    switch (result.phase)
    {
        case IotTransportPhase::Dns:
            out = "server name not resolved";
            break;
        case IotTransportPhase::Connect:
            out = "connect failed or timed out";
            break;
        case IotTransportPhase::Tls:
            // without a TLS code, the adapter recognized a handshake timeout
            out = result.tlsError != 0 ? "TLS handshake failed" : "TLS handshake timed out";
            break;
        case IotTransportPhase::Send:
        case IotTransportPhase::Receive:
        case IotTransportPhase::None:
            out = _http != nullptr
                ? _http->errorToString(result.transportError)
                : "transport error " + std::to_string(result.transportError);
            if (result.phase != IotTransportPhase::None)
            {
                out += " (";
                out += iotTransportPhaseToString(result.phase);
                out += ")";
            }
            break;
    }
    if (result.tlsError != 0)
    {
        // mbedTLS documents its codes in hex
        snprintf(text, sizeof(text), " (TLS -0x%04X", (unsigned)-result.tlsError);
        out += text;
        const std::string tlsText = _http != nullptr ? _http->tlsErrorToString(result.tlsError) : "";
        if (!tlsText.empty()) { out += ": " + tlsText; }
        out += ")";
    }
    if (result.elapsed_ms > 0)
    {
        snprintf(text, sizeof(text), " after %lu ms", (unsigned long)result.elapsed_ms);
        out += text;
    }
    return out;
}


// *****************************************************************************
// Provisioning
// *****************************************************************************

// The provisioning token is seeded into NVS by Iot::seedCredentials() and loaded
// by begin(); it is no longer set at runtime.

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
    IotApiResponse response = _perform("POST", apiPath, request,
        _requestHeaders({}, /*authenticated=*/false), {});

    if (!response.result.isOk())
    {
        log_i("updateProvisioning: status=%d", response.result.httpStatus);
        return response.result;
    }

    IotProvisioningResponse parsed;
    if (!iotParseProvisioningResponse(response.body.c_str(), parsed))
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
    return response.result;
}


// *****************************************************************************
// HTTP requests
// *****************************************************************************

std::string IotApi::urlForPath(const std::string& apiPath)
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

IotApiResponse IotApi::_perform(const char* method, const std::string& apiPath, const std::string& body,
    const std::map<std::string, std::string>& headers, const std::vector<std::string>& collectHeaders)
{
    IotApiResponse out;
    if (_http == nullptr)
    {
        log_e("IotApi not initialized - call begin() first");
        out.result = IotResult(IotResult::STATUS_NOT_CONFIGURED);
        return out;
    }
    if (_baseUrl.empty())
    {
        log_e("HTTP %s %s: no API URL seeded - set apiUrl via iot.seedCredentials()",
            method, apiPath.c_str());
        out.result = IotResult(IotResult::STATUS_NOT_CONFIGURED);
        return out;
    }

    IotHttpRequest request;
    request.method = method;
    request.url = urlForPath(apiPath);
    request.body = body;
    request.headers = headers;
    request.collectResponseHeaders = collectHeaders;

    log_i("HTTP %s url=%s", method, request.url.c_str());
    _warnIfTlsTrustMissing();

    IotHttpResponse response;
    const int httpStatusCode = _http->send(request, response);
    out.result = IotResult(httpStatusCode);
    out.result.elapsed_ms = response.elapsed_ms;
    if (out.result.isTransportError())
    {
        out.result.phase = response.phase;
        out.result.tlsError = response.tlsError;
    }
    out.body = std::move(response.body);
    out.headers = std::move(response.headers);

    if (httpStatusCode < 0)
    {
        // a missing TLS server trust is a common, hard-to-attribute cause of
        // transport errors on https - point at it explicitly
        const char *tlsHint = (!_http->hasServerTrust() && request.url.rfind("https://", 0) == 0)
            ? " (no TLS server trust seeded: set tlsMode via iot.seedCredentials())"
            : "";
        log_e("HTTP %s url=%s -> status=%d error=%s%s",
            method, request.url.c_str(), httpStatusCode,
            errorText(out.result).c_str(), tlsHint);
    } else if (httpStatusCode == 401) {
        log_e("HTTP %s url=%s -> status=401 UNAUTHORIZED - device token invalid or expired",
            method, request.url.c_str());
    } else if (httpStatusCode == 403) {
        // 403 is not an auth-token problem (nice4iot returns 401 for those on the
        // device endpoints) - re-provisioning would not help, so keep the token
        log_e("HTTP %s url=%s -> status=403 FORBIDDEN - access refused by the server",
            method, request.url.c_str());
    } else if (httpStatusCode < 200 || httpStatusCode >= 400) {
        // the response body only, shortened: it usually explains the error,
        // while the request body can carry secrets (the provisioning token)
        // and be up to the server's size limit
        const int MAX_LOGGED_BODY = 200;
        const int shown = out.body.length() > (size_t)MAX_LOGGED_BODY ? MAX_LOGGED_BODY : (int)out.body.length();
        log_e("HTTP %s url=%s -> status=%d responseBody=%.*s%s",
            method, request.url.c_str(), httpStatusCode, shown, out.body.c_str(),
            out.body.length() > (size_t)shown ? "..." : "");
    } else {
        log_i("HTTP %s url=%s -> status=%d", method, request.url.c_str(), httpStatusCode);
    }
    return out;
}

// *****************************************************************************

IotApiResponse IotApi::send(const IotApiRequest& request)
{
    return _send(request.method, request.path, request.body, request.headers, request.collectHeaders);
}

IotApiResponse IotApi::_send(const char* method, const std::string& apiPath, const std::string& body,
    const std::map<std::string, std::string>& headers, const std::vector<std::string>& collectHeaders)
{
    // the authentication policy on top of the transport: attach the device
    // token, and on a rejection get a new one and try once more
    IotApiResponse response = _perform(method, apiPath, body,
        _requestHeaders(headers, /*authenticated=*/true), collectHeaders);

    if (response.result.httpStatus == 401 && _tokens.hasProvisioningToken())
    {
        _tokens.clearDeviceToken();
        if (_requestDeviceToken(iot_paths::PROVISION).isOk())
        {
            log_i("HTTP %s url=%s retrying after re-provisioning", method, apiPath.c_str());
            // rebuild the headers: they now carry the freshly issued token
            response = _perform(method, apiPath, body,
                _requestHeaders(headers, /*authenticated=*/true), collectHeaders);
        }
    }
    return response;
}

IotApiResponse IotApi::get(const std::string& apiPath, const IotApiOptions& options)
{
    return _send("GET", apiPath, "", options.headers, options.collectHeaders);
}

IotApiResponse IotApi::head(const std::string& apiPath, const IotApiOptions& options)
{
    return _send("HEAD", apiPath, "", options.headers, options.collectHeaders);
}

IotApiResponse IotApi::post(const std::string& apiPath, const std::string& body, const IotApiOptions& options)
{
    return _send("POST", apiPath, body, options.headers, options.collectHeaders);
}

IotApiResponse IotApi::put(const std::string& apiPath, const std::string& body, const IotApiOptions& options)
{
    return _send("PUT", apiPath, body, options.headers, options.collectHeaders);
}

// *****************************************************************************

static std::string lowercase(const char* s)
{
    std::string out(s);
    for (char& c : out) { c = (char)tolower((unsigned char)c); }
    return out;
}

const std::string* IotApiResponse::_find(const char* name) const
{
    // header names are case-insensitive, and an adapter may hand them back as
    // requested or as the server spelled them
    const std::string wanted = lowercase(name);
    for (auto const& kv : headers)
    {
        if (lowercase(kv.first.c_str()) == wanted) { return &kv.second; }
    }
    return nullptr;
}
