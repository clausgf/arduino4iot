/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

#include <cstdint>

#include "Arduino.h"
#include <Preferences.h>

// *****************************************************************************
// Build-time bootstrap configuration ("seeding").
//
// All deployment-specific values a device needs *before* it can reach the
// server - WiFi credentials, the API endpoint, project name, provisioning token
// and the TLS trust configuration - are provided once at build time (via -D
// defines), written into NVS on first boot and then left untouched. This lets a
// secretless firmware update (e.g. from a public CI) reuse the values already in
// NVS. See docs/concepts.md and examples/README.md.
// *****************************************************************************

/// How the server certificate is verified for an https:// API URL.
enum class IotTlsMode : uint8_t
{
    None,       ///< no TLS configuration (plain http:// endpoint)
    Bundle,     ///< verify against the built-in Mozilla root bundle
    Insecure,   ///< development only: do not verify the server certificate
    CaPin,      ///< verify against the seeded CA certificate (caCertPem)
};

/// Values seeded into NVS by Iot::seedCredentials(). Empty strings / None / 0
/// mean "not provided" and never overwrite an existing NVS value.
struct IotSeedConfig
{
    // bootstrap connectivity (WiFi password and provisioning token are secrets)
    const char* wifiSsid          = "";
    const char* wifiPassword      = "";
    const char* apiUrl            = "";
    const char* projectName       = "";
    const char* provisioningToken = "";

    // TLS trust (deployment-specific; clientKeyPem is a secret)
    IotTlsMode  tlsMode           = IotTlsMode::None;
    const char* caCertPem         = "";   ///< PEM, for tlsMode == CaPin
    const char* clientCertPem     = "";   ///< PEM, optional mutual TLS
    const char* clientKeyPem      = "";   ///< PEM, optional mutual TLS

    /// Bump above the value stored in NVS to force a re-seed (overwrite) of the
    /// provided (non-empty) values with a new firmware; 0 keeps pure
    /// seed-if-absent behaviour.
    uint32_t    seedGeneration    = 0;
};

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

// *****************************************************************************

/// Write @p value to NVS key @p key when it is non-empty and either forced or
/// not present yet. Shared by Iot and IotApi seeding.
inline void iotSeedString(Preferences& preferences, const char* key, const char* value, bool force)
{
    if (value != nullptr && value[0] != '\0' && (force || !preferences.isKey(key)))
    {
        preferences.putString(key, value);
    }
}
