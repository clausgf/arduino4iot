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
    _configMap()
{
}

void IotConfig::begin(const char * apiPath, const char * nvramSection, 
    const char * nvram_etag_key,  const char * nvram_date_key)
{
    _apiPath = apiPath;
    _nvramSection = nvramSection;
    _nvramEtagKey = nvram_etag_key;
    _nvramDateKey = nvram_date_key;
    readConfigFromPreferences();
    log_i("--- Config section=%s etag=%s date=%s", 
        _nvramSection, getConfigHttpEtag().c_str(), getConfigHttpDate().c_str());
}

void IotConfig::end()
{
}

// *****************************************************************************

void IotConfig::readConfigFromPreferences()
{
    Preferences preferences;
    preferences.begin(_nvramSection, true);
    for (auto const& kv : _configMap)
    {
        kv.second->readFromNvram(preferences);
    }
    preferences.end();
}

void IotConfig::registerConfigValuePtr(const char *configKey, IotPersistableConfigValue* configValuePtr)
{
    _configMap[configKey] = configValuePtr;
}


// *****************************************************************************

bool IotConfig::updateConfig()
{
    if (_apiPath == nullptr || _nvramSection == nullptr || _nvramEtagKey == nullptr || _nvramDateKey == nullptr)
    {
        log_e("IotConfig not initialized - call begin() first");
        return false;
    }

    // get etag and last-modified date from preferences
    Preferences preferences;
    preferences.begin(_nvramSection, true);
    String etag = preferences.getString(_nvramEtagKey, "");
    String date = preferences.getString(_nvramDateKey, "");
    preferences.end();

    // get config from server
    String response = "";
    std::map<String, String> responseHeader;
    const char* collectResponseHeaderKeys[] =  {"ETag", "Last-Modified"};
    int httpStatusCode = api.apiRequest(
        response, responseHeader, 
        "GET", _apiPath, "", {
            {"If-None-Match", etag},
            {"If-Modified-Since", date}
        }, 
        collectResponseHeaderKeys, 2);

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
    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, response);
    if (error)
    {
        log_e("Config JSON deserialization failed error=%s", error.c_str());
        return false;
    }

    // store config in preferences
    preferences.begin(_nvramSection, false);
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
            log_e("Ignoring configKey=%s nvramKey=%s, check types", configKey);
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

    // cleanup
    preferences.end();
    log_i("Configuration data update finished");
    return true;
}

// *****************************************************************************

int32_t IotConfig::getConfigInt32(const char *key, int32_t defaultValue)
{
    Preferences preferences;
    preferences.begin(_nvramSection, true);
    int value = preferences.getInt(key, defaultValue);
    preferences.end();
    return value;
}

void IotConfig::setConfigInt32(const char *key, int32_t value)
{
    Preferences preferences;
    preferences.begin(_nvramSection, false);
    preferences.putInt(key, value);
    preferences.end();
}

bool IotConfig::getConfigBool(const char *key, bool defaultValue)
{
    Preferences preferences;
    preferences.begin(_nvramSection, true);
    bool value = preferences.getBool(key, defaultValue);
    preferences.end();
    return value;
}

void IotConfig::setConfigBool(const char *key, bool value)
{
    Preferences preferences;
    preferences.begin(_nvramSection, false);
    preferences.putBool(key, value);
    preferences.end();
}

String IotConfig::getConfigString(const char *key, String defaultValue)
{
    Preferences preferences;
    preferences.begin(_nvramSection, true);
    String value = preferences.getString(key, defaultValue);
    preferences.end();
    return value;
}

void IotConfig::setConfigString(const char *key, String value)
{
    Preferences preferences;
    preferences.begin(_nvramSection, false);
    preferences.putString(key, value.c_str());
    preferences.end();
}


// *****************************************************************************
