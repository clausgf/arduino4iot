/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

#include <cstdint>
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

/// Write @p value to NVS key @p key when it is non-empty and either forced or
/// not present yet. Shared by Iot and IotApi seeding.
inline void iotSeedString(Preferences& preferences, const char* key, const char* value, bool force)
{
    if (value != nullptr && value[0] != '\0' && (force || !preferences.isKey(key)))
    {
        preferences.putString(key, value);
    }
}
