/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#include "iot_config.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "iot_api.h"
#include "iot_endpoints.h"
#include "iot_log.h"

// *****************************************************************************

IotConfig config;

// *****************************************************************************

IotConfigValueBase::IotConfigValueBase(IotConfig& owner, const char* key, Kind kind,
    const IotConfigMeta& meta):
    _key(key), _owner(&owner), _next(_listHead()), _meta(meta), _kind(kind),
    _overridden(false)
{
    _listHead() = this;
}

// *****************************************************************************
// IotConfigMeta
// *****************************************************************************

bool IotConfigMeta::allows(const std::string& value) const
{
    if (enumValues == nullptr) { return true; }
    const char* p = enumValues;
    while (true)
    {
        const char* end = strchr(p, '|');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (value.size() == len && value.compare(0, len, p, len) == 0) { return true; }
        if (end == nullptr) { return false; }
        p = end + 1;
    }
}

// *****************************************************************************
// The typed values
// *****************************************************************************

/// The stored or received int, limited to the declared range.
static int32_t constrained(const IotConfigValueBase& v, int32_t value)
{
    int32_t limited = v.meta().clamp(value);
    if (limited != value)
    {
        log_w("Config: %s=%d is outside [%d, %d], using %d", v.key(), value,
            v.meta().min, v.meta().max, limited);
    }
    return limited;
}

template <>
void IotConfigValue<int32_t>::loadFrom(const IotStore& store)
{
    if (!_overridden) { _value = constrained(*this, (int32_t)store.getInt(_key, _value)); }
}

template <>
bool IotConfigValue<int32_t>::applyJson(JsonVariantConst value, IotStore& store)
{
    if (!value.is<int32_t>() || value.is<bool>()) { return false; }
    const int32_t limited = constrained(*this, value.as<int32_t>());
    store.putInt(_key, limited);
    if (!_overridden) { _value = limited; }
    return true;
}

template <>
void IotConfigValue<bool>::loadFrom(const IotStore& store)
{
    if (!_overridden) { _value = store.getBool(_key, _value); }
}

template <>
bool IotConfigValue<bool>::applyJson(JsonVariantConst value, IotStore& store)
{
    if (!value.is<bool>()) { return false; }
    store.putBool(_key, value.as<bool>());
    if (!_overridden) { _value = value.as<bool>(); }
    return true;
}

template <>
void IotConfigValue<std::string>::loadFrom(const IotStore& store)
{
    if (_overridden) { return; }
    std::string stored = store.getString(_key, _value);
    if (_meta.allows(stored))
    {
        _value = std::move(stored);
    } else {
        log_w("Config: %s='%s' is not one of %s, keeping '%s'", _key, stored.c_str(),
            _meta.enumValues, _value.c_str());
    }
}

template <>
bool IotConfigValue<std::string>::applyJson(JsonVariantConst value, IotStore& store)
{
    if (!value.is<const char*>()) { return false; }
    std::string text = value.as<const char*>();
    if (!_meta.allows(text))
    {
        log_w("Config: %s='%s' is not one of %s, ignored", _key, text.c_str(), _meta.enumValues);
        return false;
    }
    store.putString(_key, text);
    if (!_overridden) { _value = std::move(text); }
    return true;
}

IotConfigValueBase::~IotConfigValueBase()
{
    for (IotConfigValueBase** link = &_listHead();
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
    for (IotConfigValueBase* v = IotConfigValueBase::_listHead(); v != nullptr; v = v->_next)
    {
        if (v->_owner == this && !IotStore::isValidKey(v->key()))
        {
            log_e("IotConfig: key '%s' exceeds %u characters and cannot be stored",
                v->key(), (unsigned)IotStore::MAX_KEY_LENGTH);
        }
    }

    _reload();
    const IotCacheValidators mirrored = validators();
    log_i("--- Config etag=%s date=%s", mirrored.etag.c_str(), mirrored.lastModified.c_str());
}

// *****************************************************************************

void IotConfig::_reload()
{
    if (_store == nullptr) { return; }
    for (IotConfigValueBase* v = IotConfigValueBase::_listHead(); v != nullptr; v = v->_next)
    {
        if (v->_owner == this) { v->loadFrom(*_store); }
    }
}

// *****************************************************************************

IotCacheValidators IotConfig::validators() const
{
    if (_store == nullptr) { return IotCacheValidators{}; }
    return IotStoredValidators(*_store, iot_nvs_keys::CONFIG_ETAG, iot_nvs_keys::CONFIG_DATE).load();
}

// *****************************************************************************

IotConfigValueBase* IotConfig::_find(const char* key) const
{
    for (IotConfigValueBase* v = IotConfigValueBase::_listHead(); v != nullptr; v = v->_next)
    {
        if (v->_owner == this && std::string(v->key()) == key) { return v; }
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
        return IotResult(IotResult::STATUS_NOT_CONFIGURED);
    }

    IotStoredValidators cache(*_store, iot_nvs_keys::CONFIG_ETAG, iot_nvs_keys::CONFIG_DATE);
    IotCachedResponse response = api.getCached(_apiPath, cache.load());
    if (response.result.isNotModified())
    {
        log_i("Configuration data not modified");
        cache.commit(response.validators);
        return response.result;
    }
    if (!response.result.isOk())
    {
        return response.result;
    }

    int unknown = 0;
    int mismatched = 0;
    int applied = applyJsonBody(response.body.c_str(), &unknown, &mismatched);
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

    cache.commit(response.validators);
    log_i("Configuration data update finished (%d values)", applied);
    return response.result;
}

// *****************************************************************************
// Schema
// *****************************************************************************

std::string IotConfig::schemaJson() const
{
    JsonDocument doc;
    doc["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    doc["type"] = "object";
    JsonObject properties = doc["properties"].to<JsonObject>();

    // the list is in reverse registration order; a schema reads better in
    // declaration order, so collect first
    std::vector<const IotConfigValueBase*> values;
    for (const IotConfigValueBase* v = IotConfigValueBase::_listHead(); v != nullptr; v = v->_next)
    {
        if (v->_owner == this) { values.insert(values.begin(), v); }
    }

    for (const IotConfigValueBase* v : values)
    {
        JsonObject spec = properties[v->key()].to<JsonObject>();
        const IotConfigMeta& meta = v->meta();
        if (meta.title != nullptr) { spec["title"] = meta.title; }
        if (meta.description != nullptr) { spec["description"] = meta.description; }

        switch (v->kind())
        {
            case IotConfigValueBase::Kind::Int:
                spec["type"] = "integer";
                spec["default"] = static_cast<const IotConfigValue<int32_t>*>(v)->defaultValue();
                if (meta.hasMin()) { spec["minimum"] = meta.min; }
                if (meta.hasMax()) { spec["maximum"] = meta.max; }
                break;
            case IotConfigValueBase::Kind::Bool:
                spec["type"] = "boolean";
                spec["default"] = static_cast<const IotConfigValue<bool>*>(v)->defaultValue();
                break;
            case IotConfigValueBase::Kind::String:
            {
                spec["type"] = "string";
                spec["default"] = static_cast<const IotConfigValue<std::string>*>(v)->defaultValue();
                if (meta.enumValues != nullptr)
                {
                    JsonArray allowed = spec["enum"].to<JsonArray>();
                    const char* p = meta.enumValues;
                    while (true)
                    {
                        const char* end = strchr(p, '|');
                        allowed.add(end ? std::string(p, end - p) : std::string(p));
                        if (end == nullptr) { break; }
                        p = end + 1;
                    }
                }
                break;
            }
        }
    }

    if (!_layout.empty())
    {
        JsonArray layout = doc["x-ui"]["layout"].to<JsonArray>();
        std::vector<std::string> placed;
        for (const auto& row : _layout)
        {
            JsonArray out = layout.add<JsonArray>();
            for (const char* key : row) { out.add(key); placed.push_back(key); }
        }
        // the library's own values, grouped, after the application's rows
        static const std::vector<std::vector<const char*>> LIBRARY_ROWS = {
            { iot_config_keys::LOG_LEVEL, iot_config_keys::SLEEP },
            { iot_config_keys::BATTERY_MIN },
            { iot_config_keys::PANIC_INIT, iot_config_keys::PANIC_FACTOR, iot_config_keys::PANIC_MAX },
        };
        for (const auto& row : LIBRARY_ROWS)
        {
            JsonArray out;
            for (const char* key : row)
            {
                bool isPlaced = false;
                for (const std::string& p : placed) { if (p == key) { isPlaced = true; } }
                if (isPlaced || _find(key) == nullptr) { continue; }
                if (out.isNull()) { out = layout.add<JsonArray>(); }
                out.add(key);
            }
        }
    }

    if (!_schemaExtras.empty())
    {
        JsonDocument extras;
        if (deserializeJson(extras, _schemaExtras) || !extras.is<JsonObjectConst>())
        {
            log_w("Config: schema extras are not a JSON object, ignored");
        } else {
            for (JsonPairConst kv : extras.as<JsonObjectConst>())
            {
                const char* key = kv.key().c_str();
                if (strcmp(key, "properties") == 0 || strcmp(key, "type") == 0
                    || strcmp(key, "$schema") == 0)
                {
                    continue;
                }
                doc[key] = kv.value();
            }
        }
    }

    std::string out;
    serializeJsonPretty(doc, out);
    return out;
}

void IotConfig::setLayout(std::initializer_list<LayoutRow> rows)
{
    _layout.clear();
    for (const LayoutRow& row : rows)
    {
        std::vector<const char*> keys;
        for (const IotConfigValueBase& value : row) { keys.push_back(value.key()); }
        _layout.push_back(std::move(keys));
    }
}

/// FNV-1a: a schema changes rarely and is not adversarial, so a short hash
/// that needs no library is enough to tell "already published".
static std::string schemaHash(const std::string& text)
{
    uint32_t hash = 2166136261u;
    for (unsigned char c : text) { hash = (hash ^ c) * 16777619u; }
    char hex[9];
    snprintf(hex, sizeof(hex), "%08x", (unsigned)hash);
    return hex;
}

IotResult IotConfig::publishSchema(const char* filename)
{
    if (_store == nullptr)
    {
        log_e("IotConfig not initialized - call begin() first");
        return IotResult(IotResult::STATUS_NOT_CONFIGURED);
    }
    const std::string schema = schemaJson();
    const std::string hash = schemaHash(schema);
    if (_store->getString(iot_nvs_keys::CONFIG_SCHEMA_HASH, "") == hash)
    {
        return IotResult(304);
    }

    IotResult result = api.put(std::string(iot_paths::FILE_DIR) + filename, schema,
        { .headers = { { "Content-Type", "application/schema+json" } } }).result;
    if (result.isOk())
    {
        _store->putString(iot_nvs_keys::CONFIG_SCHEMA_HASH, hash);
        log_i("Config: published %s (%s)", filename, hash.c_str());
    } else {
        log_w("Config: publishing %s failed status=%d", filename, result.httpStatus);
    }
    return result;
}
