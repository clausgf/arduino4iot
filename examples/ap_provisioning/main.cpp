/**
 * arduino4iot example: a fully equipped device.
 *
 * The same wakeup cycle as examples/main.cpp, plus everything that is opt-in
 * because the core never names it and the linker would otherwise drop it: an
 * SNTP clock, a battery on an ADC pin, a SoftAP setup flow for a device with no
 * seeded credentials, the file-upload endpoint, and application state in RTC
 * RAM and NVS.
 *
 * examples/main.cpp is the same sketch without any of it - compare the two
 * builds to see what each costs.
 */

#include <Arduino.h>
#include <iot.h>
#include <iot_ap.h>          // pulls in DNSServer + WebServer
#include <iot_esp_ports.h>   // IotNtpClock, IotAdcBattery, IotNvsStore
#include <iot_rtc_slot.h>    // IotRtcSlot

#ifndef IOT_WIFI_SSID
#define IOT_WIFI_SSID ""
#endif
#ifndef IOT_WIFI_PASSWORD
#define IOT_WIFI_PASSWORD ""
#endif
#ifndef IOT_API_URL
#define IOT_API_URL ""
#endif
#ifndef IOT_PROJECT
#define IOT_PROJECT ""
#endif
#ifndef IOT_PROVISIONING_TOKEN
#define IOT_PROVISIONING_TOKEN ""
#endif
#ifndef IOT_SEED_GENERATION
#define IOT_SEED_GENERATION 0
#endif

// An application value in config.json. The metadata describes it in the
// published config.schema.json (a typed form on the server) and is enforced:
// a value outside the range from config.json or NVS is clamped.
static IotConfigValue<int32_t> sampleInterval(config, 60, "sample_s",
    { .title = "Sample interval (s)", .description = "Seconds between measurements",
      .min = 10, .max = 3600 });

// Application state that only has to survive deep sleep: RTC RAM, validated
// so a power-on reset or an OTA update hands out a fresh value, not garbage.
struct AppRtcState
{
    uint32_t cycles;
    bool clockReported;
};
static RTC_NOINIT_ATTR IotRtcSlot<AppRtcState> appRtc;

// Application state that must survive a power loss: its own NVS namespace,
// which factoryReset() leaves alone.
static IotNvsStore appStore("myapp");

void setup()
{
    Serial.begin(115200);

    // a build with no -D secrets seeds nothing, so a fresh device reaches
    // begin() without credentials - which triggers the handler below
    iot.seedCredentials({
        .wifiSsid          = IOT_WIFI_SSID,
        .wifiPassword      = IOT_WIFI_PASSWORD,
        .apiUrl            = IOT_API_URL,
        .projectName       = IOT_PROJECT,
        .provisioningToken = IOT_PROVISIONING_TOKEN,
        .tlsMode           = IotTlsMode::None,
        .seedGeneration    = IOT_SEED_GENERATION,
    });

    // Open a setup AP when there are no credentials. apProvisioning.run()
    // blocks and does not return: a submitted form ends in iot.restart() so the
    // freshly seeded NVS values take effect, an idle timeout in iot.shutdown().
    apProvisioning.setSsidPrefix("my-sensor-setup-");
    apProvisioning.setIdleTimeout_s(600);
    iot.setNoCredentialsHandler([] { apProvisioning.run(); });

    // A real clock, for timestamps and to renew the device token before it
    // expires. TLS does not need it: arduino-esp32 does not check certificate
    // validity dates.
    static IotNtpClock ntpClock(iotRtcState().ntpLastSyncTime);
    iot.setClock(ntpClock);

    // Supply voltage behind a divider on an ADC pin, averaged over 4 samples.
    // An I2C fuel gauge would go through IotCallbackBattery instead, without
    // the library noticing the difference.
    static IotAdcBattery battery({ .pin = 34, .factor = 2, .divider = 1, .samples = 4 });
    iot.setBattery(battery);
    iot.setBatteryMin_mV(3300);

    // Reuse the DHCP lease from RTC RAM on the next wakeups, renewing it via
    // DHCP every 20th connect. Tunes the built-in WiFi station.
    iot.wifi().setDhcpCache(true);

    if (!iot.begin())
    {
        iot.panic("*** PANIC *** network connection failed");
    }

    // the status goes into the remote log with the panic message
    IotResult prov = api.updateProvisioning();
    if (!prov)
    {
        iot.panic("*** PANIC *** Provisioning failed (http=%d, transport=%d)",
            prov.httpStatus, prov.transportError);
    }

    config.updateConfig();
    // the form layout is presentation, so it stays with the application
    config.setSchemaExtras(R"({"x-ui": {"layout": [["sample_s", "sleep_s"]]}})");
    config.publishSchema();   // once per changed schema, e.g. after an update
    iot.telemetry.postSystem();

    // a lost RTC slot means a power-on (or a new firmware layout): count it
    appStore.begin();
    if (!appRtc.isValid())
    {
        appStore.putInt("powerOns", appStore.getInt("powerOns", 0) + 1);
    }
    appRtc->cycles++;

    // report an unsynchronized clock once per power-on, not every wakeup
    if (!iot.isTimePlausible() && !appRtc->clockReported)
    {
        appRtc->clockReported =
            (bool)iot.files.upload("clock.txt", "the clock never synchronized", "text/plain");
    }

    // The network part is done: radio off for the rest of the cycle, e.g.
    // before a long display refresh. What is logged from here on is kept in
    // RTC RAM and sent with the next cycle.
    iot.radioOff();
    logger.info("app", "cycle %u done offline", (unsigned)appRtc->cycles);

    iot.deepSleep();
}

void loop()
{
    // never reached: setup() ends in deep sleep
}
