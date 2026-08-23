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
pio test -e native -f test_result  # run one test suite (test_result | test_telemetry | test_text)
```

- **ESP32 builds are never done from this repo directly.** There is no ESP32 env in
  `platformio.ini` (its only env is `native`, for tests). The library is compiled on
  target only via `pio ci` — CI does this for `esp32dev` and `esp32-s3-devkitc-1`
  using the **pioarduino** platform (the PlatformIO registry `espressif32` ships only
  arduino-esp32 2.x; this library needs 3.x / ESP-IDF 5 and C++20). To reproduce a
  target build locally, mirror the `pio ci` invocation in `.github/workflows/ci.yml`.
- Native tests require C++20 (`-std=gnu++20` in `platformio.ini`); on target the
  arduino-esp32 framework default (`gnu++2b`) suffices.

## Architecture

### Five global singletons (declared `extern` in their headers)

| Object          | Header/impl        | Responsibility |
|-----------------|--------------------|----------------|
| `iot`           | `iot.{h,cpp}`      | Lifecycle façade: WiFi + NTP, system telemetry, deep sleep, watchdog, battery, panic/backoff. |
| `api`           | `iot_api.{h,cpp}`  | HTTP(S) transport: provisioning, requests, TLS trust, OTA, file upload/forward. |
| `config`        | `iot_config.{h,cpp}` | Server config mirrored into NVRAM (ETag-cached). |
| `logger`        | `iot_logger.{h,cpp}` | Buffered remote logging + serial logging. |
| `apProvisioning`| `iot_ap.{h,cpp}`   | SoftAP + captive-portal first-time provisioning (see below). |

`iot.begin()` initializes the other three (`_initSubsystems()`). `docs/concepts.md`
is the authoritative design document — **read it before non-trivial changes**; it
covers the wakeup cycle, provisioning/tokens, telemetry shape, seeding, persistence
tiers, TLS trust, and the `IotResult` contract.

### Two value types (Arduino-free, unit-tested on host — see `test/`)

- `IotResult` (`iot_result.h`): the return type of high-level operations. Separates
  **Ok (2xx) / HttpError / TransportError**, is implicitly constructible from an
  `int` (so low-level `api.*` calls returning raw status can be viewed as one), and
  carries synthetic statuses ≥ 600 (`STATUS_NO_PROVISIONING_TOKEN`,
  `STATUS_MALFORMED_RESPONSE`, `STATUS_UPDATE_FAILED`). **Design rule:** high-level
  ops return `IotResult`; low-level `apiGet/apiPost/apiPut/apiHead/apiRequest` stay
  raw `int`. For cache-aware calls (`updateConfig`, `updateFirmware`,
  `apiCheckForUpdate`) **HTTP 304 is success** — test with `isNotModified()` /
  `isOkOrNotModified()`, not plain `operator bool` (which is strict-2xx).
- `IotTelemetry` (`iot_telemetry.h`): builder guaranteeing the server's required
  flat-JSON-of-numbers shape.

### Configuration seeding model (the key non-obvious concept)

Bootstrap values a device needs *before* it can reach the server (WiFi creds, API
URL, project, provisioning token, TLS trust) live in **NVS**, not `config.json`.
They are provided at build time via `-D` defines and written by
`iot.seedCredentials({...})` (`iot_seed.h`, `IotSeedConfig` designated initializers),
**each value only if its NVS key is absent** or when `seedGeneration` is bumped.
Consequence: a **secretless build passes empty defaults (a no-op)**, so firmware
updates built without secrets (e.g. public CI) preserve the device's existing NVS —
this is the whole point. `iot.factoryReset()` erases NVS. Runtime setters for these
values were removed in 3.x (they are `friend`-accessed by `Iot`).

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
  does not retry. See `api.updateProvisioning()` (returns `IotResult`) vs.
  `updateProvisioningOk()` (`bool` wrapper).
- Telemetry/log bodies capped at 8 KiB (413 beyond); uploads 10 MiB.
- Names (device/project) must match `^[a-zA-Z_][a-zA-Z0-9_]*$` — device IDs use
  `e32_<mac>` (underscore, not hyphen).
- One keep-alive TCP/TLS connection is reused per wakeup cycle.

## Conventions

- Versioning: bump `IOT_VERSION_{MAJOR,MINOR,PATCH}` in `include/iot.h` **and**
  `version` in `library.json` together. Releases are git tags `vX.Y.Z` with a
  matching GitHub release; CI must be green first. Verify you are on `main` before
  committing a release (a detached-HEAD commit has been an issue before).
- User (clausgf) communicates in German; prefer German in conversation.
- Only this repo is edited — **never modify the nice4iot repo** (read-only reference).
