# arduino4iot — Core concepts

This document explains the ideas the library is built on. For a task-oriented
quick start see the [README](../README.md); for API details read the doxygen
comments in the header files.

## The wakeup cycle

arduino4iot is designed around **battery-powered, deep-sleep devices**. The
mental model is not a long-running `loop()` but a short *wakeup cycle*:

1. wake from deep sleep (or power-on),
2. connect WiFi and sync time (NTP),
3. provision / refresh the API token,
4. fetch configuration and firmware updates,
5. take measurements and post telemetry,
6. flush logs,
7. go back to deep sleep.

A full cycle can complete in well under a second, so the radio and CPU are
active only briefly. Everything in the library is optimized for this: a single
reused TLS connection per cycle, logs buffered and sent in one request, tokens
and cache validators persisted across sleep. Code typically lives in `setup()`;
`loop()` is never reached because `setup()` ends in `iot.deepSleep()`.

Always-on (non-sleeping) use is possible too, but the API and its trade-offs are
tuned for the cycle model.

## The five singletons

The library exposes five global objects. You configure them, then drive them
through the cycle:

| Object           | Type        | Responsibility |
|------------------|-------------|----------------|
| `iot`            | `Iot`       | Lifecycle façade: WiFi + NTP, system telemetry, deep sleep, watchdog, battery supervision, panic/backoff. |
| `api`            | `IotApi`    | HTTP(S) transport to the nice4iot server: provisioning, requests, TLS trust, firmware update, file upload/forward. |
| `config`         | `IotConfig` | Configuration values downloaded from the server and cached in NVRAM. |
| `logger`         | `IotLogger` | Buffered remote logging plus local serial logging. |
| `apProvisioning` | `IotAp`     | SoftAP + captive-portal provisioning when no bootstrap credentials are seeded — see "First-time provisioning" below. |

`iot.begin()` initializes `api`/`config`/`logger` (and, before that, hands
off to `apProvisioning.run()` if no WiFi SSID is seeded), so most programs
only call `iot.begin(ssid, password)` after setting the `api` parameters.

Two value types round out the surface: `IotTelemetry` (a telemetry builder) and
`IotResult` (a typed operation result). Neither is a singleton — you create them
where needed. Both are intentionally Arduino-free so they can be unit-tested on
the host (see `test/`).

## Provisioning and tokens

The device authenticates to the server with a short-lived **bearer token**. It
is obtained by exchanging a long-lived **provisioning token** at `/api/provision`.

- The provisioning token is seeded from a build-time default into NVRAM (see
  *Configuration & secrets* below).
- `api.updateProvisioning()` returns the device token if the current one is still
  valid, or requests a new one when it is missing or about to expire. The expiry
  is read from the server's `expiresIn` field; the renewal margin is configurable
  (`api.setDeviceTokenExpiryMargin_s()`, default 1 h). Choose a margin larger than
  your sleep interval so a device never wakes up with an already-expired token.
- If a request is rejected with **HTTP 401**, the library clears the token,
  re-provisions once and retries the request automatically.

See [the auth section in the README](../README.md#server-api-notes) for the
401/403 semantics of the current nice4iot server.

## Telemetry

Telemetry is a **flat JSON object of numeric values** (the server's time-series
backend expects this). `IotTelemetry` guarantees that shape:

```cpp
IotTelemetry t;
t.add("temperature", 22.5).add("humidity", 40);
iot.postTelemetry("sensors", t);
```

`iot.postSystemTelemetry()` posts a set of device-health values (uptime/sleep
bookkeeping, battery voltage if configured, WiFi/connect metrics, firmware
identification, …). Bodies are capped at 8 KiB by the server.

The firmware is reported as two fields: `firmware_id` (the composite ESP
application descriptor — in a PlatformIO/Arduino build this identifies the
framework, not the sketch) and `firmware_version`, which the library derives from
the **consuming project's git** at build time via a pre-build script
(`scripts/git_version.py`, wired through `library.json`), so a firmware reports a
meaningful version with no boilerplate. The version is a single
`git describe --tags --dirty --always` string — `0.10.0` exactly on a tag, else
`0.10.0-3-gcce20b9` (with `-dirty` when the tree is modified), or the bare commit
if there is no tag. An app can override it with `iot.setFirmwareVersion()`, or
skip git with `-DIOT_NO_GIT_VERSION`.

A third, optional field, `board_id`, identifies the hardware variant (as
opposed to the firmware): set `-DIOT_BOARD_ID="waveshare_esp32_driver"` per
PlatformIO env and `iot.getBoardId()` returns it, omitted from telemetry if
undefined. Unlike the seeded bootstrap values, this is a plain compile-time
constant — not seeded, not persisted to NVS.

## Configuration & secrets (seeding)

There are two distinct kinds of configuration, and it matters which is which:

- **Bootstrap values** a device needs *before* it can reach the server — WiFi
  credentials, API endpoint, project name, provisioning token and TLS trust.
  These cannot come from `config.json` (you need them to fetch it in the first
  place), so they are **seeded** from build-time `-D` defaults into NVS by
  `iot.seedCredentials({...})`, called once before `begin()`. Each value is
  written only if its NVS key is absent, or when `IotSeedConfig::seedGeneration`
  exceeds the value stored in NVS (a deliberate re-seed via a new firmware).
- **Runtime config** — everything in `config.json`, handled by `config` below.

Because a secretless build passes empty defaults (a no-op), the seeded values in
NVS survive a firmware update built without secrets — this is what lets a public
CI produce update images. `iot.begin()` loads WiFi and the API/TLS configuration
from NVS; `iot.factoryReset()` erases all persisted state. For https, the TLS
trust (CA-pin PEM, the public bundle, insecure, optional client cert/key) is
seeded via the same struct and applied by `begin()`; the seeded PEMs are held in
`api` members for the object lifetime because the TLS stack keeps the pointers
rather than copying. None of the bootstrap values are changeable via `config.json`.
See [examples/README.md](../examples/README.md) for the build wiring.

`seedCredentials()` only ever writes NVS; it never touches the in-RAM state
`begin()` already loaded. `IotApi::begin()` reads the bootstrap values from
NVS into RAM exactly once (`_baseUrl`, `_projectName`, `_provisioningToken`,
TLS material, …) and has no reload path. So on an already-running device -
e.g. a runtime re-provisioning flow (AP + form) that calls `seedCredentials()`
again with new values - the new values sit in NVS but the device keeps acting
on the old ones in RAM until it reboots. Any such re-seeding flow must restart
the device (not just call `begin()` again in the same run) before the new
identity/endpoint takes effect.

### NVS schema (stable interface)

The bootstrap values above live in NVS namespace **`iot`** under fixed key
names. This schema is a public contract, not an implementation detail: a
device doesn't have to be provisioned by a firmware rebuild — the same keys
can be written directly into an NVS partition image with
[`nvs_partition_gen.py`](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/storage/nvs_partition_gen.html)
(initial flash, or a browser-based flashing tool). A ready-to-fill template is
at [`examples/nvs_seed_template.csv`](../examples/nvs_seed_template.csv).

| NVS key     | Type   | Meaning                                             | `IotSeedConfig` field |
|-------------|--------|------------------------------------------------------|------------------------|
| `wifiSsid`  | string | WiFi SSID                                             | `wifiSsid`             |
| `wifiPass`  | string | WiFi password                                         | `wifiPassword`         |
| `apiUrl`    | string | API base URL                                          | `apiUrl`               |
| `project`   | string | Project name (`^[a-zA-Z_][a-zA-Z0-9_]*$`)             | `projectName`          |
| `provToken` | string | Provisioning token (secret)                           | `provisioningToken`    |
| `tlsMode`   | u8     | `IotTlsMode`: 0=None, 1=Bundle, 2=Insecure, 3=CaPin   | `tlsMode`               |
| `caCert`    | string | CA certificate PEM (for `tlsMode`=CaPin)              | `caCertPem`             |
| `cliCert`   | string | Client certificate PEM (optional mTLS)                | `clientCertPem`         |
| `cliKey`    | string | Client private key PEM (optional mTLS, secret)        | `clientKeyPem`          |

A key is only ever written by `seedCredentials()` if the value is non-empty
**and** the key is absent from NVS or `seedGeneration` was bumped (see above),
so an externally written image is indistinguishable from one written by
`seedCredentials()` and survives subsequent secretless firmware. Leave a key
out of the image entirely rather than writing it empty — an empty *present*
key still counts as "already seeded" and blocks a later non-forced seed from
ever filling it in.

Two more keys share the `iot` namespace but are **not** part of the seed
schema — they are runtime-managed by the library and must not be
pre-populated by external tooling: `deviceToken`/`deviceTokExp` (issued by
`/provision`, see [Provisioning and tokens](#provisioning-and-tokens)) and
`firmwareEtag`/`firmwareDate` (OTA cache).

**Compatibility:** key names and value types/encodings in this table are
SemVer-governed. Renaming a key, changing its NVS type/encoding, or
repurposing it for a different meaning is a **breaking change** (major
version bump); adding a new key is not.

## First-time provisioning: SoftAP + captive portal

A device with no seeded WiFi SSID (a factory-fresh device, or one after
`iot.clearProvisioning()`/`iot.factoryReset()`) has no way to reach the
server at all. `Iot::begin()` detects this — an empty `wifiSsid` in NVS is
the single trigger — and hands off to `apProvisioning.run()` (`IotAp`,
`iot_ap.{h,cpp}`) instead of attempting to connect. That call blocks and
never returns: a successful setup ends in `iot.restart()`, an idle timeout
ends in `iot.shutdown()`.

The device opens its own **open** (no password) WiFi network,
`<ssidPrefix><last 4 hex chars of the MAC>` (`setSsidPrefix()`, default
`"arduino4iot-setup-"`), and serves a small web form for the same core
bootstrap values `seedCredentials()` would otherwise take from build-time
defines: WiFi SSID/password, apiUrl, project, provisioningToken. TLS trust is
deliberately **not** part of the form — an `https://` device must already
carry a build-seeded `tlsMode`/`caCert`, left untouched by re-seeding just
these five fields (`iotSeedString()` only writes non-empty values). A
successful submit force-overwrites via a bumped `seedGeneration`, then
applies the already-documented rule above: **a runtime re-seed only updates
NVS and needs a reboot**, hence the `iot.restart()`.

**No captive-portal popup by design.** The onboard `WebServer` answers known
OS connectivity-probe URLs (Android `generate_204`/`gen_204`, Apple
`hotspot-detect.html`/`success.html`, Windows `connecttest.txt`/`ncsi.txt`,
Firefox `success.txt`) truthfully, so the OS concludes there is real internet
and does not launch its own restricted in-OS mini-browser. The SSID text is
the only available hint; the user opens a normal browser and navigates to
`http://192.168.4.1/` themselves. (Windows' separate DNS-only
`dns.msftncsi.com` check is not specially handled — a documented, accepted
limitation, since the wildcard captive-portal DNS server answers every name
with the AP's own IP.)

**QR-code deep link.** Rather than a vendored JS QR-decoder in the served
page, the QR payload is simply the setup URL itself with the known fields as
query parameters — a phone's native camera app already opens a scanned URL
in the browser:

```
http://192.168.4.1/?wifiSsid=<ssid>&wifiPassword=<pw>&apiUrl=<url>&project=<name>&provisioningToken=<token>
```

All five parameters are independent/optional — a generator can supply just
`apiUrl`/`project`/`provisioningToken` (the installer types WiFi manually) or
all five (a combined per-site code). The phone must already be joined to the
setup AP for the link to resolve (same precondition as the no-popup design
above). `GET /` pre-fills the form from any given query args, but **nothing
is written to NVS until the user taps "Save"** (`POST /save`) — a
merely-opened link never silently reprovisions the device. Caveat:
`provisioningToken`/`wifiPassword` end up in the phone browser's history as
plain query-string text — acceptable for a short-lived, locally-scoped setup
flow, but worth being aware of.

**No backoff state, on purpose.** RTC RAM already doesn't survive a
power-on-reset (only deep sleep — see "Persistence" below), so "start fresh
after every physical reset" is already the natural platform behavior with
zero bookkeeping. On an unfulfilled session (idle timeout, default 5 min via
`setIdleTimeout_s()`, nobody connected/submitted), the device calls
`iot.shutdown()` — indefinite deep sleep, no wake timer — so it never
re-attempts AP mode on its own; only a physical reset restarts the check,
immediately, on the very next boot. This is deliberately *not* the escalating
panic/backoff pattern (see "Failure handling" below): since the triggering
condition (no seed data) cannot resolve itself between wakeups, any
timer-based retry would be provably useless — exactly the "battery drains in
an open AP nobody is waiting for" scenario this avoids.

**Flash footprint.** `DNSServer`/`WebServer` are arduino-esp32 core headers
(no new `library.json` dependency), but they are not free: on the `esp32dev`
board's default (dual-OTA) partition scheme, the library + a minimal sketch
already used ~95% of the 1.25 MB app partition before this module: adding it
costs roughly another 42 KB (~3 percentage points), leaving very little
headroom for application code. Projects planning to enable AP provisioning on
a flash-constrained board should budget for this — e.g. a larger/custom
partition table — rather than relying on the stock `esp32dev` scheme.

## Configuration (runtime, config.json)

`config` mirrors a server-side JSON file into NVRAM. `config.updateConfig()`
downloads it using `ETag`/`If-None-Match` (and `Last-Modified`), so an unchanged
file is not transferred again. Values are read back with
`config.getConfigInt32/Bool/String(...)`.

`IotConfigValue<T>` binds a C++ variable to a config key: it reads from the
config/NVRAM on construction and can be assigned back, which keeps the library's
own tunables (log level, sleep duration, panic parameters, …) configurable from
the server without extra plumbing.

## Logging

`logger` writes to the serial console and, unless disabled, **buffers** log
messages in RAM and sends them to the server in a single request. This keeps the
active window short (one request instead of one per line) and avoids a
re-entrancy hazard (logging from inside an API request).

- `logger.setBuffered(false)` sends each line immediately.
- `logger.flush()` forces the buffer out; it is called automatically at all
  cycle exit points (`deepSleep`/`restart`/`shutdown`/`end`) and from `panic()`.
- If the buffer would exceed the server's size limit it is flushed early; if
  there is no connectivity the buffer is kept for the next successful flush.

## Fast WiFi reconnect

The WiFi connect dominates the radio-on time of a wakeup — the all-channel scan
(~0.3–1.5 s) and DHCP (~0.3–1 s) repeat every wake even though the device almost
always reassociates with the same AP on the same channel with the same IP. Two
opt-in optimizations cut this, both with automatic fallback:

- **Fast reconnect (on by default):** the last-good BSSID + channel are cached in
  RTC RAM; the next connect targets that AP directly (`WiFi.begin(ssid, pw,
  channel, bssid)`), skipping the scan. On a timeout or a roamed/moved AP the
  cache is discarded and a plain full-scan connect is retried (`setFastReconnect()`).
- **Static IP:** `iot.setStaticIp(...)` calls `WiFi.config()` before connecting,
  skipping DHCP. Note the device then gets no DNS from DHCP — pass DNS servers if
  the API URL uses a hostname.
- **DHCP lease cache (opt-in, `setDhcpCache()`):** a static IP without the manual
  per-device configuration. The address DHCP hands out is cached in RTC RAM and
  re-applied via `WiFi.config()` on the next wake, skipping the DHCP DORA exchange
  (~0.3–0.5 s). It is reused only on the fast-reconnect path (same cached AP, so
  the same subnet) and only for a bounded number of wakeups (`maxReuse`, default
  20) before a real DHCP bind renews the lease — the DHCP lease *time* is not
  visible through the Arduino API, so this wakeup count is the renewal guard;
  keep it below `lease_time / wakeup_interval`. If the fast reconnect times out,
  the cached lease is assumed stale, discarded, and a plain DHCP connect is
  retried, so a reassigned address costs at most one slow connect. Off by default
  because it trades a small correctness risk (a lease reassigned by the server
  before the renewal bound) for speed.

The connect also sets `WiFi.persistent(false)` so the WiFi stack does not write
credentials to NVS on every connect (flash wear). The measured connect duration
is available via `iot.getWifiConnectDuration_ms()` and posted as the
`wifi_connect_ms` system-telemetry metric, so the saving is verifiable per device.

The other big time sink is the NTP sync. The system clock has to be correct
*before* the first HTTPS request (TLS certificate validity is checked against it),
so the sync cannot be hidden behind other work. Instead `syncNtpTime()` sends a
single manual SNTP request and sets the clock directly, bypassing the lwIP SNTP
daemon's randomized 0–5 s startup delay; it falls back to the daemon if the
one-shot query fails. Within the resync interval (`ntp_resync_s`) the sync is
skipped entirely, relying on the RTC clock that survives deep sleep. Pointing
`ntp_server1` at an IP or the local router additionally removes DNS from the path.

## Persistence: NVRAM vs. RTC RAM

Two storage tiers survive a deep-sleep cycle:

- **NVRAM (flash / Preferences):** survives power loss. Holds the provisioning
  and device tokens, config values and cache validators (ETag/Last-Modified). The
  NVS handle is opened once and kept open to avoid flash churn.
- **RTC RAM:** survives deep sleep but not power loss; cheap and fast. Used for
  short-lived bookkeeping across cycles (e.g. sleep accounting).

## Firmware updates (OTA)

`api.updateFirmware()` does a conditional check (ETag/Last-Modified) and streams
a new image only when one is available. The same TLS trust configuration used
for API calls applies to the OTA download.

The default apiPath is `file/{project}/{device}/firmware-{board}.bin`, where
`{board}` substitutes to the `IOT_BOARD_ID` build default (see
[`board_id`](#telemetry) / `Iot::getBoardId()`) — this lets a project serve
distinct firmware images per hardware variant from the same project/device
namespace. If `IOT_BOARD_ID` is undefined, `{board}` substitutes to an empty
string (`firmware-.bin`); pass an explicit `apiPath` without `{board}` in that
case. `{board}` is available in **any** apiPath template, not just for
firmware (see `IotApi::getApiUrlForPath()`).

## TLS server trust

For an `https://` API URL you must seed exactly how the server certificate is
verified, via the `tlsMode` field of `seedCredentials()`: `IotTlsMode::CaPin`
with `caCertPem` (pin your CA — the usual choice for a self-hosted server),
`IotTlsMode::Bundle` (verify against the public Mozilla root bundle), or
`IotTlsMode::Insecure` (development only — no verification). If none is seeded,
the handshake fails; the library detects this and logs an explicit, one-time
diagnostic instead of leaving you with an opaque transport error.

## IotResult: three outcomes, not one int

The low-level `api.*` request methods return a raw `int`: negative values are ESP
transport errors (e.g. connection refused, timeout), non-negative values are HTTP
status codes. That single int conflates three genuinely different outcomes, which
is why raw status codes are easy to ignore.

`IotResult` disentangles them into **`Ok` (2xx) / `HttpError` / `TransportError`**,
keeps the underlying `.httpStatus` / `.transportError` available, and provides an
explicit `operator bool` so `if (!iot.postTelemetry(...))` reads correctly. The
high-level operations whose notion of success is simply "2xx" return `IotResult`:
`postTelemetry()`, `postSystemTelemetry()`, `apiForward()`, `updateProvisioning()`
and `uploadFile()`.

Because `IotResult` is implicitly constructible from an `int`, any low-level call
can be viewed as a result without a new API — `IotResult r = api.apiGet(...)`.
Synthetic statuses (all ≥ 600 so they never collide with real codes) let the
library report client-side conditions through the same type:
`STATUS_NO_PROVISIONING_TOKEN`, `STATUS_MALFORMED_RESPONSE`, `STATUS_UPDATE_FAILED`.

Cache-aware calls (`updateConfig()`, `updateFirmware()`, `apiCheckForUpdate()`)
return `IotResult` too, but for them **HTTP 304 Not Modified is a normal
outcome**, not a failure: "the resource is unchanged, nothing to do". Test these
with `isNotModified()` (unchanged) and `isOkOrNotModified()` ("no error", i.e.
changed *or* unchanged) rather than the plain `operator bool`, which — following
the strict 2xx rule — reports 304 as not-ok:

```cpp
IotResult r = api.updateFirmware();
if (!r.isOkOrNotModified()) { /* real failure: unreachable, rejected, ... */ }
else if (r.isNotModified()) { /* firmware already up to date */ }
else                        { /* new firmware flashed */ }
```

This keeps a single result type across the whole library instead of a separate
type per endpoint; the one HTTP nuance the cache-aware endpoints need (304) lives
in `IotResult` as a pair of predicates.

## Failure handling: panic and backoff

`iot.panic(...)` is the library's escalating failure strategy for a device with
nobody to look at it. It logs, flushes, and goes back to deep sleep for an
increasing duration (configurable base, factor and cap), so a device that cannot
reach its server backs off instead of hammering it or draining its battery.
`iot.startWatchdog()`/`resetWatchdog()` guard against a hung wakeup cycle.

`apProvisioning`'s response to an unfulfilled setup session ("First-time
provisioning" above) is a deliberately separate, simpler policy — indefinite
sleep rather than this escalating pattern — since unlike a reachability
failure, its triggering condition cannot resolve itself between wakeups.
