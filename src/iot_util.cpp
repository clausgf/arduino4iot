/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#include "Arduino.h"

#include "iot_util.h"

// *****************************************************************************

template class IotPersistentValue<int32_t>;
template class IotPersistentValue<int64_t>;
template class IotPersistentValue<bool>;
template class IotPersistentValue<String>;

// *****************************************************************************

static PreferedPersistentStorage _preferedPersistentStorage = IOT_PERSISTENT_PREFER_NVRAM;

void setPreferedPersistentStorage(PreferedPersistentStorage preferedPersistentStorage)
{ 
    _preferedPersistentStorage = preferedPersistentStorage;
}


// *****************************************************************************
// IotPersistentValue
// *****************************************************************************

template <typename T>
IotPersistentValue<T>::IotPersistentValue(T * rtcPtr):
    IotPersistentValue(nullptr, nullptr, rtcPtr)
{
}

template <typename T>
IotPersistentValue<T>::IotPersistentValue(const char * section, const char *key):
    IotPersistentValue(section, key, nullptr)
{
}

template <typename T>
IotPersistentValue<T>::IotPersistentValue(const char * section, const char * key, T * rtcPtr)
{
    _section = section;
    _key = key;
    _rtcPtr = rtcPtr;
}

template <typename T>
void IotPersistentValue<T>::begin()
{
    // prefer RTC RAM by default
    if (_rtcPtr != nullptr) { _storageType = IOT_STORAGE_RTC; }
    else if (_section != nullptr && _key != nullptr) { _storageType = IOT_STORAGE_NVRAM_IMPLICIT; }
    else if (_section == nullptr && _key != nullptr) { _storageType = IOT_STORAGE_NVRAM_EXPLICIT; }
    else { _storageType = IOT_STORAGE_NONE; }

    // correct storage type if NVRAM prefered and supported
    if (_preferedPersistentStorage == IOT_PERSISTENT_PREFER_NVRAM && _key != nullptr)
    {
        if (_section != nullptr) { _storageType = IOT_STORAGE_NVRAM_IMPLICIT; }
        else { _storageType = IOT_STORAGE_NVRAM_EXPLICIT; }
    }

    // initialize the storage
    if (_storageType == IOT_STORAGE_RTC && _rtcPtr == nullptr)
    {
        log_e("IotPersistentValue: RTC storage requested but no RTC pointer given");
        return;
    }
    if (_storageType == IOT_STORAGE_RTC)
    {
        _value = *_rtcPtr;
    } else if (_storageType == IOT_STORAGE_NVRAM_IMPLICIT) {
        Preferences preferences;
        preferences.begin(_section, true);
        if (preferences.isKey(_key))
        {
            readFromNvram(preferences);
        } else {
            log_i("IotPersistentValue: NVRAM key '%s/%s' not found, using default value", _section, _key);
        }
        preferences.end();
    }
}

template <typename T>
void IotPersistentValue<T>::begin(Preferences& preferences)
{
    begin();  // determine storage type, read NVRAM variables
    
    if (_storageType == IOT_STORAGE_NVRAM_EXPLICIT)
    {
        readFromNvram(preferences);
    }
}

// *****************************************************************************

template <>
void IotPersistentValue<int32_t>::readFromNvram(Preferences& preferences)
{
    _value = preferences.getInt(_key, _value);
}

template <>
void IotPersistentValue<int64_t>::readFromNvram(Preferences& preferences)
{
    _value = preferences.getLong64(_key, _value);
}

template <>
void IotPersistentValue<bool>::readFromNvram(Preferences& preferences)
{
    _value = preferences.getBool(_key, _value);
}

template <>
void IotPersistentValue<String>::readFromNvram(Preferences& preferences)
{
    _value = preferences.getString(_key, _value);
}

// *****************************************************************************

template <>
void IotPersistentValue<int32_t>::writeToNvram(Preferences& preferences) const
{
    preferences.putInt(_key, _value);
    log_i("IotPersistentValue: NVRAM key '%s/%s' set to %d", _section, _key, _value);
}

template <>
void IotPersistentValue<int64_t>::writeToNvram(Preferences& preferences) const
{
    preferences.putLong64(_key, _value);
    log_i("IotPersistentValue: NVRAM key '%s/%s' set to %d", _section, _key, _value);
}

template <>
void IotPersistentValue<bool>::writeToNvram(Preferences& preferences) const
{
    preferences.putBool(_key, _value);
    log_i("IotPersistentValue: NVRAM key '%s/%s' set to %d", _section, _key, _value);
}

template <>
void IotPersistentValue<String>::writeToNvram(Preferences& preferences) const
{
    preferences.putString(_key, _value);
    log_i("IotPersistentValue: NVRAM key '%s/%s' set to %d", _section, _key, _value);
}

// *****************************************************************************

template <typename T>
T IotPersistentValue<T>::get() const
{
    return _value;
}

template <typename T>
void IotPersistentValue<T>::set(T value)
{
    if (value == _value)
    { 
        return; 
    }

    _value = value;

    if (_storageType == IOT_STORAGE_RTC)
    {
        *_rtcPtr = _value;
    } else if (_storageType == IOT_STORAGE_NVRAM_IMPLICIT) {
        Preferences preferences;
        preferences.begin(_section, false);
        writeToNvram(preferences);
        preferences.end();
    }
}

// *****************************************************************************

bool waitUntil(std::function<bool()> isFinished, unsigned long timeout_ms, const char* logMessage)
{
    unsigned long start_time = millis();
    bool finished = isFinished();

    if (finished)
    {
        // silently return
        return true;
    }
   
    while ( (millis() - start_time) < timeout_ms )
    {
        finished = isFinished();
        if (finished) 
        {
            if (logMessage != nullptr)
            {
                log_i("waitUntil %s: successful after %d ms", logMessage, millis() - start_time);
            }
            return true;
        }
        delay(10);
    }

    if (logMessage != nullptr)
    {
        log_i("waitUntil %s: timeout after %d ms", logMessage, timeout_ms);
    }
    return false;
}

// *****************************************************************************

esp_reset_reason_t getResetReason()
{
    return esp_reset_reason();
}

const char * resetReasonToString(esp_reset_reason_t resetReason)
{
    switch (resetReason)
    {
        case ESP_RST_UNKNOWN:   return "UNKNOWN";            // Reset reason can not be determined
        case ESP_RST_POWERON:   return "POWER_ON";           // Reset due to power-on event
        case ESP_RST_EXT:       return "EXTERNAL_PIN";       // Reset by external pin (not applicable for ESP32)
        case ESP_RST_SW:        return "SOFTWARE";           // Software reset via esp_restart
        case ESP_RST_PANIC:     return "EXCEPTION_PANIC";    // Software reset due to exception/panic
        case ESP_RST_INT_WDT:   return "INTERRUPT_WATCHDOG"; // Reset (software or hardware) due to interrupt watchdog
        case ESP_RST_TASK_WDT:  return "TASK_WATCHDOG";      // Reset due to task watchdog
        case ESP_RST_WDT:       return "OTHER_WATCHDOG";     // Reset due to other watchdogs
        case ESP_RST_DEEPSLEEP: return "DEEP_SLEEP";         // Reset after exiting deep sleep mode
        case ESP_RST_BROWNOUT:  return "BROWNOUT";           // Brownout reset (software or hardware)
        case ESP_RST_SDIO:      return "SDIO";               // Reset over SDIO
        default:                return "UNKNOWN";            // Reset reason can not be determined
    }
}

// *****************************************************************************

esp_sleep_wakeup_cause_t getWakeupCause()
{
    return esp_sleep_get_wakeup_cause();
}

const char * wakeupCauseToString(esp_sleep_wakeup_cause_t wakeupCause)
{
    switch (wakeupCause)
    {
        case ESP_SLEEP_WAKEUP_UNDEFINED: return "UNDEFINED"; // In case of deep sleep, reset was not caused by exit from deep sleep
        case ESP_SLEEP_WAKEUP_ALL:       return "ALL";       // Wakeup caused by all sources - used in esp_sleep_enable_ext1_wakeup()
        case ESP_SLEEP_WAKEUP_EXT0:      return "EXT0";      // Wakeup caused by external signal using RTC_IO
        case ESP_SLEEP_WAKEUP_EXT1:      return "EXT1";      // Wakeup caused by external signal using RTC_CNTL
        case ESP_SLEEP_WAKEUP_TIMER:     return "TIMER";     // Wakeup caused by timer
        case ESP_SLEEP_WAKEUP_TOUCHPAD:  return "TOUCHPAD";  // Wakeup caused by touchpad
        case ESP_SLEEP_WAKEUP_ULP:       return "ULP";       // Wakeup caused by ULP program
        case ESP_SLEEP_WAKEUP_GPIO:      return "GPIO";      // Wakeup caused by GPIO (light sleep only on ESP32, S2 and S3)
        case ESP_SLEEP_WAKEUP_UART:      return "UART";      // Wakeup caused by UART (light sleep only)
        case ESP_SLEEP_WAKEUP_WIFI:      return "WIFI";      // Wakeup caused by WIFI (light sleep only)
        case ESP_SLEEP_WAKEUP_COCPU:     return "COCPU";     // Wakeup caused by COCPU int
        case ESP_SLEEP_WAKEUP_COCPU_TRAP_TRIG: return "COCPU_TRAP_TRIG"; // Wakeup caused by COCPU crash
        case ESP_SLEEP_WAKEUP_BT:        return "BT";        // Wakeup caused by BT (light sleep only)
        default:                         return "UNKNOWN";   // Wakeup reason can not be determined
    }
}

// *****************************************************************************

