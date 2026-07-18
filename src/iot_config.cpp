/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#include "iot_config.h"

#include <map>
#include <nvs_flash.h>
#include <ArduinoJson.h>

#include "iot_logger.h"
#include "iot_api.h"

// *****************************************************************************

IotConfig config;

template class IotConfigValue<int32_t>;
template class IotConfigValue<bool>;
template class IotConfigValue<String>;


// *****************************************************************************
// ConfigValue
// *****************************************************************************

template <typename T>
IotConfigValue<T>::IotConfigValue(IotConfig& config, T value, const char *key):
    IotConfigValue(config, value, key, key)
{
}

template <typename T>
IotConfigValue<T>::IotConfigValue(IotConfig& config, T value, const char *config_key, const char *nvram_key)
{
    _config_key = config_key;
    _nvram_key = nvram_key;
    _value = value;
    config.registerConfigValuePtr(_config_key, this);
}

// *****************************************************************************

template <>
void IotConfigValue<int32_t>::readFromNvram(Preferences& preferences)
{
    _value = preferences.getInt(_nvram_key, _value);
}

template <>
void IotConfigValue<bool>::readFromNvram(Preferences& preferences)
{
    _value = preferences.getBool(_nvram_key, _value);
}

template <>
void IotConfigValue<String>::readFromNvram(Preferences& preferences)
{
    _value = preferences.getString(_nvram_key, _value);
}

template <> bool IotConfigValue<int32_t>::isInt32() const { return true; }
template <> bool IotConfigValue<int32_t>::isBool() const { return false; }
template <> bool IotConfigValue<int32_t>::isString() const { return false; }
template <> bool IotConfigValue<bool>::isInt32() const { return false; }
template <> bool IotConfigValue<bool>::isBool() const { return true; }
template <> bool IotConfigValue<bool>::isString() const { return false; }
template <> bool IotConfigValue<String>::isInt32() const { return false; }
template <> bool IotConfigValue<String>::isBool() const { return false; }
template <> bool IotConfigValue<String>::isString() const { return true; }


// *****************************************************************************
// Config
// *****************************************************************************

IotConfig::IotConfig():
    _apiPath(nullptr),
    _nvramSection(nullptr),
    _nvramEtagKey(nullptr),
    _nvramDateKey(nullptr),
    _configMap(),
    _isOpen(false)
{
}

void IotConfig::begin(const char * apiPath, const char * nvramSection,
    const char * nvram_etag_key,  const char * nvram_date_key)
{
    _apiPath = apiPath;
    _nvramSection = nvramSection;
    _nvramEtagKey = nvram_etag_key;
    _nvramDateKey = nvram_date_key;

    // open the NVRAM section read-write and keep it open until end()
    _isOpen = _preferences.begin(_nvramSection, false);
    if (!_isOpen)
    {
        log_e("IotConfig: failed to open NVRAM section %s", _nvramSection);
    }

    readConfigFromPreferences();
    log_i("--- Config section=%s etag=%s date=%s",
        _nvramSection, getConfigHttpEtag().c_str(), getConfigHttpDate().c_str());
}

void IotConfig::end()
{
    if (_isOpen)
    {
        _preferences.end();
        _isOpen = false;
    }
}

// *****************************************************************************

void IotConfig::readConfigFromPreferences()
{
    if (!_isOpen)
    {
        log_e("IotConfig::readConfigFromPreferences: NVRAM not open - call begin() first");
        return;
    }
    for (auto const& kv : _configMap)
    {
        kv.second->readFromNvram(_preferences);
    }
}

void IotConfig::registerConfigValuePtr(const char *configKey, IotPersistableConfigValue* configValuePtr)
{
    _configMap[configKey] = configValuePtr;
}


// *****************************************************************************

bool IotConfig::updateConfig()
{
    if (_apiPath == nullptr || _nvramSection == nullptr || _nvramEtagKey == nullptr || _nvramDateKey == nullptr || !_isOpen)
    {
        log_e("IotConfig not initialized - call begin() first");
        return false;
    }

    // get etag and last-modified date from the open preferences handle
    String etag = _preferences.getString(_nvramEtagKey, "");
    String date = _preferences.getString(_nvramDateKey, "");

    // get config from server
    String response = "";
    std::map<String, String> responseHeader;
    int httpStatusCode = api.apiRequest(
        response, responseHeader,
        "GET", _apiPath, "", {
            {"If-None-Match", etag},
            {"If-Modified-Since", date}
        },
        {"ETag", "Last-Modified"});

    if (httpStatusCode < 200 || httpStatusCode >= 300)
    {
        if (httpStatusCode == HTTP_CODE_NOT_MODIFIED)
        {
            log_i("Configuration data not modified");
        } else {
            log_e("HTTP GET failed status=%d", httpStatusCode);
        }
        return false;
    }

    // decode JSON payload
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, response);
    if (error)
    {
        log_e("Config JSON deserialization failed error=%s", error.c_str());
        return false;
    }

    // store config in the open preferences handle
    Preferences& preferences = _preferences;
    for (JsonPair kv : doc.as<JsonObject>())
    {
        const char * configKey = kv.key().c_str();
        if (_configMap.find(configKey) == _configMap.end())
        {
            log_e("Ignoring unknown key %s", configKey);
            continue;
        }
        IotPersistableConfigValue* configValuePtr = _configMap[configKey];
        const char * nvramKey = configValuePtr->getNvramKey();

        if (kv.value().is<int>() && configValuePtr->isInt32())
        {
            int value = kv.value().as<int>();
            if ( !preferences.isKey(nvramKey) || (value != preferences.getInt(nvramKey)) )
            {
                log_d("configKey=%s nvramKey=%s value=%d", configKey, nvramKey, value);
                preferences.putInt(nvramKey, value);
            }
        } else if (kv.value().is<bool>() && configValuePtr->isBool())
        {
            bool value = kv.value().as<bool>();
            if ( !preferences.isKey(nvramKey) || (value != preferences.getBool(nvramKey)) )
            {
                log_d("configKey=%s nvramKey=%s value=%s", configKey, nvramKey, value ? "true" : "false");
                preferences.putBool(nvramKey, value);
            }
        } else if (kv.value().is<String>() && configValuePtr->isString())
        {
            String value = kv.value().as<String>();
            if ( !preferences.isKey(nvramKey) || (value != preferences.getString(nvramKey)) )
            {
                log_d("configKey=%s nvramKey=%s value=%s", configKey, nvramKey, value.c_str());
                preferences.putString(nvramKey, value);
            }
        } else
        {
            log_e("Ignoring configKey=%s nvramKey=%s, check types", configKey, nvramKey);
        }
    }

    // update etag and last-modified date in preferences
    for (auto const& kv : responseHeader)
    {
        //log_v("  HTTP response header: %s=%s", kv.first.c_str(), kv.second.c_str());
        if (kv.first.equalsIgnoreCase("etag"))
        {
            preferences.putString(_nvramEtagKey, kv.second);
            log_d("  Config etag=%s", kv.second.c_str());
        } else if (kv.first.equalsIgnoreCase("last-modified"))
        {
            preferences.putString(_nvramDateKey, kv.second);
            log_d("  Config date=%s", kv.second.c_str());
        }
    }

    // publish config values
    for (auto const& kv : _configMap)
    {
        kv.second->readFromNvram(preferences);
    }

    // the preferences handle stays open until end()
    log_i("Configuration data update finished");
    return true;
}

// *****************************************************************************

int32_t IotConfig::getConfigInt32(const char *key, int32_t defaultValue)
{
    if (!_isOpen) { return defaultValue; }
    return _preferences.getInt(key, defaultValue);
}

void IotConfig::setConfigInt32(const char *key, int32_t value)
{
    if (!_isOpen) { return; }
    _preferences.putInt(key, value);
}

bool IotConfig::getConfigBool(const char *key, bool defaultValue)
{
    if (!_isOpen) { return defaultValue; }
    return _preferences.getBool(key, defaultValue);
}

void IotConfig::setConfigBool(const char *key, bool value)
{
    if (!_isOpen) { return; }
    _preferences.putBool(key, value);
}

String IotConfig::getConfigString(const char *key, const String& defaultValue)
{
    if (!_isOpen) { return defaultValue; }
    return _preferences.getString(key, defaultValue);
}

void IotConfig::setConfigString(const char *key, const String& value)
{
    if (!_isOpen) { return; }
    _preferences.putString(key, value.c_str());
}


// *****************************************************************************
