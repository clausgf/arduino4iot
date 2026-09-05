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
 * New code should prefer the IotStore port (iot_ports.h) and its IotNvsStore
 * adapter, which wrap these rules behind an interface that can be substituted
 * in a host test. These helpers remain for the parts of the library that have
 * not been moved onto the port yet.
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

// *****************************************************************************
// Removed in 4.0
//
// - IotPersistentValue / IotPersistableValue / setPreferedPersistentStorage():
//   replaced by the explicit split between IotRtcState (survives deep sleep,
//   free to write) and IotStore (survives a power loss, costs flash cycles).
//   Which storage a value uses is now a design decision visible at its
//   declaration, not a global runtime switch applied to every value at once -
//   and the values themselves are initialized, which the old class left to
//   begin() and thus read uninitialized on the constructor's panic path.
// - getResetReason() / resetReasonToString() / getWakeupCause() /
//   wakeupCauseToString(): replaced by IotPower::resetReason() /
//   ::wakeupCause() and iotResetReasonToString() / iotWakeupCauseToString().
// - waitUntil(): unused.
// *****************************************************************************
