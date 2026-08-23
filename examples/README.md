# arduino4iot example

A complete deep-sleep wakeup cycle: seed the configuration, connect WiFi, sync
NTP, provision, update config and firmware, post telemetry, sleep. See
[`main.cpp`](main.cpp).

## Configuration

All deployment-specific values (WiFi credentials, API endpoint, project name,
provisioning token, TLS trust) are provided **at build time** via `-D` defines,
written into NVS on the first boot (*seeding*) and then left untouched. This lets
you flash **secretless** firmware updates - e.g. from a public CI - because the
values already live in the device's NVS.

1. Copy [`secrets-example.ini`](secrets-example.ini) to `secrets.ini` (keep it
   out of version control) and fill in your values.
2. Build & flash the `initial` environment **once** - this seeds NVS.
3. For subsequent OTA updates, build the plain `esp32dev` environment **without**
   secrets. The empty defaults make `seedCredentials()` a no-op, so NVS is
   preserved.

`secrets-example.ini` contains the full `platformio.ini` wiring in its header
comment.

### https / certificates

For an `https://` endpoint, seed a TLS mode in `seedCredentials()`:
`IotTlsMode::CaPin` with `caCertPem` (pin your server's CA - typical for a
self-hosted nice4iot), `IotTlsMode::Bundle` (public Mozilla roots), or
`IotTlsMode::Insecure` (development only). A CA PEM is easier to embed in the
sketch as a `const char[]` than to pass as a `-D` define.

### Reset / re-seeding

- Bump `IOT_SEED_GENERATION` and flash a firmware containing secrets to
  deliberately overwrite the seeded values.
- `iot.factoryReset()` erases all persisted NVS state; the next boot must run a
  firmware that re-seeds.
- `iot.clearProvisioning()` erases only the seed/provisioning state (WiFi,
  API endpoint, project, provisioning token, TLS trust, device token) -
  `iot-cfg`/`iot-var` (server config mirror, library runtime values) are left
  alone. Since this empties the seeded WiFi SSID, the next `begin()` enters
  AP provisioning mode (below).

### Provisioning without a firmware rebuild

The seeded values can also be written directly into an NVS partition image
with `nvs_partition_gen.py` - useful for initial/browser-based flashing where
building per-device firmware isn't practical. See
[`nvs_seed_template.csv`](nvs_seed_template.csv) and
[docs/concepts.md#nvs-schema-stable-interface](../docs/concepts.md#nvs-schema-stable-interface)
for the key reference.

### First-time provisioning: SoftAP + captive portal

If no WiFi SSID is seeded (a factory-fresh device, or one after
`iot.clearProvisioning()`), `iot.begin()` automatically opens a setup AP and
serves a small web form instead of attempting to connect - see
[docs/concepts.md](../docs/concepts.md), "First-time provisioning: SoftAP +
captive portal", for the full design (trigger, no-popup UX, backoff policy).

```cpp
apProvisioning.setSsidPrefix("myproject-setup-");  // default "arduino4iot-setup-"
apProvisioning.setIdleTimeout_s(300);              // default 300 (5 min)
```

Connect to the open `<prefix><last 4 MAC hex chars>` WiFi network and open
`http://192.168.4.1/` in a normal browser (there is deliberately no
captive-portal popup). A QR code encoding the same URL with query parameters
pre-fills the form - e.g.
`http://192.168.4.1/?apiUrl=https://api.example.com&project=my-project&provisioningToken=P-102-...`
- the user still has to tap "Save" to actually commit it. If the 5-minute
idle window elapses with nobody connecting, the device sleeps indefinitely;
only a physical reset/power-cycle tries again.

**Flash footprint**: on the `esp32dev` board's default (dual-OTA) partition
scheme, this module adds roughly 42 KB (~3 percentage points) on top of an
already-tight ~95% baseline for the plain library + a minimal sketch -
budget a larger/custom partition table if you enable it on a flash-constrained
board.

## Requirements

- arduino-esp32 3.x via the [pioarduino](https://github.com/pioarduino/platform-espressif32)
  platform, C++20 (`-std=gnu++20`).
