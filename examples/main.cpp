/**
 * arduino4iot example: complete wakeup cycle for a battery powered device.
 *
 * Connect WiFi, sync NTP, provision, update config and firmware,
 * post telemetry and go to deep sleep.
 */

#include <Arduino.h>
#include <iot.h>

// define credentials here or via build_flags in platformio.ini
#ifndef WIFI_SSID
#define WIFI_SSID "my-wifi"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "my-password"
#endif

void setup()
{
    Serial.begin(115200);

    // configure API access to a nice4iot server
    api.setApiUrl("http://192.168.178.20:8000/api");
    api.setProjectName("my-project");
    api.setProvisioningTokenIfEmpty("P-102-FX61O...==");
    // api.setCACert(ca_cert_pem);  // optional: pin the server certificate for https

    // connect WiFi, initialize all subsystems, sync NTP time
    if (!iot.begin(WIFI_SSID, WIFI_PASSWORD))
    {
        iot.panic("*** PANIC *** WiFi connection or NTP sync failed");
    }

    // provision the device (re-provisions automatically before token expiry)
    if (!api.updateProvisioningOk())
    {
        iot.panic("*** PANIC *** Provisioning failed");
    }

    // fetch configuration and firmware updates, post system telemetry
    config.updateConfig();
    iot.resetWatchdog();
    api.updateFirmware();
    iot.postSystemTelemetry();

    // measure something and post it as telemetry
    IotTelemetry telemetry;
    telemetry.add("temperature", 22.5);
    telemetry.add("humidity", 40);
    IotResult result = iot.postTelemetry("sensors", telemetry);
    if (!result)
    {
        logger.warn("app", "telemetry post failed (http=%d, transport=%d)",
            result.httpStatus, result.transportError);
    }

    // sleep until the next cycle (duration from config value "sleep_s")
    iot.deepSleep();
}

void loop()
{
    // never reached: setup() ends in deep sleep
}
