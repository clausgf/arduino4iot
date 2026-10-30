/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

#include <cstdint>
#include <string>

#include "iot_ports.h"

// *****************************************************************************
// Build-time bootstrap configuration ("seeding").
//
// All deployment-specific values a device needs *before* it can reach the
// server - WiFi credentials, the API endpoint, project name, provisioning token
// and the TLS trust - are provided once at build time (via -D defines), written
// into the store on first boot and then left untouched. This lets a secretless
// firmware update reuse what is already there. See docs/concepts.md.
// *****************************************************************************

/// NVS keys of the bootstrap values. A documented contract: a device can be
/// provisioned by writing these directly into an NVS partition image instead of
/// by a firmware rebuild. See docs/concepts.md, "NVS schema".
namespace iot_nvs_keys
{
    /// The NVS namespace holding all of the below.
    inline constexpr const char* NAMESPACE = "iot";
    /// The NVS namespace of the server config mirror (IotConfig), kept apart so
    /// clearProvisioning() can leave it alone.
    inline constexpr const char* CONFIG_NAMESPACE = "iot-cfg";

    inline constexpr const char* WIFI_SSID     = "wifiSsid";
    inline constexpr const char* WIFI_PASSWORD = "wifiPass";
    inline constexpr const char* API_URL       = "apiUrl";
    inline constexpr const char* PROJECT       = "project";
    inline constexpr const char* TLS_MODE      = "tlsMode";
    inline constexpr const char* CA_CERT       = "caCert";
    inline constexpr const char* CLIENT_CERT   = "cliCert";
    inline constexpr const char* CLIENT_KEY    = "cliKey";
    inline constexpr const char* SEED_GEN      = "seedGen";
    inline constexpr const char* FIRMWARE_ETAG = "firmwareEtag";
    inline constexpr const char* FIRMWARE_DATE = "firmwareDate";
}

/// Values seeded into the store. Empty strings / None / 0 mean "not provided"
/// and never overwrite an existing value.
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

    /// Bump above the value in the store to force a re-seed (overwrite) of the
    /// provided values with a new firmware; 0 keeps pure seed-if-absent.
    uint32_t    seedGeneration    = 0;
};

// *****************************************************************************

/// Write @p value under @p key when it is non-empty and either forced or not
/// present yet.
inline void iotSeedString(IotStore& store, const char* key, const char* value, bool force)
{
    if (value != nullptr && value[0] != '\0' && (force || !store.has(key)))
    {
        store.putString(key, value);
    }
}

/**
 * Apply @p cfg to @p store.
 *
 * A change of endpoint, project or provisioning token invalidates the device
 * token: one issued under the old identity would be rejected, and dropping it
 * re-provisions immediately instead of waiting for it to expire.
 *
 * @return true if anything was written
 */
bool iotSeedCredentials(IotStore& store, const IotSeedConfig& cfg);
