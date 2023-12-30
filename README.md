# arduino4iot

ESP32 client library for Internet of Things applications.

## Introduction

The *x4iot system* consists of a client library (e.g. `arduino4iot`) and a simple server application. The interface is based on the HTTP protocol and relies on standard http headers like `etag` and `if-none-modified` for efficient communications. A complete client wakeup-cycle including establishing WiFi connection, sending application and system telemetry, checking configuration and firmware updates plus taking some quick measurements can be completing less than 900 ms, making *x4iot* a good choice for battery powered applications.

A *x4iot* server is intended for easy self hosting. Options include a Raspberry Pi in you home intranet or a virtual server at you cloud provider. The servers are responsible for
- provisioning and secure access using provisioning and API tokens
- storing telemetry in an [Influxdb](https://docs.influxdata.com/influxdb/v2/) time series database,
- storing logs in the same database
- providing firmware updates
- providing configuration and other files to IoT clients

Server implementations are [j4iot](https://github.com/clausgf/j4iot) or [py4iot](https://github.com/clausgf/py4iot), please refer to the respective repositories. 


## Getting started

1. Add the library to the `libdeps` section of your `platformio.ini`:
   ```
   lib_deps =
       https://github.com/clausgf/arduino4iot
   ```
1. Initialize the library and do something useful in your `setup()` function:
    ```
    api.setApiUrl("http://192.168.178.20/iot/api");
    api.setProvisioningToken("P-102-FX61O...==");
    api.setProjectName("thingpulse");
    if (!iot.begin(WIFI_SSID, WIFI_PASSWORD))
    {
        iot.panic("*** PANIC *** WiFi connection or NTP sync failed");
    }
    if (!api.updateProvisioningOk())
    {
        iot.panic("*** PANIC *** Provisioning failed");
    }
    config.updateConfig();
    iot.resetWatchdog();
    api.updateFirmware();
    iot.postSystemTelemetry();

    // measure something
    String telemetry_json = String("{\"temperature\":") + temperature + "}";
    iot.postTelemetry("sensors", telemetry_json);

    iot.deepSleep();
    ```

## Tips
- Browse the header files of the library, there is some doxygen style documentation.
- To distribute the new firmware to the server for OTA updates via ssh. Add the following lines to you `platformio.ini`:
  ```
  upload_protocol = custom
  upload_command = scp $SOURCE garnix.local:/home/username/docker/j4iot/iot-data/projectname
  ```
- Firmware update via http (instead of https) requires an IDF SDKCONFIG different from the standard Arduino one with `CONFIG_OTA_ALLOW_HTTP=y`. API calls different from the
firmware update 
