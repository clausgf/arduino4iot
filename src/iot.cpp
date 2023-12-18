/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#include "iot.h"

#include "cstdio"
#include <esp_system.h>
#include <esp_sleep.h>
#include <esp_wifi.h>
#include <esp_ota_ops.h>
#include <nvs_flash.h>
#include <esp_sntp.h>
#include <WiFi.h>
#include <Preferences.h>
#include <HttpsOTAUpdate.h>
#include <ArduinoJson.h>

// *****************************************************************************

Iot iot;
static const char *tag = "iot";

RTC_DATA_ATTR static int32_t rtcBootCount;
RTC_DATA_ATTR static int64_t rtcActiveDuration_ms;
RTC_DATA_ATTR static int32_t rtcLastSleepDuration_s;
RTC_DATA_ATTR static int64_t rtcNtpLastSyncTime;
RTC_DATA_ATTR static int32_t rtcPanicSleepDuration_s;

static void defaultPanicHandler()
{
    iot.escalatingSleepPanicHandler();
}

// TODO add wrapper class for RTC_DATA_ATTR or NVRAM based persistence

// *****************************************************************************

Iot::Iot() :
    _bootCount(&rtcBootCount),
    _activeDuration_ms(&rtcActiveDuration_ms),
    _lastSleepDuration_s(&rtcLastSleepDuration_s),
    _panicSleepDuration_s(&rtcPanicSleepDuration_s),
    _ntpLastSyncTime(&rtcNtpLastSyncTime),
    _logLevel(config, IotLogger::LogLevel::IOT_LOGLEVEL_NOTSET, "log_level", "logLevel"),
    _sleepDuration_s(config, 5 * 60, "sleep_s", "sleepFor"),
    _ntpResyncInterval_s(config, 24 * 60 * 60, "ntp_resync_s", "ntpResync"),
    _batteryOffset_mV(config, 0, "battery_offset_mV", "batOffs"),
    _batteryDivider(config, 1, "battery_divider", "batDiv"),
    _batteryFactor(config, 2, "battery_factor", "batMul"),
    _batteryPin(config, 34, "battery_pin", "batPin"),
    _batteryMin_mV(config, -1, "battery_min_mV", "batMinMv"),
    _panicSleepDurationInit_s(config, 60, "panic_sleep_init_s", "panicSlpInit"),
    _panicSleepDurationMax_s(config, 24 * 60 * 60, "panic_sleep_max_s", "panicSlpMax"),
    _panicSleepDurationFactor(config, 2, "panic_sleep_factor", "panicSlpFac")
{
    // initialize variables
    _bootTimestamp_ms = millis();
    _wakeupCause = esp_sleep_get_wakeup_cause();
    _bootCount = _bootCount + 1;
    // _activeDuration_ms is set on shutdown
    // _lastSleepDuration_s is set on shutdown
    _deviceId = "";
    _battery_mV = -1;
    // _panicSleepDuration_s is set on panic
    _panicHandler = defaultPanicHandler;
    _firmwareVersion = "";
    _firmwareSha256 = "";

    // initialize NVRAM
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK)
    {
        log_e("nvs_flash_init failed: %s", esp_err_to_name(err));
        return;
    }

    // startup logging
    log_i("--- Bootup #%lu, cause %d after %d s, panicSleepDuration=%d s",
            getBootCount(), (int)getWakeupCause(), getLastSleepDuration_s(), _panicSleepDuration_s);
    if (WiFi.status() == WL_CONNECTED)
    {
        if (_panicSleepDuration_s >= 0)
        {
            log_i("*** LAST STARTUP WAS A PANIC, panicSpeepDuration=%d s", _panicSleepDuration_s);
        }
    }
    log_i("--- Firmware %s", getFirmwareVersion().c_str());
    log_i("--- SHA256 %s", getFirmwareSha256().c_str());
}

void Iot::begin(bool isRtcAvailable)
{
    if (WiFi.status() != WL_CONNECTED)
    {
        log_e("WiFi not connected yet. Connect WiFi first.");
    }

    // initialize persistent variables
    if (!isRtcAvailable)
    {
        //_bootCount.setStorage(IotPersistentValue<int32_t>::IOT_STORAGE_NVRAM_IMPLICIT);
        //_activeDuration_ms.setStorage(IotPersistentValue<int64_t>::IOT_STORAGE_NVRAM_IMPLICIT);
        //_lastSleepDuration_s.setStorage(IotPersistentValue<int32_t>::IOT_STORAGE_NVRAM_IMPLICIT);
        _ntpLastSyncTime.setStorage(IotPersistentValue<int64_t>::IOT_STORAGE_NVRAM_IMPLICIT);
        _panicSleepDuration_s.setStorage(IotPersistentValue<int32_t>::IOT_STORAGE_NVRAM_IMPLICIT);
    }

    // read configuration again to allow overwriting parameters with WiFi config
    config.begin();
    logger.begin();

    // check the battery voltage
    if (_batteryPin >= 0 && _batteryMin_mV > 0)
    {
        int batteryVoltage_mV = getBatteryVoltage_mV();
        if (batteryVoltage_mV < _batteryMin_mV)
        {
            log_e("Battery voltage too low: %d mV", batteryVoltage_mV);
            shutdown(true);
        }
    }

    // initialize other components
    api.setDeviceName(getDeviceId());
    api.begin();
}

void Iot::end()
{
    api.end();
    logger.end();
    config.end();
}

// *****************************************************************************

bool Iot::connectWifi(const char *ssid, const char *password, unsigned long timeout_ms)
{
    // immediately return if already connected
    if (WiFi.status() == WL_CONNECTED)
    {
        log_i("WiFi already connected ip=%s", WiFi.localIP().toString().c_str());
        return true;
    }

    log_i("Connecting to WiFi network ssid=%s timeout=%lu ms", ssid, timeout_ms);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    // wait for connection
    unsigned long startTime = millis();
    while ( (WiFi.status() != WL_CONNECTED) && ((millis() - startTime) < timeout_ms) )
    {
        delay(50);
    }

    // check for failed connection due to timeout
    if (WiFi.status() != WL_CONNECTED)
    {
        log_e("WiFi connection failed");
        return false;
    }

    log_i("WiFi connected ip=%s", WiFi.localIP().toString().c_str());
    return true;
}

// *****************************************************************************

String Iot::getDeviceId()
{
    if (WiFi.status() != WL_CONNECTED)
    {
        log_e("WiFi not connected yet. Connect WiFi first.");
    } else {
        // determine device id
        uint8_t mac[6];
        esp_wifi_get_mac(WIFI_IF_STA, mac);

        const int ID_MAXLEN = 17;
        static char id_buf[ID_MAXLEN];
        snprintf(id_buf, ID_MAXLEN, "e32-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

        _deviceId = id_buf;
        WiFi.setHostname(id_buf);
        log_i("WiFi device determined and set as the hostname: %s", id_buf);
    }

    return _deviceId;
}


// *****************************************************************************
// NTP time
// *****************************************************************************

String Iot::getTimeIso(time_t time)
{
    struct tm timeinfo;
    gmtime_r(&time, &timeinfo);

    const int BUFLEN = 32;
    static char buf[BUFLEN];
    snprintf(buf, BUFLEN, "%04d-%02d-%02dT%02d:%02d:%02dZ", 
        timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, 
        timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    return buf;
}

String Iot::getTimeIso()
{
    time_t now = time(nullptr);
    return getTimeIso(now);
}

// *****************************************************************************

bool Iot::isTimePlausible()
{
    time_t implausibleTimeThreshold = 40 * 365 * 24 * 3600l;
    time_t now = time(nullptr);
    return (now > implausibleTimeThreshold);
}

bool Iot::waitUntilTimePlausible(unsigned long timeout_ms)
{
    log_i("Waiting for NTP time sync");
    unsigned long startTime = millis();
    while ( !isTimePlausible() && ((millis() - startTime) < timeout_ms) )
    {
        delay(50);
    }
    return isTimePlausible();
}

// *****************************************************************************

bool Iot::waitUntilNtpSync(unsigned long timeout_ms)
{
    log_i("Waiting for NTP time sync");
    unsigned long startTime = millis();
    bool completed = ( esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED );
    while ( !completed && ((millis() - startTime) < timeout_ms) )
    {
        delay(50);
        bool completed = ( esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED );
    }
    return completed;
}

void Iot::_ntpSyncCallback(struct timeval *tv)
{
    time_t now = time(nullptr);
    log_i("NTP time sync success, time=%s", iot.getTimeIso(now).c_str());
    iot._ntpLastSyncTime = now;
}

// *****************************************************************************

bool Iot::syncNtpTime(const char* ntpServer1, const char *ntpServer2, const char *ntpServer3, unsigned long timeout_ms)
{
    int64_t sinceLastSync_s = time(nullptr) - _ntpLastSyncTime;
    if (isTimePlausible() && sinceLastSync_s >= 0 && sinceLastSync_s < _ntpResyncInterval_s)
    {
        log_i("NTP time should be good enough: time=%s", getTimeIso().c_str());
        return true;
    }

    // initialize NTP
    esp_netif_init();
    if (esp_sntp_enabled())
    {
        esp_sntp_stop();
    }
    esp_sntp_servermode_dhcp(1);
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, ntpServer1);
    if (ntpServer2 != nullptr)
    {
        esp_sntp_setservername(1, ntpServer2);
    }
    if (ntpServer3 != nullptr)
    {
        esp_sntp_setservername(2, ntpServer3);
    }
    esp_sntp_set_time_sync_notification_cb(_ntpSyncCallback);
    esp_sntp_init();

    configTime(0, 0, ntpServer1, ntpServer2, ntpServer3);

    // wait for time to be set
    waitUntilTimePlausible(timeout_ms);
    if (!isTimePlausible())
    {
        log_i("NTP time sync failed: %s", getTimeIso().c_str());
        return false;
    }

    log_i("NTP time sync success, time=%s", getTimeIso().c_str());
    return true;
}


// *****************************************************************************
// API
// *****************************************************************************

int Iot::postTelemetry(String kind, String jsonData, String apiPath)
{
    apiPath.replace("{kind}", kind);
    // other variables are replaced in apiPost()
    String oResult = "";
    return api.apiPost(oResult, apiPath, jsonData);
}

// *****************************************************************************

int Iot::postSystemTelemetry(String kind, String apiPath)
{
    String jsonData = String("{") 
        + "\"battery_V\":" + String(getBatteryVoltage_mV()/1000.0, 2)
        + ",\"wifi_rssi\":" + WiFi.RSSI() 
        + ",\"boot_count\":" + getBootCount() 
        + ",\"active_ms\":" + getActiveDuration_ms() 
        + ",\"lastSleep_s\":" + getLastSleepDuration_s()
        + ",\"time\":\"" + getTimeIso() + "\""
        + ",\"firmware_version\":\"" + getFirmwareVersion() + "\""
        + ",\"firmware_sha256\":\"" + getFirmwareSha256() + "\""
        + "}";
    return postTelemetry(kind, jsonData, apiPath);
}


// *****************************************************************************
// Battery
// *****************************************************************************

int Iot::getBatteryVoltage_mV()
{
    if ( _batteryPin < 0 )
    {
        log_e("Battery voltage measurement not configured");
        return -1;
    }
    if (_battery_mV <= 0)
    {
        uint32_t raw = analogReadMilliVolts(_batteryPin);
        int64_t voltage = raw;
        voltage = voltage * _batteryFactor;
        voltage = voltage / _batteryDivider;
        voltage = voltage + _batteryOffset_mV;
        _battery_mV = voltage;
        log_i("Battery voltage: pin=%d, raw=%d battery_voltage=%d mV", 
            _batteryPin, raw, _battery_mV);
    }
    return _battery_mV;
}


// *****************************************************************************
// Error handling
// *****************************************************************************

void Iot::panic(const char* format...)
{
    va_list args;
    va_start(args, format);
    logger.logv(IotLogger::LogLevel::IOT_LOGLEVEL_ERROR, tag, format, args);
    va_end(args);

    delay(50);  // delay to allow log to be written
    _panicHandler();
}

void Iot::panicEarly(const char* format...)
{
    va_list args;
    va_start(args, format);
    const int logBufLen = 160;
    char logBuf[logBufLen];
    vsnprintf(logBuf, logBufLen, format, args);
    log_e("%s", logBuf);
    va_end(args);

    delay(50);  // delay to allow log to be written
    _panicHandler();
}

// *****************************************************************************

void Iot::escalatingSleepPanicHandler()
{
    if (_panicSleepDuration_s <= 0)
    {
        // first panic, last run was successful
        _panicSleepDuration_s = _panicSleepDurationInit_s;
        restart(true);
    } else {
        _panicSleepDuration_s = _panicSleepDuration_s * _panicSleepDurationFactor;
        if (_panicSleepDuration_s > _panicSleepDurationMax_s)
        {
            _panicSleepDuration_s = _panicSleepDurationMax_s;
        }
        deepSleep(_panicSleepDuration_s, true);
    }
}

std::function<void()> Iot::setPanicHandler(std::function<void()> panicHandler)
{
    std::function<void()> oldPanicHandler = _panicHandler;
    _panicHandler = panicHandler;
    return oldPanicHandler;
}


// *****************************************************************************
// System management: firmware, sleep, restart, shutdown
// *****************************************************************************

String Iot::getFirmwareVersion()
{
    if (_firmwareVersion.isEmpty())
    {
        const esp_partition_t *running = esp_ota_get_running_partition();
        esp_app_desc_t app_info;
        if (esp_ota_get_partition_description(running, &app_info) == ESP_OK)
        {
            _firmwareVersion = String(app_info.project_name) 
                + " " + String(app_info.version) 
                + " " + String(app_info.date) 
                + " " + String(app_info.time) 
                + " IDF " + String(app_info.idf_ver) 
                + " sec " + String(app_info.secure_version)
                + " ARDUINO " + ESP_ARDUINO_VERSION_MAJOR + "." + ESP_ARDUINO_VERSION_MINOR + "." + ESP_ARDUINO_VERSION_PATCH;
        }
    }
    return _firmwareVersion;
}

String Iot::getFirmwareSha256()
{
    if (_firmwareSha256.isEmpty())
    {
        const esp_partition_t *running = esp_ota_get_running_partition();
        esp_app_desc_t app_info;
        if (esp_ota_get_partition_description(running, &app_info) == ESP_OK)
        {
            char buf[sizeof(app_info.app_elf_sha256) * 2 + 1];
            for (int i = 0; i < sizeof(app_info.app_elf_sha256); i++)
            {
                sprintf(buf + i * 2, "%02x", app_info.app_elf_sha256[i]);
            }
            buf[sizeof(buf) - 1] = '\0';
            _firmwareSha256 = String(buf);
        }
    }
    return _firmwareSha256;
}

// *****************************************************************************

void Iot::setSleepDuration_s(int sleep_duration_s)
{
    _sleepDuration_s = sleep_duration_s;
}

void Iot::deepSleep()
{
    deepSleep(_sleepDuration_s);
}

void Iot::deepSleep(int sleep_duration_s, bool panic)
{
    if (!panic)
    {
        _panicSleepDuration_s = -1; // regular shutdown, reset panic sleep duration
    }

    _lastSleepDuration_s = sleep_duration_s;
    _activeDuration_ms = millis() - _bootTimestamp_ms;
    log_i("Active for %d ms, going to deep sleep for %d s", _activeDuration_ms, sleep_duration_s);
    delay(50);  // delay to allow log to be written
    esp_deep_sleep(sleep_duration_s * 1000ll * 1000ll);
}

// *****************************************************************************

void Iot::restart(bool panic)
{
    if (!panic)
    {
        _panicSleepDuration_s = -1; // regular shutdown, reset panic sleep duration
    }

    _lastSleepDuration_s = 0;
    _activeDuration_ms = millis() - _bootTimestamp_ms;
    log_i("Active for %d ms, restarting", _activeDuration_ms);
    delay(50);  // delay to allow log to be written
    esp_restart();
}

// *****************************************************************************

void Iot::shutdown(bool panic)
{
    if (!panic)
    {
        _panicSleepDuration_s = -1; // regular shutdown, reset panic sleep duration
    }

    _lastSleepDuration_s = 0;
    _activeDuration_ms = millis() - _bootTimestamp_ms;
    log_i("Active for %d ms, shutting down", _activeDuration_ms);
    delay(50);  // delay to allow log to be written
    esp_deep_sleep_start();
}

// *****************************************************************************
