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

template class IotConfigValue<int>;
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
void IotConfigValue<int>::readFromPreferences(Preferences& preferences)
{
    _value = preferences.getInt(_nvram_key, _value);
}

template <>
void IotConfigValue<bool>::readFromPreferences(Preferences& preferences)
{
    _value = preferences.getBool(_nvram_key, _value);
}

template <>
void IotConfigValue<String>::readFromPreferences(Preferences& preferences)
{
    _value = preferences.getString(_nvram_key, _value);
}

// *****************************************************************************

template <>
void IotConfigValue<int>::writeToPreferences(Preferences& preferences) const
{
    preferences.putInt(_nvram_key, _value);
}

template <>
void IotConfigValue<bool>::writeToPreferences(Preferences& preferences) const
{
    preferences.putBool(_nvram_key, _value);
}

template <>
void IotConfigValue<String>::writeToPreferences(Preferences& preferences) const
{
    preferences.putString(_nvram_key, _value);
}


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
    preferences.begin("iot", true);
    for (auto const& kv : _configMap)
    {
        kv.second->readFromPreferences(preferences);
    }
    preferences.end();
}

void IotConfig::registerConfigValuePtr(const char *configKey, PersistableIotConfigValue* configValuePtr)
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
        String key = kv.key().c_str();
        if (kv.value().is<int>())
        {
            int value = kv.value().as<int>();
            if ( !preferences.isKey(key.c_str()) || (value != preferences.getInt(key.c_str(), -1)) )
            {
                log_d("Config %s=%d", key.c_str(), value);
                preferences.putInt(key.c_str(), value);
            }
        } else if (kv.value().is<bool>())
        {
            bool value = kv.value().as<bool>();
            if ( !preferences.isKey(key.c_str()) || (value != preferences.getBool(key.c_str(), false)) )
            {
                log_d("Config %s=%s", key.c_str(), value ? "true" : "false");
                preferences.putBool(key.c_str(), value);
            }
        } else if (kv.value().is<String>())
        {
            String value = kv.value().as<String>();
            if ( !preferences.isKey(key.c_str()) || (value != preferences.getString(key.c_str(), "")) )
            {
                log_d("Config %s=%s", key.c_str(), value.c_str());
                preferences.putString(key.c_str(), value);
            }
        } else
        {
            log_d("Ignoring key %s", key.c_str());
        }
    }
    
    // update etag and last-modified date in preferences
    for (auto const& kv : responseHeader)
    {
        log_v("  HTTP response header: %s=%s", kv.first.c_str(), kv.second.c_str());
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
        kv.second->readFromPreferences(preferences);
    }

    // cleanup
    preferences.end();
    log_i("Configuration data update finished");
    return true;
}

// *****************************************************************************

int IotConfig::getConfigInt(const char *key, int defaultValue)
{
    Preferences preferences;
    preferences.begin(_nvramSection, true);
    int value = preferences.getInt(key, defaultValue);
    preferences.end();
    return value;
}

void IotConfig::setConfigInt(const char *key, int value)
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
    preferences.begin("iot", false);
    preferences.putString(key, value.c_str());
    preferences.end();
}


// *****************************************************************************