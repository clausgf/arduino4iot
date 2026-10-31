# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`arduino4iot` is the ESP32/Arduino **client library** half of the *x4iot* system; the
server half is [nice4iot](https://github.com/clausgf/nice4iot) (kept locally at
`~/git/nice4iot`, read-only from here). The library targets **battery-powered,
deep-sleep devices**: the design goal is a complete wakeup cycle (WiFi + NTP +
provisioning + config/OTA check + telemetry + log flush) in well under a second.

## Commands

```bash
pio test -e native                 # run all native (host) unit tests
pio test -e native -f test_ports    # run one suite: test_result | test_telemetry |
                                    # test_text | test_ap_text | test_ports |
                                    # test_lifecycle | test_token | test_config |
                                    # test_api | test_cache | test_rtc_slot |
                                    # test_rtc_log
```

- **ESP32 builds are never done from this repo directly.** There is no ESP32 env in
  `platformio.ini` (its only env is `native`, for tests). The library is compiled on
  target only via `pio ci` — CI does this for `esp32dev` and `esp32-s3-devkitc-1`
  using the **pioarduino** platform (the PlatformIO registry `espressif32` ships only
  arduino-esp32 2.x; this library needs 3.x / ESP-IDF 5 and C++20). To reproduce a
  target build locally, mirror the `pio ci` invocation in `.github/workflows/ci.yml`.
  CI builds **both** examples per board: `examples/main.cpp` (minimal) and
  `examples/ap_provisioning/main.cpp` (every optional adapter wired in). The
  second exists so the opt-in paths keep being compiled; the flash difference
  between the two is what the optional adapters cost.
- Native tests require C++20 (`-std=gnu++20` in `platformio.ini`); on target the
  arduino-esp32 framework default (`gnu++2b`) suffices.

## Architecture

`docs/concepts.md` is the authoritative design document — **read it before
non-trivial changes**; it covers the wakeup cycle, the ports, clock and battery,
provisioning/tokens, telemetry shape, seeding, persistence tiers, TLS trust and
the `IotResult` contract.

### Layers

The library is split along one line: **what can be tested on the host** and what
cannot. Everything that can, is Arduino-free and uses `std::string`.

| Layer | Files | Arduino? |
|---|---|---|
| Ports (interfaces + contracts) | `iot_ports.h` | no |
| Pure types and services | `iot_result` `iot_telemetry` `iot_text` `iot_ap_text` `iot_rtc_log.h` `iot_rtc_state` `iot_rtc_slot.h` `iot_lifecycle` `iot_token` `iot_config` `iot_seed` `iot_api` `iot_cache` `iot_names.h` `iot_endpoints.h` `iot_logger.h` | no |
| ESP32 adapters | `iot_esp_ports.{h,cpp}` `iot_ap` `iot_ota_internal` `iot_endpoints.cpp` | yes |
| Facade | `iot.{h,cpp}` | yes |

The services are Arduino-free including their implementations (`iot_api`,
`iot_cache`, `iot_config`, `iot_token`, `iot_lifecycle`, `iot_ports`, `iot_result`,
`iot_seed`), which is why `platformio.ini` compiles `src/` into the native test
build (`test_build_src = yes`). Its `build_src_filter` includes everything and
**excludes** the Arduino sources, so a new source file is tested by default and
one that names Arduino breaks the native build until it is made Arduino-free or
excluded there. Use `iot_log.h` rather than Arduino's logging macros.

**Rules that keep this true — check them when adding code:**

1. A header in the "no Arduino" rows must compile standalone:
   `g++ -std=gnu++20 -fsyntax-only -I include -I <ArduinoJson/src> -x c++ <header>`.
   Naming `String`, `Preferences`, `WiFi` or `HTTPClient` breaks it.
2. Use `std::string` everywhere; `String` only at an Arduino call boundary
   (`Preferences`, `HTTPClient`, `WebServer`), converted on the spot. Keys stay
   `const char*`.
3. **The core never names an optional adapter.** That is what lets the linker
   drop what an application does not use — `IotAp` (~42 KB), `IotNtpClock`,
   `IotAdcBattery`. Verify with `nm -C firmware.elf | grep <Class>::`.
4. Test doubles live in `test/iot_fakes.h`, included as `"../iot_fakes.h"` — not
   in `include/`, so firmware cannot reach them.
5. `include/` is the public interface. A header that no public header includes
   and no sketch uses belongs in `src/` (`iot_text.h`, `iot_ap_text.h`,
   `iot_ota_internal.h`, `iot_log.h`, `iot_rtc_log.h`); the native build has `-I src` so tests
   still reach them.

### Ports and their adapters

| Port | Default in `Iot` | Adapters |
|---|---|---|
| `IotStore` | `IotNvsStore` (mandatory) | — |
| `IotHttp` | `IotEspHttp` (mandatory) | a fake in tests |
| `IotPower` | `IotEspPower` (mandatory) | — |
| `IotNetwork` | `IotWifiNetwork` | Ethernet/LTE would slot in here |
| `IotClock` | `IotSystemClock` — **never synchronizes** | `IotNtpClock` (opt-in; TLS does not need it — mbedTLS is built without `MBEDTLS_HAVE_TIME_DATE`) |
| `IotBattery` | none (`nullptr`) | `IotAdcBattery`, `IotCallbackBattery` |

An absent optional port is not an error: each port documents how the library
degrades without it.

### Global singletons (declared `extern` in their headers)

Server paths and NVS keys live only in `iot_names.h` (`iot_paths`,
`iot_nvs_keys`); never as a literal or a class constant elsewhere. New
user-facing calls go where a sketch looks for them: lifecycle on `iot`,
a nice4iot endpoint into its group on `iot` (`iot.telemetry`, `iot.files`,
`iot.forward`, `iot.ota`), plain HTTP on `api`. No new free `iot*()` functions.

| Object          | Header/impl        | Responsibility |
|-----------------|--------------------|----------------|
| `iot`           | `iot.{h,cpp}`      | Lifecycle façade: drives the ports, deep sleep, radio off, watchdog, undervoltage, panic/backoff. Endpoint groups `iot.telemetry/files/forward/ota` (declared in `iot_endpoints.h`). |
| `api`           | `iot_api.{h,cpp}`  | URL building, header merging, the 401 retry, `getCached` — over an `IotHttp`. Sockets and TLS are the adapter's, auth is `IotTokenManager`'s, the endpoint groups and OTA are `iot_endpoints`'. |
| `config`        | `iot_config.{h,cpp}` | Server config mirrored into an `IotStore` (ETag-cached). |
| `logger`        | `iot_logger.{h,cpp}` | Buffered remote logging + serial logging. |
| `apProvisioning`| `iot_ap.{h,cpp}`   | **Opt-in**, never referenced by the core — see below. |

### Persistence: two tiers, decided per value

- `IotStore` (NVS) for what must survive a power loss: tokens, cache validators,
  seeded config, the config mirror. Writes are suppressed when unchanged.
- `IotRtcState` (`RTC_NOINIT_ATTR`, a plain struct, not a port) for what only has
  to survive deep sleep: boot count, sleep accounting, failure backoff, WiFi
  caches. Costs no flash cycles.

Which one a value uses is decided **where it is declared**. There is no global
switch.

### Configuration values

One key names a value in `config.json`, in NVS and in `config.getInt/...`, so it
must fit `IotStore::MAX_KEY_LENGTH` (15). Precedence, weakest first:
**compile-time default < NVS mirror < `config.json` < `set()` at runtime** — an
explicit `set()` is not overwritten by `begin()` or `updateConfig()`, while the
server value is still mirrored for the next boot. `IotConfigValue`s register
themselves through an intrusive list with a constant-initialized head, so
registration during static init touches no constructed object. An optional
`IotConfigMeta` (title, description, min/max, `enumValues` as `"a|b|c"`) is
enforced on values from `config.json` and NVS (clamp / reject) but not on
`set()`, and feeds `config.schemaJson()`/`publishSchema()`. Keep schema code
out of virtual functions: it must stay unlinked unless `publishSchema()` is
called.

### Two value types (Arduino-free, unit-tested on host — see `test/`)

- `IotResult` (`iot_result.h`): the return type of high-level operations. Separates
  **Ok (2xx) / HttpError / TransportError**, is implicitly constructible from a
  raw transport `int`, and carries synthetic statuses ≥ 600
  (`STATUS_NO_PROVISIONING_TOKEN`, `STATUS_MALFORMED_RESPONSE`,
  `STATUS_UPDATE_FAILED`). **Design rule:** every operation reports an
  `IotResult`; the request methods `api.send/get/head/post/put` return it inside an
  `IotApiResponse` (result, body, collected headers) and take an
  `IotApiRequest`/`IotApiOptions` with named fields — no raw `int`, no out-params. For cache-aware calls (`updateConfig`, `iot.ota.update`,
  `api.getCached`) **HTTP 304 is success** — test with `isNotModified()` /
  `isOkOrNotModified()`, not plain `operator bool` (which is strict-2xx).
- `IotTelemetry` (`iot_telemetry.h`): builder guaranteeing the server's required
  flat-JSON-of-numbers shape.

### First-time provisioning (opt-in)

`Iot::begin()` calls the handler from `iot.setNoCredentialsHandler()` when no
WiFi SSID is seeded, and shuts down without one. `iot_ap.h` is **not** included
by the core: naming it would keep `DNSServer`/`WebServer` (~42 KB) in every
firmware. The dependency runs one way, `iot_ap` → `iot`.

### Configuration seeding model (the key non-obvious concept)

Bootstrap values a device needs *before* it can reach the server (WiFi creds, API
URL, project, provisioning token, TLS trust) live in **NVS**, not `config.json`.
They are provided at build time via `-D` defines and written by
`iot.seedCredentials({...})` (`iot_seed.h`, `IotSeedConfig` designated initializers),
**each value only if its NVS key is absent** or when `seedGeneration` is bumped.
Consequence: a **secretless build passes empty defaults (a no-op)**, so firmware
updates built without secrets (e.g. public CI) preserve the device's existing NVS —
this is the whole point. `iot.factoryReset()` erases NVS. Runtime setters for these
values do not exist (the seeding internals are `friend`-accessed by `Iot`).
The seeding path is the one place still using `Preferences` directly rather than
`IotStore`, because it runs before `api.begin()` opens the store.

### Firmware version injection

`scripts/git_version.py` (wired via `library.json` → `build.extraScript`) runs at
pre-build and injects `IOT_FW_VERSION` from **the consuming project's** git:
`git describe --tags --dirty --always`. Critically it runs `git -C $PROJECT_DIR`,
**not** the process CWD — PlatformIO's CWD points at the library's own libdeps
checkout, so a bare `git` would report *this* library's version instead of the
firmware's. Falls back gracefully (define unset) without git; skip with
`-DIOT_NO_GIT_VERSION`. Reported as the `firmware_version` telemetry field;
`firmware_id` is the separate ESP app-descriptor composite.

## Server contract (must stay in sync with nice4iot)

- Device endpoints normalize **all** auth failures to **HTTP 401** (→ auto
  re-provision + one retry). Only `/provision` returns **403** (unfixable config:
  project/device inactive, not approved, HTTP disabled) — 403 keeps the token and
  does not retry. See `api.updateProvisioning()`, which returns an `IotResult`.
- Telemetry/log bodies capped at 8 KiB (413 beyond); uploads 10 MiB.
- Names (device/project) must match `^[a-zA-Z_][a-zA-Z0-9_]*$` — device IDs use
  `e32_<mac>` (underscore, not hyphen).
- One keep-alive TCP/TLS connection is reused per wakeup cycle.

## Conventions

- Units and types: the unit is always a suffix (`_ms`, `_s`, `_mV`); durations
  in ms are `uint32_t`, in s `int32_t`, points in time `int64_t` unix seconds
  (never `time_t`, `int`, `long` or `unsigned long` in a public signature).
  Defaults are named constants, not literals repeated in signatures.
- Breaking changes and migration notes belong in the README's
  "Migration to version X" section and in the commit message — **not** in code
  comments. Comments describe what the code does now, not what an earlier
  version did.
- Versioning: bump `IOT_VERSION_{MAJOR,MINOR,PATCH}` in `include/iot.h` **and**
  `version` in `library.json` together. Releases are git tags `vX.Y.Z` with a
  matching GitHub release; CI must be green first. Verify you are on `main` before
  committing a release (a detached-HEAD commit has been an issue before).
- User (clausgf) communicates in German; prefer German in conversation.
- Only this repo is edited — **never modify the nice4iot repo** (read-only reference).
