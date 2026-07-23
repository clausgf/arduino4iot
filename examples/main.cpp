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
    // for an https:// URL, pick one server-trust option:
    //   api.setCACert(ca_cert_pem); // pin your server's CA (self-hosted)
    //   api.setCACertBundle();      // verify against the public Mozilla root bundle
    //   api.setCertInsecure();      // development only: no server verification

    // connect WiFi, initialize all subsystems, sync NTP time
    if (!iot.begin(WIFI_SSID, WIFI_PASSWORD))
    {
        iot.panic("*** PANIC *** WiFi connection or NTP sync failed");
    }

    // provision the device (re-provisions automatically before token expiry).
    // updateProvisioning() returns a typed result so a headless device can tell
    // apart the very different failure causes (see the "if" branches below).
    IotResult prov = api.updateProvisioning();
    if (!prov)
    {
        if (prov.isTransportError())
        {
            iot.panic("*** PANIC *** No server connection (check TLS/CA cert), transport=%d",
                prov.transportError);
        }
        else if (prov.httpStatus == 403)
        {
            iot.panic("*** PANIC *** Provisioning rejected - check token / device approval");
        }
        else if (prov.httpStatus == IotResult::STATUS_NO_PROVISIONING_TOKEN)
        {
            iot.panic("*** PANIC *** No provisioning token configured");
        }
        else
        {
            iot.panic("*** PANIC *** Provisioning failed (http=%d)", prov.httpStatus);
        }
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
