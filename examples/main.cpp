/**
 * arduino4iot example: complete wakeup cycle for a battery powered device.
 *
 * Connect WiFi, sync NTP, provision, update config and firmware,
 * post telemetry and go to deep sleep.
 */

#include <Arduino.h>
#include <iot.h>

// Deployment-specific values come from build-time -D defines (see
// secrets-example.ini and examples/README.md). A secretless build leaves these
// empty, so the values already seeded in NVS by an earlier flash are reused -
// this is what lets a public CI build firmware updates without secrets.
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

    // Seed the build-time bootstrap config into NVS (once; a no-op on a
    // secretless build with empty defaults). Bump IOT_SEED_GENERATION to push
    // changed values with a new firmware. For an https:// endpoint set
    // .tlsMode = IotTlsMode::CaPin with .caCertPem, or ::Bundle / ::Insecure.
    iot.seedCredentials({
        .wifiSsid          = IOT_WIFI_SSID,
        .wifiPassword      = IOT_WIFI_PASSWORD,
        .apiUrl            = IOT_API_URL,
        .projectName       = IOT_PROJECT,
        .provisioningToken = IOT_PROVISIONING_TOKEN,
        .tlsMode           = IotTlsMode::None,
        .seedGeneration    = IOT_SEED_GENERATION,
    });

    // shorten the per-wakeup radio-on time (optional, falls back automatically).
    // A static IP additionally skips DHCP - pass DNS too if the API URL uses a
    // hostname instead of an IP literal:
    //   iot.setStaticIp(IPAddress(192,168,178,42), IPAddress(192,168,178,1),
    //                   IPAddress(255,255,255,0));

    // connect WiFi (seeded credentials from NVS), init all subsystems, sync NTP
    if (!iot.begin())
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

    // fetch configuration and firmware updates, post system telemetry.
    // updateConfig()/updateFirmware() return IotResult: isOkOrNotModified()
    // means "no error" (downloaded a change, or already up to date), so a real
    // failure (server unreachable, rejected) is easy to single out.
    config.updateConfig();
    iot.resetWatchdog();
    IotResult fw = api.updateFirmware();
    if (!fw.isOkOrNotModified())
    {
        logger.warn("app", "firmware update failed (http=%d, transport=%d)",
            fw.httpStatus, fw.transportError);
    }
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
