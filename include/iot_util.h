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

enum PreferedPersistentStorage { IOT_PERSISTENT_PREFER_RTC, IOT_PERSISTENT_PREFER_NVRAM };

/**
 * Set the prefered storage for persistent values (RTC RAM or NVRAM).
 * 
 * This preference is global. Remember to to select the prefered storage 
 * before initializing persistent values. Also be aware of indirect 
 * initializations, e.g. by calling @see iot.begin().
 * 
 * The default is to prefer NVRAM.
 */
void setPreferedPersistentStorage(PreferedPersistentStorage preferedPersistentStorage);

// *****************************************************************************

/**
 * Persistent value stored in RTC RAM or NVRAM.
 * - RTC RAM requires the internal RTC memory of your ESP32 to
 *   be powered during deep sleep. It does survive deep sleep
 *   but is reset on reset. Write cycles are not limited.
 * - NVRAM does not require your ESP32 to be powered during
 *   deep sleep at all, thus in setups using an external
 *   RTC chip and a power switch this is the only way.
 *   Write cycles to NVRAM are limited, the memory cells wear
 *   during each write and break after an unknown number of
 *   write cycles. A typical specification for NOR flash 
 *   is ~100'000 cycles.
 * The life cycle of persistent values typically starts in
 * begin() methods after executing all consturctors and after
 * basic initialization of the user code. 
 * @see setPreferedPersistentStorage()
 * has to be called before initializing a persistent value
 * using @see begin() if it supports both storage types.
 * On value change, the new values are persisted immediately.
 * 
 * The actual storage is:
 * - IOT_STORAGE_RTC: value is stored in RTC RAM
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

    /**
     * Create a persistent value for RTC RAM storage.
     */
    IotPersistentValue(T * rtcPtr);

    /**
     * Create a persistent value for NVRAM storage.
     * If the @see section parameter is not nullptr, storage
     * is implicit and the value is persisted after every write
     * (@see set(), @see operator=()). Otherwise, the NVRAM storage
     * is explicit. Then reading and writing must be handled 
     * explicitly by the user, 
     * @see readFromNvram() and @see writeToNvram().
     */
    IotPersistentValue(const char * section, const char * key);

    /**
     * Create a persistent value for RTC RAM or NVRAM depending on @see setPreferedPersistentStorage()
     */
    IotPersistentValue(const char * section, const char * key, T * rtcPtr);

    ~IotPersistentValue() {}

    /**
     * Initialize the value based on the actual storage used.
     * Remember to @setPreferredPersistentStorage() before.
     * This overload of begin() supports RTC RAM and implicit
     * NVRAM storage only.
     * For explicit NVRAM storage, use @see begin(Preferences& preferences).
     */
    void begin();

    /**
     * @see begin(), but support RTC RAM and explicit NVRAM storage.
     */
    void begin(Preferences& preferences);

    /// for bulk reading of all values, must be initialized with correct section
    virtual void readFromNvram(Preferences& preferences);
    /// for bulk writing of all values, must be initialized with correct section
    virtual void writeToNvram(Preferences& preferences) const;

    enum StorageType { IOT_STORAGE_NONE, IOT_STORAGE_RTC, IOT_STORAGE_NVRAM_IMPLICIT, IOT_STORAGE_NVRAM_EXPLICIT };

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

bool waitUntil(std::function<bool()> isFinished, unsigned long timeout_ms, 
               const char* logMessage = nullptr);

// *****************************************************************************

esp_reset_reason_t getResetReason();
const char * resetReasonToString(esp_reset_reason_t resetReason);

esp_sleep_wakeup_cause_t getWakeupCause();
const char * wakeupCauseToString(esp_sleep_wakeup_cause_t wakeupCause);

// *****************************************************************************

