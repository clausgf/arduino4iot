/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

#include <map>

#include "Arduino.h"
#include <Preferences.h>

// *****************************************************************************

class IotConfig;

/**
 * Interface for configuration values.
 */
class IotPersistableConfigValue
{
public:
    virtual void readFromNvram(Preferences& preferences) = 0;
    virtual const char * getNvramKey() const = 0;
    virtual bool isInt32() const = 0;
    virtual bool isBool() const = 0;
    virtual bool isString() const = 0;
};

/**
 * Wrapper class for configuration values.
 * 
 * The value is stored in NVRAM and can be updated from the server.
 */
template <typename T>
class IotConfigValue: public IotPersistableConfigValue
{
public:
    IotConfigValue(IotConfig& config, T value, const char *key);
    IotConfigValue(IotConfig& config, T value, const char *config_key, const char *nvram_key);
    ~IotConfigValue() {}


    T get() const { return _value; }
    void set(T value) { _value = value; }

    virtual void readFromNvram(Preferences& preferences);
    virtual const char * getNvramKey() const { return _nvram_key; }
    virtual bool isInt32() const;
    virtual bool isBool() const;
    virtual bool isString() const;

    IotConfigValue<T>& operator=(const T& value) { set(value); return *this; }
    operator T() const { return _value; }

private:
    const char *_config_key;
    const char *_nvram_key;
    T _value;
};

// *****************************************************************************

/**
 * Configuration class for the IoT system.
 * 
 * This class manages the configuration values in RAM, NVRAM and the 
 * download of the configuration file from the server. 
 * Use several instances for different configuration files.
 * 
 * A default instance managed by the IoT system is available in the
 * global @see config variable.
 * This instance is used for IoT system managemnt and also available 
 * for application specific configuration. 
 */
class IotConfig
{
public:
    // disallow copying & assignment
    IotConfig(const IotConfig&) = delete;
    IotConfig& operator=(const IotConfig&) = delete;

    IotConfig();
    void begin(
        const char * apiPath = "file/{project}/{device}/config.json", 
        const char * nvramSection = "iot-cfg", 
        const char * nvram_etag_key = "iotCfgEtag", 
        const char * nvram_date_key = "iotCfgDate");
    void end();


    // **********************************************************************
    // Configuration
    // **********************************************************************

    void readConfigFromPreferences();
    void registerConfigValuePtr(const char *configKey, IotPersistableConfigValue* configValuePtr);

    /**
     * Check if the server has a new configuration, based on the ETag and 
     * Last-Modified headers.
     * If available the new configuration is downloaded and stored in NVRAM.
     * It is available via @see getConfigString and @see getConfigInt32.
     * 
     * @return true if a new configuration was downloaded
     */
    bool updateConfig();

    /// @return the Etag of the current configuration for diagnostics
    String getConfigHttpEtag() { return getConfigString(_nvramEtagKey, ""); }
    /// @return the last modified date of the current configuration for diagnostics
    String getConfigHttpDate() { return getConfigString(_nvramDateKey, ""); }

    int32_t getConfigInt32(const char *key, int32_t defaultValue = 0);
    void setConfigInt32(const char *key, int32_t value);
    bool getConfigBool(const char *key, bool defaultValue = false);
    void setConfigBool(const char *key, bool value);
    String getConfigString(const char *key, String defaultValue = "");
    void setConfigString(const char *key, String value);

    template <typename T>
    T getFromConfig(const char *key, T defaultValue = T());


    // **********************************************************************
    // P r i v a t e
    // **********************************************************************

private:
    const char * _apiPath;
    const char * _nvramSection;
    const char * _nvramEtagKey;
    const char * _nvramDateKey;
    std::map<String, IotPersistableConfigValue*> _configMap;
};

extern IotConfig config;
