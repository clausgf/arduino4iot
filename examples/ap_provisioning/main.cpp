/**
 * arduino4iot example: first-time provisioning over a SoftAP + captive portal.
 *
 * Same wakeup cycle as examples/main.cpp, plus a setup flow for a device that
 * has no seeded WiFi credentials yet - a factory-fresh board, or one after
 * iot.clearProvisioning()/iot.factoryReset().
 *
 * The AP module is opt-in: it costs roughly 42 KB of flash (DNSServer +
 * WebServer), which is a lot on the stock esp32dev dual-OTA partition scheme.
 * Because Iot::begin() only calls a handler and never names IotAp itself, a
 * firmware that does not register the handler below does not link any of it.
 * Compare the two builds if you want to see it - examples/main.cpp is the same
 * sketch without the setup flow.
 */

#include <Arduino.h>
#include <iot.h>
#include <iot_ap.h>   // pulls in DNSServer + WebServer: only referenced here

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

    // A build with no -D secrets seeds nothing, so a fresh device reaches
    // begin() without credentials - which is what triggers the handler below.
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
    // Without this line begin() would log an error and shut down instead.
    apProvisioning.setSsidPrefix("my-sensor-setup-");
    apProvisioning.setIdleTimeout_s(600);
    iot.setNoCredentialsHandler([] { apProvisioning.run(); });

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
    iot.deepSleep();
}

void loop()
{
    // never reached: setup() ends in deep sleep
}
