/**
 * arduino4iot example: a fully equipped device.
 *
 * The same wakeup cycle as examples/main.cpp, plus everything that is opt-in
 * because the core never names it and the linker would otherwise drop it: an
 * SNTP clock, a battery on an ADC pin, a SoftAP setup flow for a device with no
 * seeded credentials, and the file-upload endpoint.
 *
 * examples/main.cpp is the same sketch without any of it - compare the two
 * builds to see what each costs.
 */

#include <Arduino.h>
#include <iot.h>
#include <iot_ap.h>          // pulls in DNSServer + WebServer
#include <iot_endpoints.h>   // iotUploadFile()
#include <iot_esp_ports.h>   // IotNtpClock, IotAdcBattery

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

    // A real clock. Needed for an https:// endpoint, whose certificate validity
    // is checked against it. The servers and the resync interval are
    // server-configurable (ntp_server1..3, ntp_resync_s).
    static IotNtpClock ntpClock(iotRtcState().ntpLastSyncTime);
    iot.setClock(ntpClock);

    // Supply voltage behind a divider on an ADC pin, averaged over 4 samples.
    // An I2C fuel gauge would go through IotCallbackBattery instead, without
    // the library noticing the difference.
    static IotAdcBattery battery({ .pin = 34, .factor = 2, .divider = 1, .samples = 4 });
    iot.setBattery(battery);
    iot.setBatteryMin_mV(3300);

    if (!iot.begin())
    {
        iot.panic("*** PANIC *** WiFi connection or NTP sync failed");
    }

    if (!api.updateProvisioningOk())
    {
        iot.panic("*** PANIC *** Provisioning failed");
    }

    config.updateConfig();
    iot.postSystemTelemetry();

    if (!iot.isTimePlausible())
    {
        iotUploadFile("clock.txt", "the clock never synchronized", "text/plain");
    }

    iot.deepSleep();
}

void loop()
{
    // never reached: setup() ends in deep sleep
}
