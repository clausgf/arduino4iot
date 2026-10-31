/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#include "iot_seed.h"

#include "iot_token.h"

// *****************************************************************************

bool iotSeedCredentials(IotStore& store, const IotSeedConfig& cfg)
{
    using namespace iot_nvs_keys;

    const uint32_t storedGeneration = (uint32_t)store.getInt(SEED_GEN, 0);
    const bool force = cfg.seedGeneration > storedGeneration;

    const std::string oldApiUrl = store.getString(API_URL, "");
    const std::string oldProject = store.getString(PROJECT, "");
    const std::string oldProvisioningToken =
        store.getString(iot_nvs_keys::PROVISIONING_TOKEN, "");

    iotSeedString(store, WIFI_SSID, cfg.wifiSsid, force);
    iotSeedString(store, WIFI_PASSWORD, cfg.wifiPassword, force);
    iotSeedString(store, API_URL, cfg.apiUrl, force);
    iotSeedString(store, PROJECT, cfg.projectName, force);
    iotSeedString(store, iot_nvs_keys::PROVISIONING_TOKEN, cfg.provisioningToken, force);
    iotSeedString(store, CA_CERT, cfg.caCertPem, force);
    iotSeedString(store, CLIENT_CERT, cfg.clientCertPem, force);
    iotSeedString(store, CLIENT_KEY, cfg.clientKeyPem, force);
    if (cfg.tlsMode != IotTlsMode::None && (force || !store.has(TLS_MODE)))
    {
        store.putInt(TLS_MODE, (int64_t)cfg.tlsMode);
    }

    const bool identityChanged =
        store.getString(API_URL, "") != oldApiUrl ||
        store.getString(PROJECT, "") != oldProject ||
        store.getString(iot_nvs_keys::PROVISIONING_TOKEN, "") != oldProvisioningToken;
    if (identityChanged)
    {
        store.remove(iot_nvs_keys::DEVICE_TOKEN);
        store.remove(iot_nvs_keys::DEVICE_TOKEN_EXPIRY);
    }

    if (force)
    {
        store.putInt(SEED_GEN, (int64_t)cfg.seedGeneration);
    }
    return force || identityChanged;
}
