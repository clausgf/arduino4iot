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
| `iot`            | `Iot`       | Lifecycle façade: drives the ports, deep sleep, radio off, watchdog, undervoltage supervision, panic/backoff. Carries the endpoint groups `iot.telemetry`, `iot.files`, `iot.forward`, `iot.ota`. |
| `api`            | `IotApi`    | HTTP(S) transport to the nice4iot server: provisioning, `get/post/put/head/send`, conditional `getCached`, TLS trust. |
| `config`         | `IotConfig` | Configuration values downloaded from the server and cached in NVRAM. |
| `logger`         | `IotLogger` | Buffered remote logging plus local serial logging. |
| `apProvisioning` | `IotAp`     | **Opt-in.** SoftAP + captive-portal provisioning when no bootstrap credentials are seeded — see "First-time provisioning" below. |

The endpoint groups are stateless members of `iot` named after the nice4iot
endpoints (`telemetry/`, `file/`, `forward/`, the firmware file). They make the
calls discoverable from one object; a method no application calls is not
linked, so a group costs nothing until it is used. A plain namespace `iot::`
was not an option — it would collide with the global object `iot`.

`iot.begin()` initializes `api`/`config`/`logger`, so most programs only call
`iot.begin()` after seeding the bootstrap configuration. `apProvisioning` is
the odd one out: the core never references it, so it exists only in firmware
that opts in via `iot.setNoCredentialsHandler()` — the module costs ~42 KB and
most devices never open a setup AP.

## What `iot` is made of

`Iot` drives three ports and owns none of the mechanisms behind them:

| Port | Default | Replace with |
|---|---|---|
| `IotNetwork` | `IotWifiNetwork`, configured by `begin()` with the seeded credentials | `iot.setNetwork()` — Ethernet, LTE, a test double |
| `IotClock` | `IotSystemClock`: reads the system time, **never synchronizes** | `iot.setClock(ntpClock)` — see *Clock* below |
| `IotBattery` | none (`nullptr`) | `iot.setBattery()` — `IotAdcBattery`, `IotCallbackBattery`, or your own |

Plus `IotPower` (`iot.setPower()`) for how the cycle ends. Optional adapters are
only linked when an application names them: a firmware without `IotNtpClock`
contains no SNTP code, one without `IotAdcBattery` no ADC code.

## Clock

There are three implementations of `IotClock`:

- **`IotSystemClock`** (the default) reads the system time; `sync()` only
  reports whether it is plausible. Enough for an `http://` endpoint, or a device
  whose time comes from an external RTC chip or GPS. Links no SNTP code.
- **`IotNtpClock`** sends a single manual SNTP request and sets the clock
  directly, bypassing the lwIP SNTP daemon whose randomized 0–5 s startup delay
  would otherwise dominate the wakeup. It syncs only when the clock is
  implausible or the resync interval elapsed, so calling it every wakeup is
  cheap. Servers and interval are set on the object (`setServers()`,
  `setResyncInterval_s()`).
- **`IotFakeClock`** for host tests.

**`https://` does not need a synchronized clock.** arduino-esp32 builds mbedTLS
without `MBEDTLS_HAVE_TIME_DATE`, so the handshake verifies the certificate
chain but not its validity dates. Only a custom sdkconfig that enables the date
check makes TLS depend on the clock. What does need it: renewing the device
token *before* it expires (without it, the token is replaced on the first 401),
meaningful timestamps, and any time logic of the application. `begin()` logs a
warning when the clock is implausible, but its return value reflects the
*network* only. A TLS transport error almost always means missing server trust,
for which the library logs a dedicated hint.

```cpp
static IotNtpClock ntpClock(iotRtcState().ntpLastSyncTime);
iot.setClock(ntpClock);
```

## Battery

The library needs one number — the supply voltage in millivolts — and does not
care how it is obtained. `IotAdcBattery` covers a divider on an ADC pin
(averaging several samples, optional enable pin so the divider can be switched
off between measurements); `IotCallbackBattery` wraps any lambda, e.g. an I2C
fuel gauge; anything else implements `IotBattery`.

Without a battery there is no `battery_V` telemetry field and no undervoltage
check. With one, `battery_min_mv` (server-configurable) makes `begin()` shut the
device down below the threshold.

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

### Where the pieces live

Authentication is deliberately **not** part of the transport:

- `IotTokenManager` (`iot_token.h`) holds the provisioning token, the device
  token and its expiry, persists the latter through `IotStore`, and owns the one
  decision that matters — `needsNewToken(now, isTimePlausible)`. It knows
  nothing about HTTP, is Arduino-free and unit tested on the host
  (`test/test_token`), including the two cases that deliberately count as "still
  good": an **unknown expiry** (a server that sent no `expiresIn`) and an
  **implausible clock** before the NTP sync, where the arithmetic would be
  meaningless. Both surface as a 401 and re-provision, which is cheaper than
  re-provisioning on every wakeup.
- `IotApi::_perform()` builds one `IotHttpRequest` and hands it to the
  `IotHttp` port. It has no notion of tokens, and no notion of sockets either:
  `IotEspHttp` owns the `HTTPClient`, the keep-alive connection and the TLS
  trust, which is what lets the whole transport be exercised on the host.
- `IotApi::send()` is the policy on top: attach the token, and on 401 get
  a new one and retry once. Because the retry cannot re-enter the request path,
  nothing has to guard against it.

**Choose the renewal margin against the server's token lifetime.** The margin is
a promise that the token outlives the next sleep, so it must be larger than the
sleep interval — but if it exceeds the lifetime the server issues, every wakeup
re-provisions. There is a test pinning exactly that interaction.

## Telemetry

Telemetry is a **flat JSON object of numeric values** (the server's time-series
backend expects this). `IotTelemetry` guarantees that shape:

```cpp
IotTelemetry t;
t.add("temperature", 22.5).add("humidity", 40);
iot.telemetry.post("sensors", t);
```

`iot.telemetry.postSystem()` posts a set of device-health values (uptime/sleep
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

All keys and both namespaces are defined in one place, `iot_nvs_keys` in
`iot_names.h`, next to the server paths (`iot_paths`) — the two sets of names
the library shares with the outside world.

**Compatibility:** key names and value types/encodings in this table are
SemVer-governed. Renaming a key, changing its NVS type/encoding, or
repurposing it for a different meaning is a **breaking change** (major
version bump); adding a new key is not.

## First-time provisioning: SoftAP + captive portal

A device with no seeded WiFi SSID (a factory-fresh device, or one after
`iot.clearProvisioning()`/`iot.factoryReset()`) has no way to reach the
server at all. `Iot::begin()` detects this — an empty `wifiSsid` in NVS is
the single trigger — and calls the handler registered with
`iot.setNoCredentialsHandler()` instead of attempting to connect:

```cpp
#include <iot_ap.h>
iot.setNoCredentialsHandler([] { apProvisioning.run(); });
```

`apProvisioning.run()` (`IotAp`, `iot_ap.{h,cpp}`) blocks and never returns:
a successful setup ends in `iot.restart()`, an idle timeout ends in
`iot.shutdown()`. If the handler returns anyway, or if none is registered,
`begin()` logs an explicit error and shuts the device down — indefinite deep
sleep, for the same reason the idle timeout does (see *No backoff state*
below). See [`examples/ap_provisioning/`](../examples/ap_provisioning/) for a
complete sketch.

**Why a hook and not a direct call.** The module is not free (see *Flash
footprint* below) and many devices never need it — they are provisioned by a
seeded NVS partition image. Since the core names no provisioning module, the
linker drops `IotAp`, `WebServer` and `DNSServer` entirely from a firmware that
does not reference them, and the dependency stays one-way, `iot_ap` → `iot`.

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
(no new `library.json` dependency), but they are not free. Measured on
`esp32dev` with its default (dual-OTA) 1.25 MB app partition, using the same
sketch with and without the handler registered:

| Build                                    | Flash       | of partition |
|------------------------------------------|-------------|--------------|
| `examples/main.cpp` (no handler)         | 1 251 382 B | 95.5 %       |
| the same, with `apProvisioning.run()`    | 1 293 406 B | 98.7 %       |

So the module costs about **42 KB**, or 3.2 percentage points of an already
tight partition — which is why it is opt-in rather than wired in. A firmware
that does not register the handler links none of it (verified with `nm`: zero
`IotAp`/`WebServer`/`DNSServer` symbols in the first build, 19/47/9 in the
second). A project that does enable it on a flash-constrained board should
budget for the 42 KB — e.g. a larger/custom partition table — rather than
relying on the stock `esp32dev` scheme.

## Configuration (runtime, config.json)

`config` mirrors a server-side JSON file into an `IotStore` (NVS namespace
`iot-cfg`). `config.updateConfig()` downloads it using `ETag`/`If-None-Match`
(and `Last-Modified`), so an unchanged file is not transferred again. Values are
read back with `config.getInt/getBool/getString(...)` — reads only: a value is
changed by the server or, for the cycle, by `set()` on its `IotConfigValue`.
`config.isMirrored()` tells whether a config.json was ever applied, e.g. to
fetch it synchronously on a device's first cycle and in the background later.

`IotConfigValue<T>` binds a C++ variable to a config key, which keeps the
library's own tunables (log level, sleep duration, panic parameters, …)
configurable from the server without extra plumbing. It is instantiated for
`int32_t`, `bool` and `std::string`.

### One key per value

The same name identifies a value in `config.json`, in NVS and in the free-form
accessors, so there is a single naming scheme rather than one name per storage
layer. The consequence is that a key must fit `IotStore::MAX_KEY_LENGTH` (15,
the NVS limit); `config.begin()` logs an error for any key that does not.

### Precedence

From weakest to strongest:

```
compile-time default  <  NVS mirror  <  config.json  <  set() at runtime
```

A value the application sets explicitly — `iot.setSleepDuration_s(60)`, or an
assignment to an `IotConfigValue` — keeps that value for the rest of the cycle:
neither `begin()` nor `updateConfig()` overwrite it. Configuring something in
`setup()` therefore sticks, instead of being silently replaced from the mirror.

Server values are **still mirrored into NVS** even for an overridden value, so
they take effect from the next boot. The override is per cycle, not a veto.
`clearOverride()` hands a value back to the server within the same cycle.

The rule of thumb: use the setter for values the application owns, and leave a
value alone to let the server own it.

An unknown key or a type mismatch in `config.json` is counted and skipped, so
one bad entry cannot discard the rest of the file; `updateConfig()` logs the
tally.

### Metadata, limits and the schema

A value can carry an `IotConfigMeta`. It is declared once, next to the value,
and replaces three hand-maintained copies — a defaults struct, a comment block
listing the keys, and a JSON Schema written by hand:

```cpp
static IotConfigValue<int32_t> minSleep(config, 300, "min_sleep_s",
    { .title = "Min sleep (s)", .description = "Lower clamp", .min = 60, .max = 86400 });
static IotConfigValue<std::string> rotation(config, "0deg", "rotation",
    { .enumValues = "0deg|90deg|180deg|270deg" });
```

- **Enforced** on values from `config.json` and from NVS: an int outside
  `[min, max]` is clamped and logged, a string not in `enumValues` is rejected
  like a type mismatch and the previous value stays. A value the application
  `set()`s is not checked — it owns it. The library's own values carry limits
  too (`sleep_s` ≥ 1, `log_level` 0–5, `panic_factor` 1–16, …).
- **Published:** `config.schemaJson()` builds a flat JSON Schema (type,
  compile-time default, title, description, minimum/maximum, enum) — the subset
  nice4iot turns into a typed form for `config.json`. `config.publishSchema()`
  uploads it as `file/{project}/{device}/config.schema.json`, but only when it
  changed: a hash of the last published schema is kept in NVS
  (`iotCfgSchema`), so a firmware that adds or changes a value publishes once,
  and every other wakeup costs no request.
- **Presentation stays with the application:** `config.setSchemaExtras(json)`
  adds top-level members such as nice4iot's `x-ui` layout or
  `additionalProperties`. The library owns `properties`, `type` and `$schema`;
  everything else is copied as is and counts for the published hash.
  ```cpp
  config.setSchemaExtras(R"({"x-ui": {"layout": [["panel", "rotation"], ["image_path"]]}})");
  ```
- **Opt-in:** only a firmware that calls `publishSchema()` (or `schemaJson()`)
  links the schema code.
- **Approval:** nice4iot ignores a device-uploaded schema until an operator
  approves its content hash, per file. Each device uploads into its own
  directory, so a fleet means one approval per device and schema version — or
  an operator places one approved `config.schema.json` in the project
  directory, which every device without its own falls back to.

## Logging

`logger` writes to the serial console and, unless disabled, **buffers** log
messages in RAM and sends them to the server in a single request. This keeps the
active window short (one request instead of one per line) and avoids a
re-entrancy hazard (logging from inside an API request).

- `logger.setBuffered(false)` sends each line immediately.
- `logger.flush()` forces the buffer out; it is called automatically at all
  cycle exit points (`deepSleep`/`restart`/`shutdown`/`end`) and from `panic()`.
- If the buffer would exceed the server's size limit it is flushed early.

### Offline: radio off, logs in RTC RAM

A cycle often has a long stretch that needs no network — an e-paper refresh,
a measurement. `iot.radioOff()` ends the network part early: it flushes the
log, closes the API connection and disconnects the adapter, which for WiFi
also powers the radio down (`WIFI_OFF`).

What is logged afterwards cannot go out this cycle. Instead of dropping it,
`flush()` — including the automatic one before deep sleep — moves the buffer
into RTC RAM, and the first flush of a later cycle that has connectivity sends
it ahead of its own lines. The same happens when a cycle never got a
connection, or when the log request failed with a transport error or a 5xx.

- The RTC buffer holds `IOT_RTC_LOG_SIZE` bytes (default 1536, `0` disables
  it). When full, the oldest whole lines are dropped first and a note with
  their count leads the next upload — the most recent lines, usually the ones
  explaining a failure, are kept.
- It lives in an `IotRtcSlot`, so it survives deep sleep and the
  software/watchdog resets of a crash loop, and is discarded after a power-on
  reset or a firmware with a different layout.
- Each line keeps the `millis()` timestamp of the cycle it was written in.

The library logs its own failures through the same path, so the reason a
device went quiet reaches the server once it is back: a failed network connect
with the adapter's reason (`IotNetwork::lastError()`, e.g. `ssid not found
(status 1) … after 10012 ms`), a panic at startup after an abnormal reset
(written straight into RTC RAM, before any network), the backoff duration, an
undervoltage shutdown, missing credentials, and a failed sync of an attached
`IotNtpClock`. A failed provisioning or config update is reported to the caller
as an `IotResult`; put it into the `iot.panic()` message.

```cpp
iot.telemetry.post("app", telemetry);
iot.radioOff();                        // flushes the log, then radio off
display.refresh();                     // 15 s without network
logger.info("app", "refresh done");    // kept in RTC RAM, sent next cycle
iot.deepSleep();
```

## Fast WiFi reconnect

The WiFi connect dominates the radio-on time of a wakeup — the all-channel scan
(~0.3–1.5 s) and DHCP (~0.3–1 s) repeat every wake even though the device almost
always reassociates with the same AP on the same channel with the same IP. Two
opt-in optimizations cut this, both with automatic fallback:

- **Fast reconnect (on by default):** the last-good BSSID + channel are cached in
  RTC RAM; the next connect targets that AP directly (`WiFi.begin(ssid, pw,
  channel, bssid)`), skipping the scan. On a timeout or a roamed/moved AP the
  cache is discarded and a plain full-scan connect is retried (`iot.wifi().setFastReconnect()`).
- **Static IP:** `iot.wifi().setStaticIp(...)` calls `WiFi.config()` before connecting,
  skipping DHCP. Note the device then gets no DNS from DHCP — pass DNS servers if
  the API URL uses a hostname.
- **DHCP lease cache (opt-in, `iot.wifi().setDhcpCache()`):** a static IP without the manual
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
is available via `iot.network().lastConnectDuration_ms()` and posted as the
`wifi_connect_ms` system-telemetry metric, so the saving is verifiable per device.

The other big time sink is the NTP sync, for an application that attaches an
`IotNtpClock`. The clock should be correct before the first API request, so the
token expiry check can use it; the sync is therefore not hidden behind other
work. Instead `IotNtpClock::sync()` sends a
single manual SNTP request and sets the clock directly, bypassing the lwIP SNTP
daemon's randomized 0–5 s startup delay. Within the resync interval the sync is skipped entirely,
relying on the RTC clock that survives deep sleep. Pointing the first server at
an IP or the local router additionally removes DNS from the path.

## Persistence: NVRAM vs. RTC RAM

Two storage tiers survive a deep-sleep cycle, and they have genuinely different
contracts — which is why they are two different things in the code rather than
two interchangeable backends behind one interface:

|                     | NVRAM (`IotStore`)          | RTC RAM (`IotRtcState`)        |
|---------------------|-----------------------------|--------------------------------|
| Keys                | at runtime, dynamic         | static, at link time           |
| Survives power loss | yes                         | no                             |
| Survives SW reset   | yes                         | only with `RTC_NOINIT_ATTR`    |
| Write cost          | flash cycles, limited       | none                           |
| Failure mode        | open/write can fail         | none                           |
| Capacity            | ~ partition                 | 8 KB, shared                   |

- **NVRAM (flash / Preferences)** is reached through the `IotStore` port
  (`iot_ports.h`) and its `IotNvsStore` adapter. Holds the provisioning and
  device tokens, config values and cache validators (ETag/Last-Modified). The
  NVS handle is opened once and kept open to avoid flash churn, and writes are
  suppressed when the stored value is unchanged.
- **RTC RAM** is one plain struct, `IotRtcState` (`iot_rtc_state.h`), in
  `RTC_NOINIT_ATTR`. Deliberately *not* a port: RTC memory has no runtime keys,
  no failure mode and no write cost, so a key/value interface over it would buy
  a vtable and string keys and nothing else. Holds boot count, active/sleep
  accounting, the failure backoff and the WiFi fast-reconnect caches. A magic
  word detects that the contents did not survive (power-on reset, or a firmware
  whose layout changed) and resets the struct.

`RTC_NOINIT_ATTR` rather than `RTC_DATA_ATTR` matters: the bootloader
reinitializes the `.rtc.data` section on a software or watchdog reset, which
would clear the failure backoff exactly when it is needed.

Which tier a value uses is decided **once, where the value is declared** — there
is no global runtime switch flipping the storage of every value at the same
time.

### Application data

The same two tiers are open to the application, without touching the
library's own state:

- **NVS:** an own `IotNvsStore` namespace (at most 15 characters). It opens the
  handle once, suppresses writes of unchanged values and reads absent keys
  silently — the raw `Preferences` boilerplate goes away. `factoryReset()` and
  `clearProvisioning()` clear only the library's namespaces.
  ```cpp
  static IotNvsStore appStore("myapp");
  appStore.begin();
  appStore.putString("panel", panelId);
  ```
- **RTC RAM:** `IotRtcSlot<T>` (`iot_rtc_slot.h`) wraps a trivially copyable
  struct with a magic word and a layout tag, and hands out `T{}` whenever the
  contents did not survive — a power-on reset, or an OTA update that left the
  previous firmware's bytes at that address. `IotRtcState` itself is not
  extensible; its layout belongs to the library.
  ```cpp
  struct AppRtcState { char etag[96]; uint32_t deferredCycles; };
  static RTC_NOINIT_ATTR IotRtcSlot<AppRtcState> appRtc;

  if (!appRtc.isValid()) { /* lost: power-on or new layout */ }
  appRtc->deferredCycles++;
  ```
  Bump the `Version` template argument when the meaning of the struct changes
  without its size changing. Prefer `RTC_NOINIT_ATTR`: `RTC_DATA_ATTR` is
  reinitialized on every reset that is not a deep-sleep wakeup.

## Firmware updates (OTA)

`iot.ota.update()` does a conditional check (ETag/Last-Modified) and streams
a new image only when one is available. The same TLS trust configuration used
for API calls applies to the OTA download.

The default apiPath is `file/{project}/{device}/firmware.bin`. A `{board}` 
parameter is supported and substitutes to the `IOT_BOARD_ID` build default (see
[`board_id`](#telemetry) / `Iot::getBoardId()`) — this lets a project serve
distinct firmware images per hardware variant from the same project/device
namespace. If `IOT_BOARD_ID` is undefined, `{board}` substitutes to an empty
string. `{board}` is available in **any** apiPath template, not just for
firmware (see `IotApi::getApiUrlForPath()`).

## Conditional GET for application resources

`api.getCached()` is the conditional GET that `config.json` uses,
open to any resource an application fetches — an image, a schedule, a lookup
table:

```cpp
static IotNvsStore appStore("myapp");   // appStore.begin() in setup()
static IotStoredValidators imageCache(appStore, "imgEtag", "imgDate");

IotCachedResponse r = iot.forward.getCached("img", "latest.png",
    imageCache.load(), { .headers = { { "Accept", "image/png" } } });
if (r.result.isOk() && render(r.body)) { imageCache.commit(r.validators); }
else if (r.result.isNotModified()) { /* the shown copy is current */ }
```

- **The caller commits the validators.** `api.getCached()` takes the current
  `ETag`/`Last-Modified` and hands back the new ones; it stores nothing. Commit
  them only once the body was used: a resource that failed to apply is then
  fetched again next cycle instead of answered with 304 forever.
- **Where they live is the caller's choice.** `IotStoredValidators` keeps them
  in an `IotStore` (survives a power loss); RTC RAM is enough for a resource
  that is cheap to fetch once after power-on; a plain struct passed in works
  for both.
- **`maxAge_s`** is the response's `Cache-Control: max-age`, or -1 if absent or
  if `no-cache`/`no-store` forbid reusing it unvalidated. nice4iot's file
  endpoint always sends `no-cache`; a forwarded upstream may send a max-age, e.g.
  to derive the next sleep duration from it.
- The body is read into RAM as a whole. It goes through `api`, so the device
  token, the 401 re-provisioning and the keep-alive connection apply as usual.

## TLS server trust

For an `https://` API URL you must seed exactly how the server certificate is
verified, via the `tlsMode` field of `seedCredentials()`: `IotTlsMode::CaPin`
with `caCertPem` (pin your CA — the usual choice for a self-hosted server),
`IotTlsMode::Bundle` (verify against the public Mozilla root bundle), or
`IotTlsMode::Insecure` (development only — no verification). If none is seeded,
the handshake fails; the library detects this and logs an explicit, one-time
diagnostic instead of leaving you with an opaque transport error.

## IotResult: three outcomes, not one int

An `IotHttp` transport returns a raw `int`: negative values are ESP transport
errors (e.g. connection refused, timeout), non-negative values are HTTP status
codes. That single int conflates three genuinely different outcomes, which is
why raw status codes are easy to ignore; no public method returns one.

`IotResult` disentangles them into **`Ok` (2xx) / `HttpError` / `TransportError`**,
keeps the underlying `.httpStatus` / `.transportError` available, and provides an
explicit `operator bool` so `if (!iot.telemetry.post(...))` reads correctly. The
operations whose notion of success is simply "2xx" return `IotResult`:
`iot.telemetry.post()`, `iot.telemetry.postSystem()`, `iot.files.upload()`,
`api.updateProvisioning()` and `logger.flush()`.

The request methods `api.send()`, `get()`, `head()`, `post()` and `put()` return
an `IotApiResponse`: the `IotResult` plus body and the headers asked for in
`collectHeaders`, looked up case-insensitively with `header()`. Requests name
their fields with designated initializers instead of positional parameters:

```cpp
IotApiResponse r = api.get("file/{project}/{device}/data.json",
    { .headers = { { "Accept", "application/json" } }, .collectHeaders = { "ETag" } });
if (r.result) { use(r.body, r.header("ETag")); }
```

Synthetic statuses (all ≥ 600 so they never collide with real codes) let the
library report client-side conditions through the same type:
`STATUS_NO_PROVISIONING_TOKEN`, `STATUS_MALFORMED_RESPONSE`, `STATUS_UPDATE_FAILED`.

Cache-aware calls (`config.updateConfig()`, `iot.ota.update()`, `api.getCached()`)
return `IotResult` too, but for them **HTTP 304 Not Modified is a normal
outcome**, not a failure: "the resource is unchanged, nothing to do". Test these
with `isNotModified()` (unchanged) and `isOkOrNotModified()` ("no error", i.e.
changed *or* unchanged) rather than the plain `operator bool`, which — following
the strict 2xx rule — reports 304 as not-ok:

```cpp
IotResult r = iot.ota.update();
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

The strategy is split in two, along the same line as everywhere else:

- the **policy** — how long to sleep next — is `IotBackoffPolicy`
  (`iot_lifecycle.h`), a plain value type with no I/O, unit-tested on the host
  including an int32 overflow after several escalations and a misconfiguration
  producing a zero-length sleep;
- the **mechanism** — actually sleeping — is the `IotPower` port, so a host test
  can reach a failure path and keep running. `IotLifecycle` ties the two
  together and owns the backoff state in `IotRtcState`.

An **abnormal reset is acted on in `iot.begin()`, not in the constructor**, so
the diagnosis reaches a serial console the sketch has had a chance to open, and
the other singletons are constructed by then.

An application that must do something before the device sleeps supplies its own
`IotPower` (`iot.setPower()`), or registers `iot.addExitHook()` to run at every
exit — deep sleep, restart, shutdown and the backoff path alike.

`apProvisioning`'s response to an unfulfilled setup session ("First-time
provisioning" above) is a deliberately separate, simpler policy — indefinite
sleep rather than this escalating pattern — since unlike a reachability
failure, its triggering condition cannot resolve itself between wakeups.
