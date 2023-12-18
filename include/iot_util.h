/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

#include "Arduino.h"
#include <Preferences.h>

// *****************************************************************************

/**
 * Interface for configuration values.
 */
class IotPersistableValue
{
public:
    virtual void readFromNvram(Preferences& preferences) = 0;
    virtual void writeToNvram(Preferences& preferences) const = 0;
};

// *****************************************************************************

/**
 * Persistent value stored in RTC ram or NVRAM.
 * The following storage types are supported:
 * - IOT_STORAGE_NONE: no storage, value is not persistent
 * - IOT_STORAGE_RTC: value is stored in RTC ram
 * - IOT_STORAGE_NVRAM_IMPLICIT: value is stored in NVRAM and
 *   updated automatically on every call to set() and
 *   every assignment
 * - IOT_STORAGE_NVRAM_EXPLICIT: value is stored in NVRAM and
 *   must be initialized using readFromNvram() and
 *   explicitly stored using writeToNvram(); this allows
 *   bulk reading and writing of all values in a single
 *   NVRAM transaction
 */
template <typename T>
class IotPersistentValue: public IotPersistableValue
{
public:
    /// create a persistent value for RTC ram storage
    IotPersistentValue(T * rtcPtr);
    /// create a persistent value for implicit NVRAM storage
    IotPersistentValue(const char * section, const char * key);
    /// create a persistent value for RTC ram storage (can be adapted to NVRAM later)
    IotPersistentValue(const char * section, const char * key, T * rtcPtr);
    ~IotPersistentValue() {}

    /// for bulk reading of all values, must be initialized with correct section
    virtual void readFromNvram(Preferences& preferences);
    /// for bulk writing of all values, must be initialized with correct section
    virtual void writeToNvram(Preferences& preferences) const;

    enum StorageType { IOT_STORAGE_NONE, IOT_STORAGE_RTC, IOT_STORAGE_NVRAM_IMPLICIT, IOT_STORAGE_NVRAM_EXPLICIT };
    void setStorage(StorageType storageType);

    T get() const;
    void set(const T value);

    IotPersistentValue<T>& operator=(const T& value) { set(value); return *this; }
    operator T() const { return _value; }
private:
    enum StorageType _storageType;
    const char * _section;
    const char * _key;
    T * _rtcPtr;
    T _value;
};

// *****************************************************************************
