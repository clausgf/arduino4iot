/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

#include <cstdint>
#include <string>

#include <ArduinoJson.h>

#include "iot_ports.h"
#include "iot_result.h"

// *****************************************************************************
// Configuration values mirrored from a server-side config.json into NVS.
//
// One key per value: the same name identifies it in config.json, in NVS and in
// the free-form accessors. It must therefore fit IotStore::MAX_KEY_LENGTH.
//
// Precedence, from weakest to strongest:
//
//   compile-time default  <  NVS mirror  <  config.json  <  set() at runtime
//
// A value the application sets explicitly keeps that value for the rest of the
// cycle - neither begin() nor updateConfig() overwrite it. Server values are
// still mirrored into NVS, so they take effect from the next boot. Use the
// setter for values the application owns, and leave a value alone to let the
// server own it.
//
// Arduino-free apart from IotConfig::updateConfig(), which performs the HTTP
// request; everything else is exercised on the host (test/test_config).
// *****************************************************************************

class IotConfig;
class IotConfigValueBase;

/// Head of the intrusive list of all registered values.
///
/// Values register themselves from their constructors, which run during static
/// initialization in an unspecified order across translation units. A raw
/// pointer is constant-initialized before any of that, so linking into this
/// list is safe even when the target IotConfig has not been constructed yet -
/// only its address is taken, never its members.
inline IotConfigValueBase*& iotConfigValueListHead()
{
    static IotConfigValueBase* head = nullptr;
    return head;
}

/**
 * A registered configuration value, independent of its type.
 */
class IotConfigValueBase
{
public:
    /// Unlinks, so a value that does not live for the whole program leaves no
    /// dangling entry in the list.
    virtual ~IotConfigValueBase()
    {
        for (IotConfigValueBase** link = &iotConfigValueListHead();
             *link != nullptr; link = &(*link)->_next)
        {
            if (*link == this) { *link = _next; return; }
        }
    }

    const char* key() const { return _key; }
    const IotConfig* owner() const { return _owner; }
    IotConfigValueBase* next() const { return _next; }

    /// @return true if the application set this value explicitly, which makes
    /// it immune to the NVS mirror and to config.json for this cycle.
    bool isOverridden() const { return _overridden; }
    void clearOverride() { _overridden = false; }

    /// Adopt the stored value, unless overridden.
    virtual void loadFrom(const IotStore& store) = 0;

    /// Mirror a config.json member into @p store and publish it unless
    /// overridden. @return false if the JSON type does not match.
    virtual bool applyJson(JsonVariantConst value, IotStore& store) = 0;

protected:
    IotConfigValueBase(IotConfig& owner, const char* key):
        _key(key), _owner(&owner), _next(iotConfigValueListHead()), _overridden(false)
    {
        iotConfigValueListHead() = this;
    }

    const char* _key;
    IotConfig* _owner;
    IotConfigValueBase* _next;
    bool _overridden;
};


// *****************************************************************************

/**
 * Binds a C++ variable to a configuration key.
 *
 * Instantiated for int32_t, bool and std::string. Assignment counts as an
 * explicit override, @see IotConfigValueBase::isOverridden().
 */
template <typename T>
class IotConfigValue : public IotConfigValueBase
{
public:
    IotConfigValue(IotConfig& config, T defaultValue, const char* key):
        IotConfigValueBase(config, key), _value(std::move(defaultValue))
    {
    }

    /// @return the current value. A reference, so .c_str() on a string value
    /// stays valid until the value is reassigned.
    const T& get() const { return _value; }

    /// Set explicitly; wins over the NVS mirror and config.json for this cycle.
    void set(const T& value) { _value = value; _overridden = true; }

    void loadFrom(const IotStore& store) override;
    bool applyJson(JsonVariantConst value, IotStore& store) override;

    IotConfigValue<T>& operator=(const T& value) { set(value); return *this; }
    operator const T&() const { return _value; }

private:
    T _value;
};

// --- int32_t ---

template <>
inline void IotConfigValue<int32_t>::loadFrom(const IotStore& store)
{
    if (!_overridden) { _value = (int32_t)store.getInt(_key, _value); }
}

template <>
inline bool IotConfigValue<int32_t>::applyJson(JsonVariantConst value, IotStore& store)
{
    if (!value.is<int32_t>() || value.is<bool>()) { return false; }
    store.putInt(_key, value.as<int32_t>());
    if (!_overridden) { _value = value.as<int32_t>(); }
    return true;
}

// --- bool ---

template <>
inline void IotConfigValue<bool>::loadFrom(const IotStore& store)
{
    if (!_overridden) { _value = store.getBool(_key, _value); }
}

template <>
inline bool IotConfigValue<bool>::applyJson(JsonVariantConst value, IotStore& store)
{
    if (!value.is<bool>()) { return false; }
    store.putBool(_key, value.as<bool>());
    if (!_overridden) { _value = value.as<bool>(); }
    return true;
}

// --- std::string ---

template <>
inline void IotConfigValue<std::string>::loadFrom(const IotStore& store)
{
    if (!_overridden) { _value = store.getString(_key, _value); }
}

template <>
inline bool IotConfigValue<std::string>::applyJson(JsonVariantConst value, IotStore& store)
{
    if (!value.is<const char*>()) { return false; }
    std::string text = value.as<const char*>();
    store.putString(_key, text);
    if (!_overridden) { _value = std::move(text); }
    return true;
}


// *****************************************************************************

/**
 * Mirrors a server-side JSON configuration file into an IotStore.
 *
 * A default instance is available as the global `config`; it serves the
 * library's own tunables and application values alike. Further instances can
 * mirror further files - each value is bound to the instance passed to its
 * constructor.
 */
class IotConfig
{
public:
    IotConfig(const IotConfig&) = delete;
    IotConfig& operator=(const IotConfig&) = delete;

    IotConfig() = default;

    static constexpr const char* KEY_ETAG = "iotCfgEtag";
    static constexpr const char* KEY_DATE = "iotCfgDate";

    /**
     * Bind to a store and publish the mirrored values into the registered
     * variables. Values the application already set are left alone.
     *
     * @param store persistence for this configuration, must outlive the object
     * @param apiPath server path of the JSON file, @see updateConfig()
     */
    void begin(IotStore& store, const char* apiPath = "file/{project}/{device}/config.json")
    {
        _store = &store;
        _apiPath = apiPath;
        reload();
    }

    /// Bind to the library's default NVS namespace ("iot-cfg").
    void begin(const char* apiPath = "file/{project}/{device}/config.json");

    void end() { _store = nullptr; }

    IotStore* store() const { return _store; }

    /// Re-read every registered value of this instance from the store.
    void reload()
    {
        if (_store == nullptr) { return; }
        for (IotConfigValueBase* v = iotConfigValueListHead(); v != nullptr; v = v->next())
        {
            if (v->owner() == this) { v->loadFrom(*_store); }
        }
    }

    /**
     * Fetch the configuration file if the server has a newer one and apply it.
     *
     * Conditional on the stored ETag/Last-Modified, so an unchanged file is not
     * transferred again.
     *
     * @return Ok if a new configuration was applied; isNotModified() (304) if
     *         unchanged; STATUS_MALFORMED_RESPONSE if the body was not valid
     *         JSON; otherwise the HTTP or transport error.
     *         Test with isOkOrNotModified() for "no error".
     */
    IotResult updateConfig();

    /**
     * Apply a configuration body: mirror every known key into the store and
     * publish it. Unknown keys and type mismatches are counted and skipped, so
     * one bad entry cannot discard the rest of the file.
     *
     * @return the number of applied keys, or -1 if the body was not valid JSON
     */
    int applyJsonBody(const char* body, int* oUnknown = nullptr, int* oMismatched = nullptr)
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

    /// @return the ETag of the mirrored configuration, for diagnostics.
    std::string etag() const { return _store ? _store->getString(KEY_ETAG, "") : std::string(); }
    /// @return the Last-Modified date of the mirrored configuration.
    std::string lastModified() const { return _store ? _store->getString(KEY_DATE, "") : std::string(); }

    // --- free-form access, for values without a bound variable ---

    int64_t getInt(const char* key, int64_t defaultValue = 0) const
    {
        return _store ? _store->getInt(key, defaultValue) : defaultValue;
    }
    bool getBool(const char* key, bool defaultValue = false) const
    {
        return _store ? _store->getBool(key, defaultValue) : defaultValue;
    }
    std::string getString(const char* key, const std::string& defaultValue = "") const
    {
        return _store ? _store->getString(key, defaultValue) : defaultValue;
    }

    void setInt(const char* key, int64_t value) { if (_store) { _store->putInt(key, value); } }
    void setBool(const char* key, bool value) { if (_store) { _store->putBool(key, value); } }
    void setString(const char* key, const std::string& value) { if (_store) { _store->putString(key, value); } }

    /// @return the value registered under @p key for this instance, or nullptr.
    IotConfigValueBase* find(const char* key) const { return _find(key); }

private:
    IotStore* _store = nullptr;
    const char* _apiPath = nullptr;

    IotConfigValueBase* _find(const char* key) const
    {
        for (IotConfigValueBase* v = iotConfigValueListHead(); v != nullptr; v = v->next())
        {
            if (v->owner() == this && std::string(v->key()) == key) { return v; }
        }
        return nullptr;
    }
};

extern IotConfig config;
