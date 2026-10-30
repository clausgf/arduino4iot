/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#include "iot_config.h"

#include <cctype>
#include <map>

#include "iot_api.h"
#include "iot_log.h"

// *****************************************************************************

IotConfig config;

// *****************************************************************************

IotConfigValueBase::IotConfigValueBase(IotConfig& owner, const char* key):
    _key(key), _owner(&owner), _next(iotConfigValueListHead()), _overridden(false)
{
    iotConfigValueListHead() = this;
}

IotConfigValueBase::~IotConfigValueBase()
{
    for (IotConfigValueBase** link = &iotConfigValueListHead();
         *link != nullptr; link = &(*link)->_next)
    {
        if (*link == this) { *link = _next; return; }
    }
}

// *****************************************************************************

void IotConfig::begin(IotStore& store, const char* apiPath)
{
    _store = &store;
    _apiPath = apiPath;

    // a key that does not fit the store would silently never be stored
    for (IotConfigValueBase* v = iotConfigValueListHead(); v != nullptr; v = v->next())
    {
        if (v->owner() == this && !IotStore::isValidKey(v->key()))
        {
            log_e("IotConfig: key '%s' exceeds %u characters and cannot be stored",
                v->key(), (unsigned)IotStore::MAX_KEY_LENGTH);
        }
    }

    reload();
    log_i("--- Config etag=%s date=%s", etag().c_str(), lastModified().c_str());
}

// *****************************************************************************

void IotConfig::reload()
{
    if (_store == nullptr) { return; }
    for (IotConfigValueBase* v = iotConfigValueListHead(); v != nullptr; v = v->next())
    {
        if (v->owner() == this) { v->loadFrom(*_store); }
    }
}

// *****************************************************************************

IotConfigValueBase* IotConfig::_find(const char* key) const
{
    for (IotConfigValueBase* v = iotConfigValueListHead(); v != nullptr; v = v->next())
    {
        if (v->owner() == this && std::string(v->key()) == key) { return v; }
    }
    return nullptr;
}

// *****************************************************************************

int IotConfig::applyJsonBody(const char* body, int* oUnknown, int* oMismatched)
{
    if (_store == nullptr) { return -1; }
    JsonDocument doc;
    if (deserializeJson(doc, body)) { return -1; }
    JsonObjectConst root = doc.as<JsonObjectConst>();
    if (root.isNull()) { return -1; }

    int applied = 0, unknown = 0, mismatched = 0;
    for (JsonPairConst kv : root)
    {
        IotConfigValueBase* target = _find(kv.key().c_str());
        if (target == nullptr) { unknown++; continue; }
        if (target->applyJson(kv.value(), *_store)) { applied++; } else { mismatched++; }
    }
    if (oUnknown != nullptr) { *oUnknown = unknown; }
    if (oMismatched != nullptr) { *oMismatched = mismatched; }
    return applied;
}

// *****************************************************************************

IotResult IotConfig::updateConfig()
{
    if (_store == nullptr || _apiPath == nullptr)
    {
        log_e("IotConfig not initialized - call begin() first");
        return IotResult();  // default: transport error, no request was made
    }

    std::string response;
    std::map<std::string, std::string> responseHeader;
    int httpStatusCode = api.apiRequest(
        response, responseHeader, "GET", _apiPath, "",
        { { "If-None-Match", etag() },
          { "If-Modified-Since", lastModified() } },
        { "ETag", "Last-Modified" });

    if (httpStatusCode < 200 || httpStatusCode >= 300)
    {
        if (httpStatusCode == 304)
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
        // the header name comes back as the server spelled it
        std::string name = kv.first;
        for (char& c : name) { c = (char)tolower((unsigned char)c); }
        if (name == "etag")
        {
            _store->putString(KEY_ETAG, kv.second);
        } else if (name == "last-modified")
        {
            _store->putString(KEY_DATE, kv.second);
        }
    }

    log_i("Configuration data update finished (%d values)", applied);
    return IotResult(200);
}
