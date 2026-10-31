/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

#include <climits>
#include <cstdint>
#include <string>

#include <ArduinoJson.h>

#include "iot_cache.h"
#include "iot_names.h"
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
 * Optional description of a configuration value: what the generated
 * config.schema.json tells the server's form (title, description, limits, the
 * allowed strings), and what the library enforces on values from config.json
 * and NVS.
 *
 *   IotConfigValue<int32_t> minSleep(config, 300, "min_sleep_s",
 *       { .title = "Min sleep", .min = 60, .max = 86400 });
 *   IotConfigValue<std::string> rotation(config, "0deg", "rotation",
 *       { .enumValues = "0deg|90deg|180deg|270deg" });
 *
 * An int outside [min, max] is clamped (and logged); a string not among
 * enumValues is rejected like a type mismatch and the previous value stays.
 * Values the application set() are not checked.
 */
struct IotConfigMeta
{
    const char* title = nullptr;
    const char* description = nullptr;
    /// Inclusive limits of an int value.
    int32_t min = INT32_MIN;
    int32_t max = INT32_MAX;
    /// The allowed values of a string, separated by '|'; nullptr allows any.
    const char* enumValues = nullptr;

    bool hasMin() const { return min != INT32_MIN; }
    bool hasMax() const { return max != INT32_MAX; }
    /// @return true if @p value is allowed by enumValues.
    bool allows(const std::string& value) const;
    /// @return @p value limited to [min, max].
    int32_t clamp(int32_t value) const { return value < min ? min : (value > max ? max : value); }
};

/**
 * A registered configuration value, independent of its type.
 */
class IotConfigValueBase
{
public:
    enum class Kind : uint8_t { Int, Bool, String };

    /// Unlinks, so a value that does not live for the whole program leaves no
    /// dangling entry in the list.
    virtual ~IotConfigValueBase();

    const char* key() const { return _key; }
    const IotConfig* owner() const { return _owner; }
    IotConfigValueBase* next() const { return _next; }
    Kind kind() const { return _kind; }
    const IotConfigMeta& meta() const { return _meta; }

    /// @return true if the application set this value explicitly, which makes
    /// it immune to the NVS mirror and to config.json for this cycle.
    bool isOverridden() const { return _overridden; }
    void clearOverride() { _overridden = false; }

    /// Adopt the stored value, unless overridden.
    virtual void loadFrom(const IotStore& store) = 0;

    /// Mirror a config.json member into @p store and publish it unless
    /// overridden. @return false if the JSON type does not match or the value
    /// is not allowed, @see IotConfigMeta.
    virtual bool applyJson(JsonVariantConst value, IotStore& store) = 0;

protected:
    IotConfigValueBase(IotConfig& owner, const char* key, Kind kind, const IotConfigMeta& meta);

    const char* _key;
    IotConfig* _owner;
    IotConfigValueBase* _next;
    IotConfigMeta _meta;
    Kind _kind;
    bool _overridden;
};


// *****************************************************************************

template <typename T> struct IotConfigKind;
template <> struct IotConfigKind<int32_t>     { static constexpr auto value = IotConfigValueBase::Kind::Int; };
template <> struct IotConfigKind<bool>        { static constexpr auto value = IotConfigValueBase::Kind::Bool; };
template <> struct IotConfigKind<std::string> { static constexpr auto value = IotConfigValueBase::Kind::String; };

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
    IotConfigValue(IotConfig& config, T defaultValue, const char* key, const IotConfigMeta& meta = {}):
        IotConfigValueBase(config, key, IotConfigKind<T>::value, meta),
        _value(defaultValue), _default(std::move(defaultValue))
    {
    }

    /// @return the current value. A reference, so .c_str() on a string value
    /// stays valid until the value is reassigned.
    const T& get() const { return _value; }

    /// @return the compile-time default, as published in the schema.
    const T& defaultValue() const { return _default; }

    /// Set explicitly; wins over the NVS mirror and config.json for this cycle.
    void set(const T& value) { _value = value; _overridden = true; }

    void loadFrom(const IotStore& store) override;
    bool applyJson(JsonVariantConst value, IotStore& store) override;

    IotConfigValue<T>& operator=(const T& value) { set(value); return *this; }
    operator const T&() const { return _value; }

private:
    T _value;
    T _default;
};

template <> void IotConfigValue<int32_t>::loadFrom(const IotStore& store);
template <> bool IotConfigValue<int32_t>::applyJson(JsonVariantConst value, IotStore& store);
template <> void IotConfigValue<bool>::loadFrom(const IotStore& store);
template <> bool IotConfigValue<bool>::applyJson(JsonVariantConst value, IotStore& store);
template <> void IotConfigValue<std::string>::loadFrom(const IotStore& store);
template <> bool IotConfigValue<std::string>::applyJson(JsonVariantConst value, IotStore& store);


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


    /**
     * Bind to a store and publish the mirrored values into the registered
     * variables. Values the application already set are left alone. Logs an
     * error for every registered key that does not fit IotStore::MAX_KEY_LENGTH.
     * Iot::begin() binds the global config to the NVS namespace
     * iot_nvs_keys::CONFIG_NAMESPACE.
     *
     * @param store persistence for this configuration, must outlive the object
     * @param apiPath server path of the JSON file, @see updateConfig()
     */
    void begin(IotStore& store, const char* apiPath = iot_paths::CONFIG);

    void end() { _store = nullptr; }

    IotStore* store() const { return _store; }

    /// Re-read every registered value of this instance from the store.
    void reload();

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
    int applyJsonBody(const char* body, int* oUnknown = nullptr, int* oMismatched = nullptr);

    /// @return the ETag and Last-Modified of the mirrored configuration file.
    IotCacheValidators validators() const;

    /// @return true if a config.json was applied at some point, so the
    /// registered values hold the server's values from NVS rather than only
    /// their compile-time defaults. False on a fresh device or after
    /// factoryReset().
    bool isMirrored() const { return !validators().isEmpty(); }

    // --- free-form reads, for keys without a bound variable ---

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

    /// @return the value registered under @p key for this instance, or nullptr.
    IotConfigValueBase* find(const char* key) const { return _find(key); }

    // --- schema ---

    /**
     * @return a JSON Schema describing every registered value of this
     *         instance: type, default, and the IotConfigMeta (title,
     *         description, minimum/maximum, enum). The flat subset nice4iot
     *         turns into a form for config.json.
     */
    std::string schemaJson() const;

    /**
     * Add top-level members to schemaJson(), e.g. a form layout:
     *
     *   config.setSchemaExtras(R"({"x-ui": {"layout": [["panel", "rotation"], ["image_path"]]}})");
     *
     * @p json must be a JSON object; its members are copied as they are, except
     * "properties", "type" and "$schema", which the library owns. They count
     * for the published hash, so changing them publishes again. Invalid JSON
     * is logged and ignored. Not copied: @p json must outlive this object,
     * which a string literal does.
     */
    void setSchemaExtras(const char* json) { _schemaExtras = json; }

    /**
     * Upload schemaJson() as file/{project}/{device}/@p filename, unless the
     * same schema was published before (a hash of it is kept in the store).
     * Call after updateConfig(); a new firmware with changed values publishes
     * once. nice4iot shows a device-uploaded schema only once an operator
     * approved it - per device, since each device uploads its own.
     *
     * @return Ok if uploaded, isNotModified() (304) if unchanged, otherwise
     *         the failed upload
     */
    IotResult publishSchema(const char* filename = "config.schema.json");

private:
    IotStore* _store = nullptr;
    const char* _apiPath = nullptr;
    const char* _schemaExtras = nullptr;

    IotConfigValueBase* _find(const char* key) const;
};

extern IotConfig config;
