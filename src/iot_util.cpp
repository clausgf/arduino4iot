/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#include "iot_util.h"

// *****************************************************************************

template class IotPersistentValue<int32_t>;
template class IotPersistentValue<int64_t>;
template class IotPersistentValue<bool>;
template class IotPersistentValue<String>;


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
    if (_rtcPtr != nullptr) { _storageType = IOT_STORAGE_RTC; }
    else if (_section != nullptr && _key != nullptr) { _storageType = IOT_STORAGE_NVRAM_IMPLICIT; }
    else if (_section == nullptr && _key != nullptr) { _storageType = IOT_STORAGE_NVRAM_EXPLICIT; }
    else { _storageType = IOT_STORAGE_NONE; }
    // set storage type again to initialize the storage
    setStorage(_storageType);
}

// *****************************************************************************

template <typename T>
void IotPersistentValue<T>::setStorage(StorageType storageType)
{
    if (storageType == IOT_STORAGE_RTC && _rtcPtr == nullptr)
    {
        log_e("IotPersistentValue: RTC storage requested but no RTC pointer given");
        return;
    }
    if (storageType == IOT_STORAGE_NVRAM_IMPLICIT && (_section == nullptr || _key == nullptr))
    {
        log_e("IotPersistentValue: NVRAM storage requested but no key given");
        return;
    }
    _storageType = storageType;
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
