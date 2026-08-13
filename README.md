# arduino4iot

ESP32 client library for Internet of Things applications.

## Introduction

The *x4iot system* consists of this client library (`arduino4iot`) and the server application [nice4iot](https://github.com/clausgf/nice4iot). The interface is based on the HTTP protocol and relies on standard HTTP headers like `ETag` and `If-None-Match` for efficient communication. A complete client wakeup-cycle including establishing a WiFi connection, sending application and system telemetry, checking configuration and firmware updates plus taking some quick measurements can be completed in less than 900 ms, making *x4iot* a good choice for battery powered applications.

The *nice4iot* server is intended for easy self hosting. Options include a Raspberry Pi in your home intranet or a virtual server at your cloud provider. The server is responsible for

- provisioning and secure access using provisioning and API tokens,
- storing telemetry in a time series backend (Prometheus Remote Write or InfluxDB line protocol),
- storing logs (Grafana Loki or plain files),
- providing firmware updates,
- providing configuration and other files to IoT clients,
- receiving file uploads from devices (e.g. diagnostic dumps),
- forwarding device requests to configured upstream services.

## Features

- **Provisioning:** obtain a device API token using a provisioning token; the token expiry reported by the server is stored, and the token is renewed proactively before it expires. On HTTP 401, the library re-provisions and retries the request automatically.
- **Telemetry:** post measurements built with the `IotTelemetry` builder, which guarantees the flat JSON format expected by the server.
- **Remote logging:** log messages are buffered in RAM and sent to the server in a single request at the end of the wakeup cycle (configurable via `logger.setBuffered()`, forced via `logger.flush()`); large bodies are split to respect the server's size limit.
- **Configuration:** download per-device or project-wide configuration files with ETag caching; values are persisted in NVRAM.
- **OTA firmware updates:** ETag-based update check and streaming firmware download.
- **File upload:** `api.uploadFile()` pushes files to the device-specific storage on the server.
- **Forwarding:** `api.apiForward()` proxies requests to upstream services configured in the project settings.
- **Fast WiFi connect:** to cut the radio-on time of a wakeup, the last-good AP (BSSID + channel) is cached in RTC RAM for a scan-free reconnect (on by default, `iot.setFastReconnect()`); DHCP can be skipped either with a fixed static IP (`iot.setStaticIp()`) or by caching the DHCP-assigned lease in RTC and reusing it automatically (`iot.setDhcpCache()`, opt-in). All fall back automatically to a full-scan/DHCP connect. The connect duration is exposed as `wifi_connect_ms` telemetry.
- **System management:** deep sleep cycle bookkeeping, watchdog supervision, battery monitoring with undervoltage shutdown, escalating panic/backoff strategy.

## Getting started

1. Add the platform and the library to your `platformio.ini`. The library requires arduino-esp32 3.x (via the [pioarduino](https://github.com/pioarduino/platform-espressif32) platform — the registry `espressif32` only ships 2.x) and C++20:
   ```ini
   [env:esp32dev]
   platform = https://github.com/pioarduino/platform-espressif32/releases/download/54.03.20/platform-espressif32.zip
   board = esp32dev
   framework = arduino
   lib_deps =
       https://github.com/clausgf/arduino4iot
   build_flags = -std=gnu++20
   ```
2. Provide the deployment-specific configuration at build time and initialize the library in `setup()` (see [`examples/`](examples/) for a complete project including the secrets wiring):
   ```cpp
   // values come from -D defines (empty on a secretless build), seeded into NVS
   iot.seedCredentials({
       .wifiSsid          = IOT_WIFI_SSID,
       .wifiPassword      = IOT_WIFI_PASSWORD,
       .apiUrl            = IOT_API_URL,
       .projectName       = IOT_PROJECT,
       .provisioningToken = IOT_PROVISIONING_TOKEN,
       .seedGeneration    = IOT_SEED_GENERATION,
   });
   if (!iot.begin())                     // WiFi from NVS, subsystems, NTP
   {
       iot.panic("*** PANIC *** WiFi connection or NTP sync failed");
   }
   if (!api.updateProvisioningOk())
   {
       iot.panic("*** PANIC *** Provisioning failed");
   }
   config.updateConfig();
   iot.resetWatchdog();
   api.updateFirmware();
   iot.postSystemTelemetry();

   // measure something
   IotTelemetry telemetry;
   telemetry.add("temperature", 22.5);
   iot.postTelemetry("sensors", telemetry);

   iot.deepSleep();
   ```

## Configuration & secrets

Deployment-specific values a device needs *before* it can reach the server — WiFi credentials, API endpoint, project name, provisioning token and TLS trust — are **not** set at runtime and **cannot** be changed via `config.json`. They are provided once at build time via `-D` defines, written into NVS by `iot.seedCredentials()` on the first boot (each value only if absent, or on a `seedGeneration` bump), and then left untouched. Because a secretless build passes empty defaults (a no-op), you can flash firmware updates without secrets — e.g. from a public CI — and the device reuses the values already in its NVS.

- **Initial flash:** build with the secrets (see [`examples/secrets-example.ini`](examples/secrets-example.ini)) once to seed NVS.
- **Updates:** build without secrets; NVS is preserved.
- **Reset:** bump `IOT_SEED_GENERATION` (overwrite via a new firmware) or call `iot.factoryReset()` (erase all NVS state).
- **https:** seed a TLS mode/cert via the `tlsMode` / `caCertPem` fields (`CaPin` / `Bundle` / `Insecure`).

See [docs/concepts.md](docs/concepts.md) and [examples/README.md](examples/README.md) for details.

## Server API notes

- **Telemetry format:** the server expects a *flat* JSON object whose values are numbers, e.g. `{"temperature": 22.4, "battery_V": 3.71}`. Non-numeric fields are currently ignored by the telemetry backend (see open points below). The `IotTelemetry` builder guarantees a flat, valid JSON object.
- **Size limits:** telemetry and log bodies are limited to 8 KiB by default (HTTP 413 beyond that); file uploads are limited to 10 MiB.
- **Authentication:** all API calls use a bearer token obtained via provisioning. Tokens are short-lived (7 days by default); the library renews them proactively using the `expiresIn` field of the provisioning response (margin configurable with `api.setDeviceTokenExpiryMargin_s()`). On the device endpoints (telemetry, log, file, forward) the server normalizes *every* authentication failure to **HTTP 401** — devices are deliberately not told why auth failed — which triggers automatic re-provisioning and a single retry. The `/provision` endpoint itself still returns **HTTP 403** for configuration problems that re-provisioning cannot fix (project inactive, device inactive or not approved, HTTP API disabled for the project); this surfaces as a failed `api.updateProvisioningOk()`. Should a device endpoint ever return 403, the library treats it the same way — it keeps the token and does not retry — but current nice4iot emits 403 only from `/provision`.
- **Caching:** configuration and firmware downloads use `ETag`/`If-None-Match` and `Last-Modified`/`If-Modified-Since` for cache validation, so unchanged files are not downloaded again.
- **Connection reuse:** all HTTP(S) requests within a wakeup cycle share a single keep-alive TCP/TLS connection, so the (expensive) TLS handshake is paid only once per cycle. The connection is closed automatically before deep sleep, restart and shutdown (or manually via `api.closeConnection()`).
- **TLS server authentication:** for an `https://` API URL you must choose how the server certificate is verified — pick exactly one:
  - `api.setCACert(caPem)` — pin your server's CA (recommended for a self-hosted nice4iot with a private/self-signed certificate);
  - `api.setCACertBundle()` — verify against the ESP-IDF attested Mozilla root bundle (a browser-like trust store), convenient for servers with a public-CA certificate. Requires the bundle embedded in the build (arduino-esp32 default);
  - `api.setCertInsecure()` — development only; the connection is encrypted but the server identity is *not* verified.

  If none of these is set, the TLS handshake fails with an opaque transport error. The library detects this case and logs a single explicit error (visible at `CORE_DEBUG_LEVEL >= 1`) pointing at the missing trust configuration, and appends the same hint to the transport-error log line.

## Tips

- Read [docs/concepts.md](docs/concepts.md) for the core concepts (wakeup cycle, the four singletons, provisioning, telemetry, config, persistence, TLS trust, `IotResult`).
- Browse the header files of the library, there is some doxygen style documentation.
- To distribute new firmware to the server for OTA updates via ssh, add the following lines to your `platformio.ini` (adjust the target path to your nice4iot data directory):
  ```ini
  upload_protocol = custom
  upload_command = scp $SOURCE myserver.local:/path/to/nice4iot/data/projects/my-project/firmware.bin
  ```
- Reducing logging is crucial to achieve short active periods. Use `CORE_DEBUG_LEVEL=5` for full logs and `2` for production (remember to rebuild everything):
  ```ini
  build_flags = -DCORE_DEBUG_LEVEL=2
  ```
- Firmware update via http (instead of https) requires an IDF SDKCONFIG configuration different from the one which is shipped with `arduino-esp32`. It needs the configuration option `CONFIG_OTA_ALLOW_HTTP=y`. Only firmware updates are affected, other API calls support http as well as https in the standard configuration.

## Development

- Native unit tests: `pio test -e native`
- The CI workflow (GitHub Actions) runs the native tests and builds the example for ESP32 and ESP32-S3 with arduino-esp32 3.x.

## Open points

The following topics are known limitations that are being addressed on the server side in [nice4iot](https://github.com/clausgf/nice4iot), or are planned as future work:

- **Telemetry backend (nice4iot):** non-numeric telemetry fields are silently dropped by the server, and measurements are always timestamped with the server arrival time — devices cannot backfill buffered measurements with their own timestamps. Both are being improved in nice4iot; the library already sends non-numeric system telemetry fields (`time`, `firmware_id`, `firmware_version`, `firmware_sha256`) for backends that support them.
- **Offline buffering of telemetry (to be discussed):** if the server or WiFi is temporarily unreachable, the measurements of a whole wakeup cycle are lost. A small ring buffer in RTC RAM (survives deep sleep) could hold a few telemetry payloads and re-send them on the next successful cycle. This is coupled to the device-timestamp topic above — re-sent measurements need their original timestamp, otherwise they all collapse onto the server arrival time. Design questions still open: buffer location and size, eviction policy, and interaction with the flat-JSON telemetry format.
- **MQTT transport:** nice4iot supports telemetry, logging and file transfer via MQTT for always-on devices. This library currently implements the HTTP transport only, which remains the best fit for deep-sleep cycles; an optional MQTT transport is future work.

## Migration to version 3.x

- **New in 3.4 (non-breaking): DHCP lease caching.** `iot.setDhcpCache(true)` caches the DHCP-assigned IP/gateway/netmask/DNS in RTC RAM and reuses it on the next fast reconnect to skip the DHCP DORA exchange — a static IP without per-device configuration. Off by default; falls back to DHCP on a fast-reconnect timeout and renews via real DHCP every `maxReuse` wakeups (default 20). See [Fast WiFi reconnect](docs/concepts.md#fast-wifi-reconnect).
- **System-telemetry firmware fields changed (3.1–3.3).** The old `firmware_version` field (the composite ESP application descriptor, which in a PlatformIO/Arduino build identifies the framework rather than the sketch) is now sent as **`firmware_id`**. `firmware_version` now carries a single git-derived version string from the consuming project — `git describe --tags --dirty --always`, e.g. `0.10.0`, `0.10.0-3-gcce20b9` or `0.10.0-3-gcce20b9-dirty` — injected at build time by a pre-build script shipped via `library.json`; override with `iot.setFirmwareVersion()` or disable git with `-DIOT_NO_GIT_VERSION`. The `Iot::getFirmwareVersion()` method returns that string; the composite is `Iot::getFirmwareId()`. (The short-lived separate `firmware_commit` field / `getFirmwareCommit()`/`setFirmwareCommit()` from 3.1/3.2 were folded into `firmware_version` and removed.) Update dashboards/queries that referenced `firmware_version`.
- **Configuration is now seeded from build-time defaults into NVS.** The runtime setters `api.setApiUrl()`, `api.setProjectName()`, `api.setProvisioningToken()/setProvisioningTokenIfEmpty()` and the TLS setters `api.setCACert()/setCACertBundle()/setCertInsecure()/setClientCertificateAndKey()` are **removed from the public API**. Instead, call `iot.seedCredentials({...})` once with `-D` defaults; the values (including WiFi credentials and TLS trust) are loaded from NVS by `begin()`. None of these can be changed via `config.json`.
- **`iot.begin(ssid, password)` is replaced by `iot.begin(timeout_ms = 10000)`**, which reads the WiFi credentials from NVS (seeded via `seedCredentials()`). The former no-argument `iot.begin()` (subsystem init only) is now internal.
- **New:** `iot.factoryReset()` erases all persisted NVS state; `IotSeedConfig::seedGeneration` forces a re-seed of changed values via a new firmware.
- **C++20 is now required** (`-std=gnu++20`); the seed struct uses designated initializers.

## Migration to version 2.x

- **Default device name changed from `e32-<mac>` to `e32_<mac>`** (hyphen → underscore). The newest nice4iot validates device and project names against `^[a-zA-Z_][a-zA-Z0-9_]*$`, which forbids the hyphen. Existing devices will provision under the new name and appear as new device records on the server; migrate server-side data or set an explicit device name via `api.setDeviceName()` if you need to keep the old identity.
- **ArduinoJson 7** is now a declared dependency (was an undeclared ArduinoJson 6 requirement before).
- **arduino-esp32 3.x** (ESP-IDF 5) is supported and used in CI; arduino-esp32 2.x should continue to work.
- `apiSetConnectionTimeout()` / `apiSetRequestTimeout()` are deprecated in favor of `setConnectionTimeout_ms()` / `setRequestTimeout_ms()`.
- `IotApi::apiRequest()` now takes the response header keys to collect as a `std::vector<String>` instead of a C array plus count.
- HTTP 403 no longer clears the device token (only 401 does).
- **`iot.postTelemetry()`, `iot.postSystemTelemetry()` and `api.apiForward()` now return `IotResult`** instead of a raw `int` status code. `IotResult` separates success (2xx), HTTP errors and transport errors and converts to `bool` in a boolean context, so `if (!iot.postTelemetry(...))` reads correctly. The raw status is still available via `.httpStatus` / `.transportError`. The low-level `api.apiGet()/apiPost()/apiPut()/apiHead()/apiRequest()` methods still return `int`, which `IotResult` accepts implicitly.
- **`api.uploadFile()` now returns `IotResult`** instead of `bool`, so a failed upload is diagnosable (e.g. `.httpStatus == 413` for an oversized file). `if (!api.uploadFile(...))` keeps working.
- **`config.updateConfig()`, `api.updateFirmware()` and `api.apiCheckForUpdate()` now return `IotResult`** instead of `bool`, so "server unreachable" is distinguishable from "already up to date". For these cache-aware calls HTTP 304 is not an error: test with `IotResult::isOkOrNotModified()` ("no error") and `isNotModified()` ("unchanged"). `if (api.updateFirmware())` still means "new firmware was flashed" (`isOk()`), as the old `bool` did. A firmware download/flash that fails after an update was found reports `.httpStatus == IotResult::STATUS_UPDATE_FAILED`.
- **`api.updateProvisioning()`** is a new variant of `api.updateProvisioningOk()` that returns `IotResult` so a headless device can tell apart the failure causes: a transport/TLS error (`isTransportError()`), a rejected provisioning (`.httpStatus == 403`), an invalid token (`.httpStatus == 401`), a missing provisioning token (`.httpStatus == IotResult::STATUS_NO_PROVISIONING_TOKEN`) or a malformed response body (`.httpStatus == IotResult::STATUS_MALFORMED_RESPONSE`). `api.updateProvisioningOk()` is unchanged (now a thin wrapper returning `bool`).
- **Fast WiFi reconnect is on by default** and `connectWifi()` now sets `WiFi.persistent(false)`. The first connect after power-on does a normal full scan (empty cache), so there is no regression; later wakeups reuse the cached BSSID/channel and fall back automatically on any mismatch. Disable via `iot.setFastReconnect(false)`.
