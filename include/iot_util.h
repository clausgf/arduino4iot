/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

#include "Arduino.h"
#include <Preferences.h>

// *****************************************************************************

/**
 * Silent NVRAM readers.
 *
 * Preferences::getXxx() unconditionally logs an error like
 * "nvs_get_str len fail: <key> NOT_FOUND" whenever a key is absent - the
 * normal case for every key which has never been written (first boot,
 * optional config values, empty caches). Arduino's log_e() bypasses the
 * ESP-IDF log level machinery, so the message cannot be filtered by tag;
 * the only way to avoid it is not to trigger it. Preferences::isKey()
 * uses the raw nvs_get_*() calls and stays silent, so guarding the read
 * with it keeps the serial log clean at the cost of one extra NVS lookup
 * for missing keys.
 *
 * A key present with a mismatching type still logs - that is a real error.
 *
 * Prefer the IotStore port and its IotNvsStore adapter, which wrap these rules
 * behind a substitutable interface. These helpers remain for the parts of the
 * library not yet moved onto the port.
 */
inline int32_t nvramGetInt(Preferences& preferences, const char *key, int32_t defaultValue)
{
    return preferences.isKey(key) ? preferences.getInt(key, defaultValue) : defaultValue;
}

inline uint8_t nvramGetUChar(Preferences& preferences, const char *key, uint8_t defaultValue)
{
    return preferences.isKey(key) ? preferences.getUChar(key, defaultValue) : defaultValue;
}

inline uint32_t nvramGetUInt(Preferences& preferences, const char *key, uint32_t defaultValue)
{
    return preferences.isKey(key) ? preferences.getUInt(key, defaultValue) : defaultValue;
}

inline int64_t nvramGetLong64(Preferences& preferences, const char *key, int64_t defaultValue)
{
    return preferences.isKey(key) ? preferences.getLong64(key, defaultValue) : defaultValue;
}

inline bool nvramGetBool(Preferences& preferences, const char *key, bool defaultValue)
{
    return preferences.isKey(key) ? preferences.getBool(key, defaultValue) : defaultValue;
}

inline String nvramGetString(Preferences& preferences, const char *key, const String& defaultValue)
{
    return preferences.isKey(key) ? preferences.getString(key, defaultValue) : defaultValue;
}

