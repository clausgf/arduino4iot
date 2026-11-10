/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

// *****************************************************************************
// The names the library shares with the outside world: the nice4iot server
// paths, its own config.json keys and the NVS keys. Both are contracts - the server serves these paths,
// and a device can be provisioned by writing the keys into an NVS partition
// image (docs/concepts.md, "NVS schema") - so they live in one place.
// *****************************************************************************

/// Server paths. {project}, {device} and {board} are substituted per request,
/// {kind} by iot.telemetry.post(). Pass a different path to override one.
namespace iot_paths
{
    inline constexpr const char* PROVISION = "provision";
    inline constexpr const char* TELEMETRY = "telemetry/{project}/{device}/{kind}";
    inline constexpr const char* LOG       = "log/{project}/{device}";
    inline constexpr const char* CONFIG    = "file/{project}/{device}/config.json";
    inline constexpr const char* FIRMWARE  = "file/{project}/{device}/firmware.bin";
    inline constexpr const char* FILE_DIR  = "file/{project}/{device}/";
    inline constexpr const char* FORWARD   = "forward/{project}/{device}/";
}

/// The library's own config.json keys, which are also their NVS keys in
/// "iot-cfg". config.setLayout() places these values itself.
namespace iot_config_keys
{
    inline constexpr const char* LOG_LEVEL    = "log_level";
    inline constexpr const char* SLEEP        = "sleep_s";
    inline constexpr const char* BATTERY_MIN  = "battery_min_mv";
    inline constexpr const char* PANIC_INIT   = "panic_init_s";
    inline constexpr const char* PANIC_FACTOR = "panic_factor";
    inline constexpr const char* PANIC_MAX    = "panic_max_s";
    inline constexpr const char* WIFI_TX_POWER = "wifi_tx_qdbm";
}

/// NVS namespaces and keys. Every key fits IotStore::MAX_KEY_LENGTH (15).
namespace iot_nvs_keys
{
    // --- namespace "iot": bootstrap, tokens, firmware validators ---
    inline constexpr const char* NAMESPACE = "iot";

    inline constexpr const char* WIFI_SSID     = "wifiSsid";
    inline constexpr const char* WIFI_PASSWORD = "wifiPass";
    inline constexpr const char* API_URL       = "apiUrl";
    inline constexpr const char* PROJECT       = "project";
    inline constexpr const char* TLS_MODE      = "tlsMode";
    inline constexpr const char* CA_CERT       = "caCert";
    inline constexpr const char* CLIENT_CERT   = "cliCert";
    inline constexpr const char* CLIENT_KEY    = "cliKey";
    inline constexpr const char* SEED_GEN      = "seedGen";

    inline constexpr const char* PROVISIONING_TOKEN  = "provToken";
    inline constexpr const char* DEVICE_TOKEN        = "deviceToken";
    inline constexpr const char* DEVICE_TOKEN_EXPIRY = "deviceTokExp";

    inline constexpr const char* FIRMWARE_ETAG = "firmwareEtag";
    inline constexpr const char* FIRMWARE_DATE = "firmwareDate";

    // --- namespace "iot-cfg": the config.json mirror, kept apart so
    //     clearProvisioning() can leave it alone. Its other keys are the
    //     config.json keys themselves. ---
    inline constexpr const char* CONFIG_NAMESPACE = "iot-cfg";

    inline constexpr const char* CONFIG_ETAG        = "iotCfgEtag";
    inline constexpr const char* CONFIG_DATE        = "iotCfgDate";
    inline constexpr const char* CONFIG_SCHEMA_HASH = "iotCfgSchema";
}
