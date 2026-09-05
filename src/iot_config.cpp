/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#include "iot_config.h"

#include <map>

#include "iot_api.h"
#include "iot_esp_ports.h"

// *****************************************************************************

IotConfig config;

/// The library's default configuration namespace. A function-local static, so
/// it is constructed on first use rather than in an unspecified order relative
/// to the global config object.
static IotStore& defaultConfigStore()
{
    static IotNvsStore store("iot-cfg");
    store.begin();
    return store;
}

// *****************************************************************************

void IotConfig::begin(const char* apiPath)
{
    begin(defaultConfigStore(), apiPath);

    // a key that does not fit NVS would silently never be stored
    for (IotConfigValueBase* v = iotConfigValueListHead(); v != nullptr; v = v->next())
    {
        if (v->owner() == this && !IotStore::isValidKey(v->key()))
        {
            log_e("IotConfig: key '%s' exceeds %u characters and cannot be stored",
                v->key(), (unsigned)IotStore::MAX_KEY_LENGTH);
        }
    }
    log_i("--- Config etag=%s date=%s", etag().c_str(), lastModified().c_str());
}

// *****************************************************************************

IotResult IotConfig::updateConfig()
{
    if (_store == nullptr || _apiPath == nullptr)
    {
        log_e("IotConfig not initialized - call begin() first");
        return IotResult();  // default: transport error, no request was made
    }

    String response;
    std::map<String, String> responseHeader;
    int httpStatusCode = api.apiRequest(
        response, responseHeader, "GET", _apiPath, "",
        { { "If-None-Match", String(etag().c_str()) },
          { "If-Modified-Since", String(lastModified().c_str()) } },
        { "ETag", "Last-Modified" });

    if (httpStatusCode < 200 || httpStatusCode >= 300)
    {
        if (httpStatusCode == HTTP_CODE_NOT_MODIFIED)
        {
            log_i("Configuration data not modified");
        } else {
            log_e("HTTP GET failed status=%d", httpStatusCode);
        }
        return IotResult(httpStatusCode);
    }

    int unknown = 0;
    int mismatched = 0;
    int applied = applyJsonBody(response.c_str(), &unknown, &mismatched);
    if (applied < 0)
    {
        log_e("Config JSON deserialization failed");
        return IotResult(IotResult::STATUS_MALFORMED_RESPONSE);
    }
    if (unknown > 0 || mismatched > 0)
    {
        log_w("Config: %d applied, %d unknown key(s), %d type mismatch(es)",
            applied, unknown, mismatched);
    }

    for (auto const& kv : responseHeader)
    {
        if (kv.first.equalsIgnoreCase("etag"))
        {
            _store->putString(KEY_ETAG, std::string(kv.second.c_str()));
        } else if (kv.first.equalsIgnoreCase("last-modified"))
        {
            _store->putString(KEY_DATE, std::string(kv.second.c_str()));
        }
    }

    log_i("Configuration data update finished (%d values)", applied);
    return IotResult(HTTP_CODE_OK);
}
