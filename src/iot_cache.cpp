/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#include "iot_cache.h"

#include <cctype>
#include <cstdlib>

#include "iot_log.h"

// *****************************************************************************

static std::string lowercase(std::string s)
{
    for (char& c : s) { c = (char)tolower((unsigned char)c); }
    return s;
}

static std::string trim(const std::string& s)
{
    size_t begin = s.find_first_not_of(" \t");
    if (begin == std::string::npos) { return std::string(); }
    size_t end = s.find_last_not_of(" \t");
    return s.substr(begin, end - begin + 1);
}

// *****************************************************************************

int32_t iotParseMaxAge(const std::string& cacheControl)
{
    int32_t maxAge = -1;
    size_t pos = 0;
    while (pos <= cacheControl.size())
    {
        size_t comma = cacheControl.find(',', pos);
        if (comma == std::string::npos) { comma = cacheControl.size(); }
        const std::string directive = lowercase(trim(cacheControl.substr(pos, comma - pos)));
        pos = comma + 1;

        if (directive == "no-cache" || directive == "no-store")
        {
            return -1;
        }
        if (directive.compare(0, 8, "max-age=") != 0)
        {
            continue;
        }
        std::string value = directive.substr(8);
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
        {
            value = value.substr(1, value.size() - 2);
        }
        if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
        {
            return -1;
        }
        long long seconds = strtoll(value.c_str(), nullptr, 10);
        maxAge = seconds > INT32_MAX ? INT32_MAX : (int32_t)seconds;
    }
    return maxAge;
}

// *****************************************************************************

IotCachedResponse iotGetCached(const std::string& apiPath, const IotCacheValidators& current,
    const std::map<std::string, std::string>& header, IotApi& via)
{
    std::map<std::string, std::string> request;
    if (!current.etag.empty()) { request["If-None-Match"] = current.etag; }
    if (!current.lastModified.empty()) { request["If-Modified-Since"] = current.lastModified; }
    for (auto const& kv : header) { request[kv.first] = kv.second; }

    IotApiResponse response = via.get(apiPath,
        { .headers = request, .collectHeaders = { "ETag", "Last-Modified", "Cache-Control" } });

    IotCachedResponse out;
    out.result = response.result;
    if (out.result.isNotModified())
    {
        // a 304 may resend the validators; whatever it leaves out stays as is
        out.validators = current;
        if (response.hasHeader("ETag")) { out.validators.etag = response.header("ETag"); }
        if (response.hasHeader("Last-Modified")) { out.validators.lastModified = response.header("Last-Modified"); }
    } else if (out.result.isOk())
    {
        // a new copy: validators the server does not send describe nothing
        out.body = std::move(response.body);
        out.validators = { response.header("ETag"), response.header("Last-Modified") };
    } else {
        log_w("Conditional GET %s failed status=%d", apiPath.c_str(),
            out.result.isTransportError() ? out.result.transportError : out.result.httpStatus);
        out.validators = current;
        return out;
    }

    if (response.hasHeader("Cache-Control")) { out.maxAge_s = iotParseMaxAge(response.header("Cache-Control")); }
    return out;
}

// *****************************************************************************

IotCacheValidators IotStoredValidators::load() const
{
    return IotCacheValidators{
        _store.getString(_etagKey, ""),
        _store.getString(_lastModifiedKey, ""),
    };
}

void IotStoredValidators::commit(const IotCacheValidators& validators)
{
    // IotStore suppresses writes of an unchanged value
    _store.putString(_etagKey, validators.etag);
    _store.putString(_lastModifiedKey, validators.lastModified);
}
