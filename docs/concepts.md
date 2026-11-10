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

Without a battery there is no `battery_v` telemetry field and no undervoltage
check. With one, `battery_min_mv` (server-configurable) is the threshold below
which `begin()` does not connect. The check runs early in `begin()`: before the
WiFi connect, whose current peaks are what browns a weak battery out, and
before the abnormal-start check, so a brownout caused by a weak battery leads
to the undervoltage reaction rather than to the failure backoff.

The reaction is the undervoltage handler, analogous to the panic handler.
`iot.defaultUndervoltageHandler()` shuts down until a reset when
`battery_sleep_s` is 0 (the default, for a primary cell that will not
recover), and otherwise sleeps that long and measures again (a battery that
gets recharged, e.g. by solar). `iot.setUndervoltageHandler()` replaces it,
e.g. to show a message on a display and then continue with the default:

```cpp
iot.setUndervoltageHandler([] {
    display.show("Battery low");
    iot.defaultUndervoltageHandler();
});
```

The handler runs before the connect and should not use the network; the
undervoltage log line is kept in RTC RAM and sent by a later cycle. If the
handler returns, `begin()` continues normally.

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

Telemetry is a **flat JSON object**. Numbers are measurements in nice4iot's
time series. Strings are **labels**: slowly changing device metadata such as
the firmware version, stored with their change history (`label_history`) and
shown as change markers in the Data tab. nice4iot keeps at most 16 labels per
post, 128 characters each; a value that changes with every post (a
timestamp, an id, a measurement) must not be a label. `IotTelemetry`
guarantees the flat shape:

```cpp
IotTelemetry t;
t.add("temperature", 22.5).add("humidity", 40);
iot.telemetry.post("sensors", t);
```

`iot.telemetry.postSystem()` posts a set of device-health values. Bodies are
capped at 8 KiB by the server. Field names are snake case with the unit as a
lower-case suffix (`_v`, `_ms`, `_s`), like the config keys.

| Field | Meaning |
|---|---|
| `battery_v` | supply voltage; only with an `IotBattery` |
| `wifi_rssi`, `wifi_connect_ms` | signal strength and duration of this cycle's connect |
| `boot_count` | boots since the last power-on reset |
| `reset_reason` | why this cycle started, `IotResetReason` as a number: 0 unknown, 1 power-on, 2 reset pin, 3 software, 4 panic, 5 watchdog, 6 brownout, 7 deep sleep |
| `active_ms`, `last_sleep_s` | awake time of the previous cycle, nominal duration of the sleep this cycle woke from |
| `panic_sleep_s` | running failure backoff, 0 when not in backoff |
| `panic_count` | `iot.panic()` calls since the last power-on reset |
| `crash_count` | starts after a crash, watchdog timeout or brownout since the last power-on reset |
| `undervoltage_count` | cycles that found the battery below `battery_min_mv` since the last power-on reset |
| `firmware_id`, `firmware_version`, `firmware_sha256`, `board` | labels, see below |

The counters live in RTC RAM like `boot_count`: they cost no flash writes,
survive deep sleep and software or watchdog resets, and start over at a power
loss. A rate is a counter over `boot_count`; the difference between two posts
is what happened in between. `panic_count` counts the event, not the reaction,
so a replaced panic handler does not change it. A panic points at the network,
the server or the application, a crash at the firmware or the power supply.

**Values known only after the network is gone** — a display refresh after
`iot.radioOff()`, the length of the whole cycle — go into an
`IotRtcTelemetry<N>` in the application's `IotRtcSlot`. The next cycle's post
of the same kind adds them as `last_<key>` and empties the buffer, whether or
not the post succeeds. The prefix is deliberate: nice4iot stamps every
measurement with its arrival time, so these values belong to the previous
cycle and must not pose as the current one.

```cpp
struct AppState { IotRtcTelemetry<8> next; };
static RTC_NOINIT_ATTR IotRtcSlot<AppState> rtc;

rtc->next.add("refresh_ms", refresh_ms);        // after the radio is off
iot.telemetry.post("epaper", t, rtc->next);     // next cycle: last_refresh_ms
```

`add()` overwrites a key it already holds and rejects a key of 24 characters
or more, or a value beyond `N` places; it holds numbers only, since a label
from a previous cycle would not be metadata of the current one.

The firmware is reported as `firmware_version`, which the library derives from
the **consuming project's git** at build time via a pre-build script
(`scripts/git_version.py`, wired through `library.json`), so a firmware reports a
meaningful version with no boilerplate. The version is a single
`git describe --tags --dirty --always` string — `0.10.0` exactly on a tag, else
`0.10.0-3-gcce20b9` (with `-dirty` when the tree is modified), or the bare commit
if there is no tag. An app can override it with `iot.setFirmwareVersion()`, or
skip git with `-DIOT_NO_GIT_VERSION`. nice4iot also takes it as the device's
reported firmware.

`firmware_id` identifies the whole build in one string:

```
1.4.0-2-g622e017 2026-11-10 10:51 ARDUINO 3.2.0 IDF 5.4.1 IOT 4.0.0-6-g3455250-dirty
```

— the firmware version, the project's last commit date in UTC, the Arduino
core, the ESP-IDF release, and arduino4iot: its own `git describe` when the
library is a git checkout (a local clone, `symlink://`, or `lib_deps` from a
git URL), otherwise the release from `IOT_VERSION_*`. A secure version other
than 0 is appended as `sec <n>`, unknown parts are left out. The commit date
rather than the build time, because a define that changes with every build
would force a full rebuild every time; for a `-dirty` build it is the date of
the last commit. The project name, version, date and time of the ESP app
descriptor are deliberately not used: in an Arduino build they describe the
precompiled framework libraries (`arduino-lib-builder`, built months ago), not
the firmware. At about 85 characters it stays within nice4iot's 128 per label.

A third, optional field, `board`, identifies the hardware variant (as
opposed to the firmware): set `-DIOT_BOARD_ID="waveshare_esp32_driver"` per
PlatformIO env and `iot.boardId()` returns it, omitted from telemetry if
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

**Opening the setup on request.** A device that already has credentials never
reaches the handler on its own - not even when the network rejects it. For
that, `iot.setSetupTrigger()` registers a check that `begin()` asks before
connecting, and if it says yes, `begin()` runs the same handler:

```cpp
iot.setSetupTrigger([] { return apProvisioning.waitForButton(); });
```

`begin()` asks only after a power-on or a reset-pin start, never after a
deep-sleep wakeup or a software restart, so the normal cycle of a battery
device does not get any longer. `waitForButton()` waits up to 2 s for the
BOOT button (GPIO0; pin, level and window are parameters). The button has
to be pressed **after** the reset: held during the reset, it puts the chip
into its serial bootloader, since GPIO0 is a strapping pin. Opened this way,
the portal offers "Leave setup without changes", and its idle timeout
restarts the device into its normal cycle instead of sleeping indefinitely.

**MAC address for allow lists.** Networks that admit only registered MAC
addresses need the device's address before its first connect. It is the
station MAC, which the device id is made of (`e32_<mac>`), and it is
available without a connection:

- the setup page shows device id and MAC address, and `GET /info` returns
  them as JSON (`{"deviceId":…,"macAddress":…}`);
- `begin()` logs `--- Device id …, MAC …` before connecting;
- over USB without any firmware:
  `pio pkg exec -p tool-esptoolpy -- esptool.py --port <port> read_mac`
  (the first "MAC" line is the station address);
- in code, `iot.macAddress()` (`"aa:bb:cc:dd:ee:ff"`).

The SSID of the setup AP shows only the last 4 hex digits, and the BSSID a
phone sees is the SoftAP address, which differs from the station address
(on the ESP32 by +1) - neither is what the allow list needs.

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

`Iot::begin()` binds the mirror first, before the panic backoff and
the network connect, so all of them already run with the server's values of
the last cycle; `updateConfig()` later in the cycle applies a newer file.

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
  too (`sleep_s` ≥ 1, `log_level` 0–5, `panic_factor` 1–16, `wifi_tx_qdbm` 0–84, …).
- **Published:** `config.schemaJson()` builds a flat JSON Schema (type,
  compile-time default, title, description, minimum/maximum, enum) — the subset
  nice4iot turns into a typed form for `config.json`. `config.publishSchema()`
  uploads it as `file/{project}/{device}/config.schema.json`, but only when it
  changed: a hash of the last published schema is kept in NVS
  (`iotCfgSchema`), so a firmware that adds or changes a value publishes once,
  and every other wakeup costs no request.
- **Layout:** `config.setLayout()` arranges nice4iot's form (`x-ui.layout`)
  from the values themselves, not their key strings. The application lists
  its own rows; the library appends its own, grouped (log level and sleep,
  battery, backoff), and leaves out any the application already placed. A
  value in no row gets one of its own below.
  ```cpp
  config.setLayout({ { cvPanel, cvRotation }, { cvImagePath } });
  ```
- **Anything else** goes through `config.setSchemaExtras(json)`: top-level
  members such as `additionalProperties`, copied as they are and applied last
  (an `x-ui` there replaces the generated layout). The library owns
  `properties`, `type` and `$schema`. Both count for the published hash.
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
connection, or when the log request failed in a way a later cycle may not:
a transport error, a 5xx, a 401 the re-provisioning could not resolve, a 408
or 429, or a device without a seeded API URL. Of a log split into several
requests only the part the server has not accepted is kept, so nothing is sent
twice. Before `iot.begin()` has initialized the API no request is tried at all.

- The RTC buffer holds `IOT_RTC_LOG_SIZE` bytes (default 1536, about 15–25
  lines; `0` disables it). Set it as a build flag
  (`build_flags = -DIOT_RTC_LOG_SIZE=4096`): a `#define` in the sketch does not
  reach the library's own translation unit. When full, the oldest whole lines are dropped first and a note with
  their count leads the next upload — the most recent lines, usually the ones
  explaining a failure, are kept.
- It lives in an `IotRtcSlot`, so it survives deep sleep and the
  software/watchdog resets of a crash loop, and is discarded after a power-on
  reset or a firmware with a different layout.
- Each line keeps the `millis()` timestamp of the cycle it was written in, and
  the first line a cycle keeps is led by a marker naming that cycle:
  `I (5012) iot: --- kept offline: boot 17, 2026-11-08T10:00:00Z ---` (the time
  only when the clock is plausible, otherwise `clock not set`). The server
  stamps the whole upload with its arrival time; the marker is what dates the
  lines and tells the cycles apart.
- A message is one line: the logger drops trailing line breaks and turns inner
  ones into spaces, so the buffer counts lines correctly and the server sees
  one entry per line.

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

### Transmit power

`iot.wifi().setMaxTxPower_qdBm()` limits the transmit power, in 0.25 dBm steps
(the unit of `esp_wifi_set_max_tx_power()` and `wifi_power_t`, so
`WIFI_POWER_8_5dBm` passes directly; the driver accepts 8..84 = 2..21 dBm). It is
applied on every connect between `WiFi.mode()` and `WiFi.begin()`, so the
association already runs at the limited power. 0 leaves the stack's default.
Two reasons to lower it: boards that fail to associate at full power (a poorly
matched antenna), and batteries that brown out on the transmit current peaks.

The server can override it with the config value `wifi_tx_qdbm` (0..84, 0 keeps
the firmware's setting). Since the connect precedes `updateConfig()`, the value
the connect sees is the NVS mirror of the last cycle, so a change on the server
takes effect from the next wakeup. A board that cannot connect at full power
never receives a server value: give it the setting in the firmware.

The SoftAP of the first-time provisioning uses the firmware's setting too,
since it describes the board; the server value is tuning for a site and does
not apply there (the handler runs before `begin()` applies it). Without a
firmware setting the AP keeps the stack's default.

There is no telemetry field for it: the ESP32 has no automatic transmit power
control, so the limit stays what was set. The connect logs the level the
driver actually uses, which may be the nearest supported one.

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
  accounting, the failure backoff and the WiFi fast-reconnect caches. It lives
  in an `IotRtcSlot` like any application state: the slot detects contents that
  did not survive (power-on reset, or a firmware whose layout changed) and
  hands out a zeroed struct, so every field's "nothing yet" is 0.

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
  reinitialized on every reset that is not a deep-sleep wakeup. `T` must have
  no constructor and no default member initializers — they would run at
  startup and overwrite what the slot keeps; a `static_assert` enforces it.

### What survives the cycle, and what a failure does to it

Four things carry data from one wakeup cycle into the next through RTC RAM.
They differ in who owns them and, deliberately, in what a failed request does:

| Building block | Owner | Holds | On a failed request |
|---|---|---|---|
| `IotRtcState` | library | boot count, sleep accounting, backoff, WiFi caches | — |
| RTC log (`IOT_RTC_LOG_SIZE`) | library | log lines that could not be sent | kept on a transport error or 5xx; oldest lines go first when full |
| `IotRtcValidators<N>` | application | ETag/Last-Modified of a cached resource | not committed, so the next cycle fetches again |
| `IotRtcTelemetry<N>` | application | values for the next post, as `last_*` | emptied anyway — a value two cycles late would be wrong |

The library's two are always there: it logs into RTC RAM before any
application code runs (a panic after an abnormal reset). The application's two
cost nothing until it declares them, inside its own `IotRtcSlot`.

**Budget:** RTC slow memory is 8 KB on ESP32 and ESP32-S3. The library takes
about 80 bytes of state plus `IOT_RTC_LOG_SIZE` (1536 by default); the rest
belongs to the application — and to a ULP program, if it has one.

## Firmware updates (OTA)

`iot.ota.update()` does a conditional check (ETag/Last-Modified) and streams
a new image only when one is available. The same TLS trust configuration used
for API calls applies to the OTA download. The download feeds the task
watchdog, so an image may take longer than one watchdog period.

The downloader (`esp_https_ota`) only accepts what the framework's sdkconfig
enables. The prebuilt arduino-esp32 has neither `CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP`
nor `CONFIG_ESP_TLS_INSECURE`, so OTA needs an `https://` URL with `tlsMode`
`Bundle` or `CaPin`; a plain `http://` API or `Insecure` works for API calls
but not for OTA. `update()` checks this before any request and returns
`STATUS_NOT_CONFIGURED` with the reason in the log.

The default apiPath is `file/{project}/{device}/firmware.bin`. A `{board}` 
parameter is supported and substitutes to the `IOT_BOARD_ID` build default (see
[`board`](#telemetry) / `Iot::boardId()`) — this lets a project serve
distinct firmware images per hardware variant from the same project/device
namespace. If `IOT_BOARD_ID` is undefined, `{board}` substitutes to an empty
string. `{board}` is available in **any** apiPath template, not just for
firmware (see `IotApi::urlForPath()`).

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
  in an `IotStore` (survives a power loss). `IotRtcValidators<N>` keeps them in
  RTC RAM, as a member of the struct in an `IotRtcSlot` — enough for a
  resource that is cheap to fetch once after power-on:
  ```cpp
  struct AppState { IotRtcValidators<> image; uint32_t cycles; };
  static RTC_NOINIT_ATTR IotRtcSlot<AppState> rtc;

  IotCachedResponse r = api.getCached(path, rtc->image.load());
  if (r.result.isOk() && render(r.body)) { rtc->image.commit(r.validators); }
  ```
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
`STATUS_NO_PROVISIONING_TOKEN`, `STATUS_MALFORMED_RESPONSE`, `STATUS_UPDATE_FAILED`,
and `STATUS_NOT_CONFIGURED` for a request that cannot be made at all: before
`begin()`, without a seeded API URL, or an OTA download this firmware cannot do.

A body that arrives shorter than announced (Content-Length, or a chunked
transfer without its last chunk) is a transport error
(`HTTPC_ERROR_READ_TIMEOUT`), not a success with a truncated body — otherwise
`getCached()` would keep the fragment under the new ETag.

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

### Where a transport error happened

A transport error code alone hardly tells a person what to check: HTTPClient
reports every failure to connect - an unknown name, a refused or unreachable
port, a rejected certificate, a stalled handshake - as `-1`. A `TransportError`
therefore also carries:

- `phase` (`IotTransportPhase`): `Dns`, `Connect`, `Tls`, `Send`, `Receive`,
  or `None` if unknown;
- `tlsError`: the mbedTLS code of a TLS failure, e.g. `-0x2700` for a
  certificate that does not verify;
- `elapsed_ms`: how long the request took (set for every request).

`api.errorText(result)` turns it into one line for a log or a display, e.g.
`TLS handshake failed (TLS -0x2700: X509 - Certificate verification failed) after 812 ms`,
`TLS handshake timed out after 10012 ms` or `read Timeout (receive) after 5012 ms`,
and the library logs exactly that line for a failed request. A failure while
setting up the connection (`Dns`, `Connect`, `Tls`) is described by its phase,
not by the transport's generic code.

`IotEspHttp` finds the phase like this: before a new connection it resolves the
server name itself (the connect that follows gets it from the DNS cache), so a
DNS failure is certain. A failed connect then has the mbedTLS code
`NetworkClientSecure` recorded for it - a TLS failure - or `-1`, which is a
refused connection, a TCP timeout or a TLS handshake timeout. The two timeouts
are told apart by the time they took, which works as long as the handshake
timeout is longer than the connect timeout. After the connect, HTTPClient's
codes are specific: `-2`/`-3` sending, `-5`/`-7`/`-11` receiving.

### Timeouts and the watchdog

Every phase of a request is bounded, so a stall ends the request instead of
the cycle:

| Phase | Bound | Set with |
|---|---|---|
| DNS | lwIP's resolver retries, not configurable | — |
| TCP connect | 5 s | `api.setConnectTimeout_ms()` |
| TLS handshake | 6 s, in steps of the connect timeout (so up to 10 s) | `api.setHandshakeTimeout_ms()` |
| response | 5 s per wait for data | `api.setRequestTimeout_ms()` |
| whole request | 60 s, checked while the body arrives | `api.setResponseTimeout_ms()` |

The TLS handshake needs its own bound: `NetworkClientSecure` waits up to
120 s by default, longer than any watchdog. `IotEspHttp::send()` resets the
task watchdog before each request, because a single `api.send()` can make
three of them (request, re-provisioning after a 401, retry), and again
whenever body data arrives. The per-wait response timeout alone would let a
body that trickles in - a byte every few seconds - run forever; the deadline
for the whole request ends it (`response took too long`, phase `receive`).
It may exceed the watchdog timeout, since the watchdog is fed only while
data arrives: a slow download of a large image completes, a stalled one
still ends after the per-wait timeout. Up to the first body byte, keep the
sum of the timeouts well below the watchdog timeout (20 s by default). The
response headers are bounded only per wait: HTTPClient offers no hook there.
The setters may be called before `iot.begin()`.

OTA downloads go through `esp_https_ota`, with their own timeout, and are not
covered by this.

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

`iot.setPanicHandler()` replaces what `panic()` does after logging;
`iot.defaultPanicHandler()` is the backoff, so a replacement can show a
message and then continue with it - the same pattern as the undervoltage
handler (see "Battery").

An application that must do something before the device sleeps supplies its own
`IotPower` (`iot.setPower()`), or registers `iot.addExitHook()` to run at every
exit — deep sleep, restart, shutdown and the backoff path alike.

`apProvisioning`'s response to an unfulfilled setup session ("First-time
provisioning" above) is a deliberately separate, simpler policy — indefinite
sleep rather than this escalating pattern — since unlike a reachability
failure, its triggering condition cannot resolve itself between wakeups.
