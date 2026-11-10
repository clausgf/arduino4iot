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
- **Remote logging:** log messages are buffered in RAM and sent to the server in a single request at the end of the wakeup cycle (configurable via `logger.setBuffered()`, forced via `logger.flush()`); large bodies are split to respect the server's size limit. Lines that cannot be sent — offline, or after `iot.radioOff()` — are kept in RTC RAM and sent with a later cycle.
- **Radio off:** `iot.radioOff()` flushes the log, closes the connection and powers the radio down for a long stretch without network, such as an e-paper refresh.
- **Configuration:** download per-device or project-wide configuration files with ETag caching; values are persisted in NVRAM. Values declare title, description, limits and allowed strings once; the library enforces them and publishes a matching `config.schema.json` for the server's form.
- **Conditional GET:** `api.getCached()` fetches an application resource only if it changed, hands back the validators for the caller to commit once the resource was used, and reports the `Cache-Control` max-age.
- **OTA firmware updates:** ETag-based update check and streaming firmware download.
- **File upload:** `iot.files.upload()` pushes files to the device-specific storage on the server.
- **Forwarding:** `iot.forward.get()` proxies requests to upstream services configured in the project settings.
- **Fast WiFi connect:** to cut the radio-on time of a wakeup, the last-good AP (BSSID + channel) is cached in RTC RAM for a scan-free reconnect (on by default, `iot.wifi().setFastReconnect()`); DHCP can be skipped either with a fixed static IP (`iot.wifi().setStaticIp()`) or by caching the DHCP-assigned lease in RTC and reusing it automatically (`iot.wifi().setDhcpCache()`, opt-in). All fall back automatically to a full-scan/DHCP connect. The connect duration is exposed as `wifi_connect_ms` telemetry. The transmit power can be limited (`iot.wifi().setMaxTxPower_qdBm()`, server-side `wifi_tx_qdbm`) for boards with a weak antenna match or batteries that brown out on transmit peaks.
- **System management:** deep sleep cycle bookkeeping, watchdog supervision, battery monitoring with undervoltage shutdown, escalating panic/backoff strategy.
- **Application state:** `IotRtcSlot<T>` keeps a struct in RTC RAM across deep sleep and resets it when it did not survive; an own `IotNvsStore` namespace holds what must survive a power loss.

## API overview

Five global objects, each with one job. Everything a sketch calls starts with
one of them:

| Task | Call |
|---|---|
| Wakeup cycle | `iot.begin()`, `iot.deepSleep()`, `iot.radioOff()`, `iot.panic()`, `iot.restart()`, `iot.shutdown()` |
| Failure reactions | `iot.setPanicHandler()`, `iot.defaultPanicHandler()`, `iot.setUndervoltageHandler()`, `iot.defaultUndervoltageHandler()`, `iot.setBatteryMin_mV()`, `iot.setBatterySleep_s()` |
| Bootstrap and reset | `iot.seedCredentials()`, `iot.factoryReset()`, `iot.clearProvisioning()`, `iot.setNoCredentialsHandler()`, `iot.setSetupTrigger()` |
| Hardware ports | `iot.setNetwork()`, `iot.setClock()`, `iot.setBattery()`, `iot.setPower()`, `iot.wifi()` |
| Device info | `iot.deviceId()`, `iot.macAddress()`, `iot.resetReason()`, `iot.panicCount()`, `iot.crashCount()`, `iot.undervoltageCount()`, `iot.boardId()`, `iot.firmwareVersion()`, `iot.isTimePlausible()`, `iot.battery_mV()` |
| Telemetry | `iot.telemetry.post()`, `iot.telemetry.postSystem()` |
| Files | `iot.files.upload()`, `iot.files.getCached()` |
| Forwarding | `iot.forward.get()`, `iot.forward.getCached()` |
| Firmware update | `iot.ota.update()`, `iot.ota.validators()` |
| HTTP to the server | `api.get()`, `api.post()`, `api.put()`, `api.head()`, `api.send()`, `api.getCached()` |
| Provisioning | `api.updateProvisioning()` |
| Configuration | `config.updateConfig()`, `IotConfigValue<T>`, `config.getInt()`/`getString()`/... |
| Logging | `logger.info()`/`warn()`/`error()`, `logger.flush()` |
| Application state | `IotRtcSlot<T>` (RTC RAM), `IotNvsStore` (NVS) |

`iot.telemetry`, `iot.files`, `iot.forward` and `iot.ota` are the nice4iot
endpoints; a method an application never calls is not linked, so grouping them
under `iot` costs nothing. `api` is the plain HTTP layer underneath, for paths
the library has no group for.

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
   if (!iot.begin())                     // WiFi from NVS, subsystems, clock
   {
       iot.panic("*** PANIC *** network connection failed");
   }
   if (!api.updateProvisioning())
   {
       iot.panic("*** PANIC *** Provisioning failed");
   }
   config.updateConfig();
   iot.resetWatchdog();
   iot.ota.update();
   iot.telemetry.postSystem();

   // measure something
   IotTelemetry telemetry;
   telemetry.add("temperature", 22.5);
   iot.telemetry.post("sensors", telemetry);

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
- **Authentication:** all API calls use a bearer token obtained via provisioning. Tokens are short-lived (7 days by default); the library renews them proactively using the `expiresIn` field of the provisioning response (margin configurable with `api.setDeviceTokenExpiryMargin_s()`). On the device endpoints (telemetry, log, file, forward) the server normalizes *every* authentication failure to **HTTP 401** — devices are deliberately not told why auth failed — which triggers automatic re-provisioning and a single retry. The `/provision` endpoint itself still returns **HTTP 403** for configuration problems that re-provisioning cannot fix (project inactive, device inactive or not approved, HTTP API disabled for the project); this surfaces as `api.updateProvisioning()` returning `.httpStatus == 403`. Should a device endpoint ever return 403, the library treats it the same way — it keeps the token and does not retry — but current nice4iot emits 403 only from `/provision`.
- **Caching:** configuration and firmware downloads use `ETag`/`If-None-Match` and `Last-Modified`/`If-Modified-Since` for cache validation, so unchanged files are not downloaded again. `api.getCached()` offers the same for an application's own resources and returns the `Cache-Control` max-age; see [Conditional GET](docs/concepts.md#conditional-get-for-application-resources).
- **Connection reuse:** all HTTP(S) requests within a wakeup cycle share a single keep-alive TCP/TLS connection, so the (expensive) TLS handshake is paid only once per cycle. The connection is closed automatically before deep sleep, restart and shutdown (or manually via `api.closeConnection()`).
- **TLS server authentication:** for an `https://` API URL you must choose how the server certificate is verified — pick exactly one:
  - `api.setCACert(caPem)` — pin your server's CA (recommended for a self-hosted nice4iot with a private/self-signed certificate);
  - `api.setCACertBundle()` — verify against the ESP-IDF attested Mozilla root bundle (a browser-like trust store), convenient for servers with a public-CA certificate. Requires the bundle embedded in the build (arduino-esp32 default);
  - `api.setCertInsecure()` — development only; the connection is encrypted but the server identity is *not* verified.

  If none of these is set, the TLS handshake fails with an opaque transport error. The library detects this case and logs a single explicit error (visible at `CORE_DEBUG_LEVEL >= 1`) pointing at the missing trust configuration, and appends the same hint to the transport-error log line.

## Tips

- Read [docs/concepts.md](docs/concepts.md) for the core concepts (wakeup cycle, the five singletons, provisioning, telemetry, config, persistence, TLS trust, `IotResult`).
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
- For a network with a MAC allow list, read the device's MAC before its first connect: over USB with `pio pkg exec -p tool-esptoolpy -- esptool.py --port <port> read_mac`, or on the setup page of the AP provisioning, which a device with credentials opens when BOOT is pressed within 2 s after a reset (`iot.setSetupTrigger()`). See [MAC address for allow lists](docs/concepts.md#first-time-provisioning-softap--captive-portal).
- Firmware update via http (instead of https) requires an IDF SDKCONFIG configuration different from the one which is shipped with `arduino-esp32`. It needs the configuration option `CONFIG_OTA_ALLOW_HTTP=y`. Only firmware updates are affected, other API calls support http as well as https in the standard configuration.

## Development

- Native unit tests: `pio test -e native`
- The CI workflow (GitHub Actions) runs the native tests and builds the example for ESP32 and ESP32-S3 with arduino-esp32 3.x.

## Open points

The following topics are known limitations that are being addressed on the server side in [nice4iot](https://github.com/clausgf/nice4iot), or are planned as future work:

- **Telemetry backend (nice4iot):** non-numeric telemetry fields are silently dropped by the server, and measurements are always timestamped with the server arrival time — devices cannot backfill buffered measurements with their own timestamps. Both are being improved in nice4iot; the library already sends non-numeric system telemetry fields (`time`, `firmware_id`, `firmware_version`, `firmware_sha256`) for backends that support them.
- **Offline buffering of telemetry (to be discussed):** if the server or WiFi is temporarily unreachable, the measurements of a whole wakeup cycle are lost. A small ring buffer in RTC RAM (survives deep sleep) could hold a few telemetry payloads and re-send them on the next successful cycle. This is coupled to the device-timestamp topic above — re-sent measurements need their original timestamp, otherwise they all collapse onto the server arrival time. Design questions still open: buffer location and size, eviction policy, and interaction with the flat-JSON telemetry format.
- **MQTT transport:** nice4iot supports telemetry, logging and file transfer via MQTT for always-on devices. This library currently implements the HTTP transport only, which remains the best fit for deep-sleep cycles; an optional MQTT transport is future work.

## Migration to version 4.x

Version 4 restructures the library around a small set of **ports** (`IotStore`,
`IotClock`, `IotNetwork`, `IotBattery`, `IotPower` in `iot_ports.h`) with ESP32
adapters in `iot_esp_ports.h`. Optional ports are held as pointers that may be
`nullptr` — a mains-powered device passes no `IotBattery`, a device that needs
no wall-clock time no `IotClock` — and the core names no optional adapter, so
the linker drops every one an application does not reference.

- **System telemetry fields in snake case (action required for dashboards).**
  `lastSleep_s` → `last_sleep_s`, `panicSleep_s` → `panic_sleep_s`. New fields:
  `reset_reason`, `panic_count`, `crash_count`, `undervoltage_count`, see
  [Telemetry](docs/concepts.md#telemetry).
- **`config.json` keys renamed (action required).** One key now names a value in
  `config.json`, in NVS and in the free-form accessors, so every key must fit the
  15-character NVS limit. Four were too long:
  `battery_offset_mv` → **`battery_offs_mv`**, `panic_sleep_init_s` →
  **`panic_init_s`**, `panic_sleep_factor` → **`panic_factor`**,
  `panic_sleep_max_s` → **`panic_max_s`**. Update your `config.json`; an
  unrecognized key is logged and skipped. Because the NVS keys are now the
  config keys, the mirror of *all* values is rebuilt on the first
  `updateConfig()` after the upgrade — until then the compile-time defaults
  apply.
- **Configuration precedence is now explicit:** compile-time default < NVS
  mirror < `config.json` < `set()` at runtime. A value the application sets
  explicitly (e.g. `iot.setSleepDuration_s()` in `setup()`) is no longer
  silently overwritten by `begin()` or `updateConfig()`; the server value is
  still mirrored to NVS and applies from the next boot. See
  [Configuration](docs/concepts.md#configuration-runtime-configjson).
- **Four `config.json` keys are removed:** `ntp_server1`, `ntp_server2`,
  `ntp_resync_s` and `ntp_timeout_ms` (never worth turning per deployment from
  the server) and `watchdog_s` (a value set too low from the server bricks a
  fleet, and a device that cannot boot cannot fetch a corrected file). None was
  ever seeded, so nothing else is affected. Set them on the object instead:
  `IotNtpClock::setServers()` / `setResyncInterval_s()`, `iot.setClockSyncTimeout_ms()`,
  and `iot.startWatchdog(timeout_s)` before `iot.begin()`, which now remembers
  the timeout it was given. Stale keys left in the `iot-cfg` namespace are
  ignored and disappear on `factoryReset()`; a `config.json` still listing them
  logs them as unknown and applies the rest. What remains configurable is the
  operational set: `log_level`, `sleep_s`, `battery_min_mv`, `panic_init_s`,
  `panic_factor`, `panic_max_s`, `battery_sleep_s`, `wifi_tx_qdbm`.
- **Internal headers moved from `include/` to `src/`** (`iot_text.h`,
  `iot_ap_text.h`, `iot_ota_internal.h`, `iot_log.h`). `include/` is the public
  interface; none of these appear in a public header or a sketch.
- **`api.updateFirmware()` → `iot.ota.update()`**, together with
  `getFirmwareHttpEtag()/Date()` → `iot.ota.validators()`. The OTA
  downloader is ESP-specific, and `IotApi` is now Arduino-free: it builds URLs,
  merges headers and handles the 401 retry over an `IotHttp` port, while sockets
  and TLS belong to `IotEspHttp`. That is what makes the transport testable —
  18 native tests now cover the retry, header precedence and the provisioning
  failure modes. `api.apiCheckForUpdate()` is removed (it had no callers).
- **The library's string type is `std::string` (action required).** `String` is
  defined by the Arduino core, so any file naming it cannot be compiled — and
  therefore not tested — on the host. Public signatures that took or returned
  `String` now take `std::string`: `iot.telemetry.post()`, `iot.deviceId()`,
  `iot.timeIso()`, `iot.firmwareVersion()/Id()/Sha256()/BoardId()`, all of
  `api.*`, `logger.*`, `iot.forward.get()`/`iot.files.upload()`. String *literals* keep
  working unchanged, because `std::string` constructs implicitly from
  `const char*`; only a sketch holding an Arduino `String` needs `.c_str()`:
  ```cpp
  iot.telemetry.post("sensors", t);                       // a literal: fine
  iot.telemetry.post(kindAsArduinoString.c_str(), t);     // String: .c_str()
  ```
  Inside the library `String` survives only at Arduino call boundaries
  (`Preferences`, `HTTPClient`), converted on the spot.
- **The clock no longer synchronizes by default (action required if you need
  the time).** `iot.begin()` used to run SNTP unconditionally. The default
  `IotClock` is now `IotSystemClock`, which reads the system time and never
  syncs — so no firmware links SNTP code it does not use. TLS does not need it
  (arduino-esp32 does not check certificate validity dates); timestamps, renewing
  the device token before it expires and the application's own time logic do:
  ```cpp
  static IotNtpClock ntpClock(iotRtcState().ntpLastSyncTime);
  iot.setClock(ntpClock);
  ```
  `begin()` logs a warning when the clock is implausible. `iot.setNtp()` is gone;
  configure servers and interval on the `IotNtpClock`. The lwIP SNTP daemon
  fallback is removed — the one-shot query is the only path.
- **`iot.begin()` now reports the network only.** It used to return "WiFi
  connected AND NTP synced", which conflated two unrelated failures. A device on
  `http://` works with an unsynchronized clock; check `iot.isTimePlausible()`
  where it matters.
- **Battery measurement is an adapter (action required if you used it).**
  `iot.setBattery(pin, factor, divider, offset)` and the `battery_pin`,
  `battery_factor`, `battery_divider`, `battery_offs_mv` config keys are gone.
  Attach an `IotBattery` instead:
  ```cpp
  static IotAdcBattery battery({ .pin = 34, .factor = 2, .divider = 1, .samples = 4 });
  iot.setBattery(battery);
  ```
  `IotAdcBattery` adds multi-sample averaging and an optional enable pin; an I2C
  fuel gauge goes through `IotCallbackBattery`. Without a battery there is no
  `battery_V` telemetry and no undervoltage check. `battery_min_mv` is unchanged.
- **The LED helpers are removed.** `iot.setLedPin()`, `iot.setLed()` and the
  `led_pin` config key were a `digitalWrite()` wrapper; do it in the sketch.
- **`api.apiForward()` → `iot.forward.get()` and `api.uploadFile()` →
  `iot.files.upload()`**. The transport no longer carries the server's endpoint
  map, and both are linked only when referenced.
- **`api.apiSetConnectionTimeout()` / `api.apiSetRequestTimeout()` are removed**
  (deprecated since 2.x); use `setConnectTimeout_ms()` / `setRequestTimeout_ms()`.
- **`iot.setStaticIp()`, `iot.setFastReconnect()` and `iot.setDhcpCache()` →
  `iot.wifi().setStaticIp()` etc.** (with `#include <iot_esp_ports.h>`). They
  tune the built-in `IotWifiNetwork`; the facade no longer carries WiFi details
  of a port that may be replaced via `iot.setNetwork()`. `iot.connectWifi()`
  keeps working and also uses the built-in adapter. `iot.deviceId()` reads the MAC from the eFuse and therefore works
  without an established connection — it used to return an empty string when the
  connect had failed, which silently broke every API URL on that path.
- **`config.getConfigInt32/Bool/String()` → `config.getInt/getBool/getString()`**;
  `setConfigXxx()` is gone (it wrote the config mirror behind the back of the
  precedence rules — set a declared `IotConfigValue`, or keep application data
  in an own `IotNvsStore`); `getConfigHttpEtag()/getConfigHttpDate()` →
  `config.validators()`, and `config.isMirrored()` tells whether a config.json
  was ever applied. `IotConfigValue<String>` is now
  `IotConfigValue<std::string>`, and `get()` returns a reference.
- **New: `IotConfigMeta`** on `IotConfigValue` (title, description, min/max,
  `enumValues`), enforced on values from `config.json` and NVS, and
  `config.publishSchema()`, which uploads a generated `config.schema.json`
  once per changed schema; `config.setLayout({ { cvA, cvB }, { cvC } })`
  arranges the server's form from the values (the library appends rows for
  its own), `config.setSchemaExtras()` adds other top-level members. The
  library's own values are now limited:
  `log_level` 0–5, `sleep_s` 1 s – 7 d, `battery_min_mv` −1–10000,
  `panic_init_s`/`panic_max_s` 1 s – 7 d, `panic_factor` 1–16; a
  `config.json` outside these is clamped. See
  [Metadata, limits and the schema](docs/concepts.md#metadata-limits-and-the-schema).
- **New: `iot.radioOff()`** and offline logging in RTC RAM (`IOT_RTC_LOG_SIZE`,
  default 1536 bytes). `IotNetwork::disconnect()` now powers the interface
  down; `IotWifiNetwork` sets `WIFI_OFF`. `logger.flush()` returns
  `IotResult` instead of an `int`; offline, `flush()`
  keeps the buffer instead of dropping it. See
  [Offline logging](docs/concepts.md#offline-radio-off-logs-in-rtc-ram).
- **New: `IotRtcSlot<T>`** (`iot_rtc_slot.h`) for application state in RTC RAM,
  validated against power-on garbage and layout changes, and a documented
  pattern for application NVS through an own `IotNvsStore` namespace. See
  [Application data](docs/concepts.md#application-data).
- **New: `api.getCached()`** (types in `iot_cache.h`), a conditional GET for an
  application's own resources, with `IotStoredValidators` to keep the
  validators in an `IotStore` and `IotRtcValidators<N>` for RTC RAM. `config.updateConfig()` is built on it.
- **`config.begin()` takes the store.** The overload binding the default NVS
  namespace is gone, so the configuration service names no adapter;
  `iot.begin()` binds `config` to `iot-cfg` as before. A sketch calling
  `config.begin()` itself passes an `IotStore`.
- **AP provisioning is opt-in.** `iot.begin()` no longer calls
  `apProvisioning.run()` itself; register it with
  `iot.setNoCredentialsHandler([]{ apProvisioning.run(); })`. Without a handler,
  a device with no seeded WiFi SSID logs an error and shuts down. This keeps the
  ~42 KB of `DNSServer`/`WebServer` out of firmware that never opens a setup AP.
  See [`examples/ap_provisioning/`](examples/ap_provisioning/).
- **The facade drops what the ports already offer, and the panic API shrinks.**
  `iot.connectWifi()` → `iot.wifi().setCredentials()` + `iot.network().connect()`;
  `iot.getWifiConnectDuration_ms()` → `iot.network().lastConnectDuration_ms()`;
  `iot.getNtpLastSyncTime()` → `iot.clock().lastSyncTime()`; `iot.syncNtpTime()`
  is internal to `begin()`, whose budget is `iot.setClockSyncTimeout_ms()`
  (was `setNtpTimeout_ms()`). `iot.timeIso()` takes an optional epoch.
  `iot.setPanic(init, factor, max)` → `iot.setBackoff({ .initialDuration_s = …,
  .factor = …, .maxDuration_s = … })`; `panicEarly()`,
  `escalatingSleepPanicHandler()` and `getPanicSleepDuration_s()` are internal
  (the backoff is reported as `panic_sleep_s`). `api.updateProvisioningOk()`
  → `api.updateProvisioning()` in a boolean context.
- **`config.store()`, `reload()` and `find()` are internal**, as are
  `IotConfigValueBase::loadFrom()`, `applyJson()`, `next()`, `owner()` and the
  former `IotConfigKind` / `iotConfigValueListHead()`. `config.begin(store)`
  stays public for further `IotConfig` instances, `applyJsonBody()` for a
  configuration that arrives another way.
- **`IotRtcState` is plain data in an `IotRtcSlot`.** Its `MAGIC`, `isValid()`
  and `reset()` are gone; the slot detects a power-on or a changed layout from
  the struct's size. "Not in backoff" is now 0 instead of -1, which the
  `panic_sleep_s` telemetry field shows. After this update every device starts
  over once: boot count from 1, any running backoff cleared.
- **New: `IotRtcTelemetry<N>`** for values known only after the radio is off:
  kept in the application's `IotRtcSlot` and added to the next cycle's post of
  the same kind as `last_<key>` (`iot.telemetry.post(kind, t, rtc->next)`).
- **New: `IotResult::code()`** — the HTTP status, or the negative transport
  error, as one number for logs and telemetry. **`IotRtcValidators<N>`** keeps
  cache validators in RTC RAM inside an `IotRtcSlot`. **`iot_config_keys`**
  names the library's own config keys (a layout does not need them:
  `config.setLayout()` places the library's values itself).
- **`config.setSchemaExtras()` takes a `std::string` and copies it**; there is
  no lifetime to keep. A layout goes through `config.setLayout()` now.
- **Getters drop the `get` prefix**, like the ports already did:
  `iot.getDeviceId()` → `iot.deviceId()`, likewise `boardId()`,
  `firmwareVersion()`, `firmwareId()`, `firmwareSha256()`, `bootCount()`,
  `timeIso()`, `lastSleepDuration_s()`, `iot.getBatteryVoltage_mV()` →
  `iot.battery_mV()`, `iot.getActiveDuration_ms()` →
  `iot.lastActiveDuration_ms()` (it is the previous cycle's), and
  `api.getDeviceTokenExpiresAt()` → `api.deviceTokenExpiresAt()`. Setters keep
  `set`; keyed lookups (`config.getInt("key")`, `IotStore::getString()`) and
  `getCached()` (the HTTP verb) keep `get`.
- **One rule for units and types:** the unit is always a suffix, durations in
  milliseconds are `uint32_t`, in seconds `int32_t`, points in time `int64_t`
  unix seconds. Visible changes: `IotBattery::readMilliVolts()` → `read_mV()`
  (an own `IotBattery` overrides the new name); `api.setConnectionTimeout_ms()`
  → `setConnectTimeout_ms()`; `setRequestTimeout_ms()` takes `uint32_t` (the
  ESP adapter clamps to HTTPClient's 65 535 ms); `IotPower::deepSleep()` takes
  `int32_t`. Most other signatures only widen `int` to `int32_t`.
- **Logger levels are `IotLogger::Level`** (`Error`, `Warning`, `Info`, `Debug`,
  `Verbose`, `All`) instead of `IotLogger::IOT_LOGLEVEL_*`; `setLogLevel()` →
  `setLevel()`, `logf()` → `log()`. `begin()`, `end()`, `setNetwork()`,
  `logv()` and `postLog()` are internal — `iot` drives them. The numeric
  `log_level` config values are unchanged.
- **`api.store()`, `deviceToken()`, `setDeviceToken()`, `tlsConfig()`,
  `mergeHeaders()` and `urlForPath()` are no longer public.** They were
  plumbing for the OTA download; a token is issued by provisioning, never set
  by the application. Keep application data in an own `IotNvsStore`.
- **Endpoint functions move into groups on `iot`** (see [API overview](#api-overview)).
  Coming from 3.x, the bullets above already name the result; for code written
  against an earlier 4.0 development state:
  `iot.postTelemetry()` → `iot.telemetry.post()`, `iot.postSystemTelemetry()` →
  `iot.telemetry.postSystem()`, `iotUploadFile()` → `iot.files.upload()`,
  `iotForward()` → `iot.forward.get()`, `iotUpdateFirmware()` →
  `iot.ota.update()`, `iotFirmwareHttpEtag()/Date()` →
  `iot.ota.validators()`, `iotGetCached()` → `api.getCached()`. Including
  `iot_endpoints.h` is no longer necessary; `iot.h` brings it.
- **`api.apiGet/apiPost/apiPut/apiHead/apiRequest()` → `api.get/post/put/head/send()`.**
  They return an `IotApiResponse` (`.result`, `.body`, `.header(name)`) instead
  of a raw `int` with out-parameters, and take the optional parts as named
  fields: `api.apiGet(body, headers, path, { "ETag" })` becomes
  `IotApiResponse r = api.get(path, { .collectHeaders = { "ETag" } });`. GET no
  longer takes a body. `iotForward()` returns an `IotApiResponse` the same way.
- **`deepSleep()`, `restart()` and `shutdown()` lose their `bool panic`
  parameter.** They end a cycle cleanly and clear the escalating backoff; a
  failure exit goes through `iot.panic()`. `iot.deepSleep(s, false)` becomes
  `iot.deepSleep(s)`; a call passing `true` becomes `iot.panic("...")`.
- **`setDeepSleepHandler()`/`setRestartHandler()`/`setShutdownHandler()` are
  replaced** by `iot.setPower(IotPower&)` (replace the mechanism) and
  `iot.addExitHook()` (run something at every exit, including the backoff path).
- **`IotPersistentValue`, `setPreferedPersistentStorage()` and the `iot-var` NVS
  namespace are gone.** State that must survive a power loss goes through
  `IotStore`; state that only has to survive deep sleep lives in `IotRtcState`
  (`RTC_NOINIT_ATTR`). Which one a value uses is decided where it is declared,
  not by a global runtime switch. Boot count, active/sleep accounting and the
  failure backoff therefore no longer write flash on every wakeup.
- **`getResetReason()`, `resetReasonToString()`, `getWakeupCause()`,
  `wakeupCauseToString()` and `waitUntil()` are removed** — use
  `IotPower::resetReason()`/`wakeupCause()` and
  `iotResetReasonToString()`/`iotWakeupCauseToString()`.
- **An abnormal reset is acted on in `iot.begin()`, not in the `Iot`
  constructor**, so the diagnosis appears on a serial console the sketch has had
  a chance to open. The boot counter is incremented in `begin()` too, so cycles
  that fail to connect are counted.
- **`setPanic()`'s documented default factor was 3 while the effective default
  was 2**; the signature now says 2. Behaviour is unchanged.

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
