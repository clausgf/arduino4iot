# On-device tests

The native tests (`pio test -e native`) cover everything that is Arduino-free.
What only the hardware can show - sockets, TLS, WiFi, NVS, the watchdog - is
covered by on-device tests, run by hand against a devkit on USB. They are not
part of CI: the CI workflow runs `pio test -e native` only, and needs no
device, no WiFi and no test server.

## Running

```bash
pio test -e device                        # all on-device suites
pio test -e device -f device/test_http    # one suite
pio test -e device -v                     # with the device log and measurements
```

What it takes:

- An **ESP32 DevKit v1** (`board = esp32dev`) on USB. The port is detected;
  pass `--upload-port`/`--test-port` if there are several.
- **WiFi credentials.** By default the ones seeded into the device's NVS
  (namespace `iot`), so a device that has run a firmware with arduino4iot
  needs nothing. Otherwise set `DEVICE_TEST_WIFI_SSID` and
  `DEVICE_TEST_WIFI_PASSWORD`. The tests only read the `iot` namespace; they
  write to `iot-test` only.
- The device has to **reach this host** on TCP ports 18080-18082.
  `DEVICE_TEST_HOST` overrides the address (default: the one of the host's
  default route), `DEVICE_TEST_PORT_BASE` the ports.
- `openssl` on the host, for the test certificates.

Flashing a test replaces the application on the device; NVS stays. Flash the
real firmware again afterwards.

## How it works

This is PlatformIO's own mechanism: Unity tests on the target, with a
[custom test runner](https://docs.platformio.org/en/latest/advanced/unit-testing/frameworks/custom/index.html).
`[env:device]` in `platformio.ini` sets `test_framework = custom` and
`test_filter = device/*`; `[env:native]` ignores `device/*`.

`test/device/test_custom_runner.py` extends PlatformIO's `UnityTestRunner`.
Before the first build it

1. generates a test CA, an unrelated CA and a server certificate for the host
   address (EC P-256), in `test/device/generated/` (git-ignored, reused while
   the address stays the same);
2. writes `test/device/generated/device_test_env.h`: host, ports, WiFi
   override, the two CA certificates;
3. starts the test server, which runs until `pio test` ends:
   - `base`: HTTP, a small hand-written server that can also misbehave on
     purpose (trickling headers or body, a stall, a malformed status line,
     dropping a request on a kept-alive connection);
   - `base+1`: the same over TLS with the test CA's certificate;
   - `base+2`: accepts TCP and never answers the TLS handshake;
   - `base+3`: closed.

The suites share `test/device/device_test_support.h` (WiFi from NVS, URLs).
The WiFi transmit power is limited to 8.5 dBm: a devkit on a weak USB supply
can brown out at full power.

## Suites

| Suite | What it covers |
|---|---|
| `device/test_platform` | `IotNvsStore` round trip and integer widths, MAC before the connect, WiFi connect and reconnect with the transmit power in effect; reports IP, RSSI and connect time |
| `device/test_http` | `IotIdfHttp`: Content-Length, chunked and until-close bodies, HEAD/204/304, POST with small and large bodies, keep-alive, repeating a request the server dropped; the phase and the bound of DNS failure, closed port, connect timeout, read timeout, trickling body and headers (whole-request deadline), malformed response; TLS with pinned CA, insecure, wrong CA (`-0x2700`), handshake timeout; the watchdog fed through an 8 s download under a 3 s watchdog. `IotEspHttp`: the basic cases as a fallback check. A throughput measurement of both (informational) |

Timing assertions allow ±700 ms. The throughput depends on the radio link
far more than on the code: at -86 dBm with 3 % packet loss, downloads beyond
~20 KB are dominated by TCP retransmits. Place the devkit with a good signal
(the platform suite reports the RSSI) and compare measurements made at the
same place.

## Not covered yet

Deep sleep and the RTC state across it, OTA, AP provisioning, and the
`iot` facade against a real nice4iot server. The first needs a test that
spans a reset; the last writes to a server project and needs its own
provisioning.
