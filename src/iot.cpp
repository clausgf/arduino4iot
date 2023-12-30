/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#include "iot.h"

#include "cstdio"
#include <esp_system.h>
#include <esp_task_wdt.h>
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

RTC_DATA_ATTR static int32_t rtcBootCount = 0;
RTC_DATA_ATTR static int64_t rtcActiveDuration_ms = 0;
RTC_DATA_ATTR static int32_t rtcLastSleepDuration_s = 0;
RTC_DATA_ATTR static int64_t rtcNtpLastSyncTime = 0;
RTC_DATA_ATTR static int32_t rtcPanicSleepDuration_s = -1;

bool Iot::_isWatchdogEnabled = false;

// *****************************************************************************

static void defaultPanicHandler()
{
    iot.escalatingSleepPanicHandler();
}

static void defaultDeepSleepHandler(int duration_s)
{
    esp_deep_sleep(duration_s * 1000ll * 1000ll);
}

static void defaultRestartHandler()
{
    esp_restart();
}

static void defaultShutdownHandler()
{
    esp_deep_sleep_start();
}

// *****************************************************************************

Iot::Iot() :
    _bootCount(&rtcBootCount),
    _activeDuration_ms(&rtcActiveDuration_ms),
    _lastSleepDuration_s(&rtcLastSleepDuration_s),
    _ntpLastSyncTime("iot-var", "ntpLastSync", &rtcNtpLastSyncTime),
    _panicSleepDuration_s("iot-var", "panicSlpDur", &rtcPanicSleepDuration_s),

    _logLevel(config, IotLogger::LogLevel::IOT_LOGLEVEL_NOTSET, "log_level", "logLevel"),
    _sleepDuration_s(config, 5 * 60, "sleep_s", "sleepFor"),
    _watchdogTimeout_s(config, 20, "watchdog_s", "watchdog"),
    _ledPin(config, -1, "led_pin", "ledPin"),
    _ntpResyncInterval_s(config, 24 * 60 * 60, "ntp_resync_s", "ntpResync"),
    _ntpTimeout_ms(config, 10000, "ntp_timeout_ms", "ntpTimeout"),
    _ntpServer1(config, "pool.ntp.org", "ntp_server1", "ntpServer1"),
    _ntpServer2(config, "time.nist.gov", "ntp_server2", "ntpServer2"),
    _ntpServer3(config, "time.google.com", "ntp_server3", "ntpServer3"),
    _batteryOffset_mV(config, 0, "battery_offset_mv", "batOffs"),
    _batteryFactor(config, 2, "battery_factor", "batMul"),
    _batteryDivider(config, 1, "battery_divider", "batDiv"),
    _batteryPin(config, 34, "battery_pin", "batPin"),
    _batteryMin_mV(config, -1, "battery_min_mv", "batMinMv"),
    _panicSleepDurationInit_s(config, 60, "panic_sleep_init_s", "panicSlpInit"),
    _panicSleepDurationFactor(config, 2, "panic_sleep_factor", "panicSlpFac"),
    _panicSleepDurationMax_s(config, 24 * 60 * 60, "panic_sleep_max_s", "panicSlpMax")
{
    // initialize variables
    _deviceId = "";
    _battery_mV = -1;
    _panicHandler = defaultPanicHandler;
    _firmwareVersion = "";
    _firmwareSha256 = "";
    _deepSleepHandler = defaultDeepSleepHandler;
    _restartHandler = defaultRestartHandler;
    _shutdownHandler = defaultShutdownHandler;
    __ntpServer1 = _ntpServer1.get();
    __ntpServer2 = _ntpServer2.get();
    __ntpServer3 = _ntpServer3.get();

    // initialize NVRAM
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK)
    {
        log_e("nvs_flash_init failed: %s", esp_err_to_name(err));
        return;
    }

    esp_reset_reason_t resetReason = getResetReason();
    switch (resetReason)
    {
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
            panicEarly("Last reset was due to exception/panic or watchdog: %s", resetReasonToString(resetReason));
            break;
        case ESP_RST_BROWNOUT:
            panicEarly("Last reset was due to brownout: %s", resetReasonToString(resetReason));
            break;
        default:
            break;
    }
}

void Iot::begin()
{
    setLed(true);  

    // initialize persistent variables
    _bootCount.begin();
    _activeDuration_ms.begin();
    _lastSleepDuration_s.begin();
    _ntpLastSyncTime.begin();
    _panicSleepDuration_s.begin();

    _bootCount = _bootCount.get() + 1;

    if (WiFi.status() != WL_CONNECTED)
    {
        log_e("WiFi not connected yet. Connect WiFi first.");
    }

    // startup logging
    log_w("--- Bootup #%lu, reset reason %s, wakeup cause %s after %d s, panicSleepDuration=%d s",
            getBootCount(), resetReasonToString(getResetReason()), wakeupCauseToString(getWakeupCause()), 
            getLastSleepDuration_s(), getPanicSleepDuration_s());
    log_i("--- Firmware %s", getFirmwareVersion().c_str());
    log_i("--- SHA256 %s", getFirmwareSha256().c_str());

    // read configuration again to allow overwriting hardcoded parameters with WiFi config
    config.begin();
    logger.begin((IotLogger::LogLevel)_logLevel.get());

    // check the battery voltage
    if (_batteryPin.get() >= 0 && _batteryMin_mV.get() > 0)
    {
        int batteryVoltage_mV = getBatteryVoltage_mV();
        if (batteryVoltage_mV < _batteryMin_mV.get())
        {
            log_e("Battery voltage too low: %d mV < %d mV", batteryVoltage_mV, _batteryMin_mV.get());
            shutdown(true);
        }
    }

    // initialize other components
    startWatchdog(_watchdogTimeout_s.get());
    api.setDeviceName(getDeviceId());
    api.begin();
}

bool Iot::begin(const char *ssid, const char *password, unsigned long timeout_ms)
{
    bool success = connectWifi(ssid, password, timeout_ms);
    begin();
    success = success && syncNtpTime();
    return success;
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
        (timeinfo.tm_year + 1900) % 10000, (timeinfo.tm_mon + 1) % 100, timeinfo.tm_mday % 100, 
        timeinfo.tm_hour % 100, timeinfo.tm_min % 100, timeinfo.tm_sec % 100);
    return buf;
}

String Iot::getTimeIso()
{
    time_t now = time(nullptr);
    return getTimeIso(now);
}

// *****************************************************************************

void Iot::setNtp(int resyncInterval_s, int timeout_ms,
        const char *ntpServer1, const char *ntpServer2, const char *ntpServer3)
{
    _ntpResyncInterval_s = resyncInterval_s;
    _ntpTimeout_ms = timeout_ms;
    _ntpServer1 = ntpServer1;
    _ntpServer2 = ntpServer2;
    _ntpServer3 = ntpServer3;
    __ntpServer1 = _ntpServer1.get();
    __ntpServer2 = _ntpServer2.get();
    __ntpServer3 = _ntpServer3.get();
}

// *****************************************************************************

bool Iot::isTimePlausible()
{
    time_t implausibleTimeThreshold = 50 * 365 * 24 * 3600l;
    time_t now = time(nullptr);
    return (now > implausibleTimeThreshold);
}

// bool Iot::waitUntilTimePlausible(unsigned long timeout_ms)
// {
//     log_i("Waiting for NTP time sync");
//     unsigned long startTime = millis();
//     while ( !isTimePlausible() && ((millis() - startTime) < timeout_ms) )
//     {
//         delay(50);
//     }
//     return isTimePlausible();
// }

// *****************************************************************************

bool Iot::waitUntilNtpSync(unsigned long timeout_ms)
{
    if (timeout_ms == 0)
    {
        log_i("waitUntilNtpSync with timeout_ms=0, returning immediately");
        return false;
    }
    log_i("Waiting for NTP time sync");
    unsigned long startTime = millis();
    bool completed = ( esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED );
    while ( !completed && ((millis() - startTime) < timeout_ms) )
    {
        delay(50);
        completed = ( esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED );
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

bool Iot::syncNtpTime()
{
    int64_t sinceLastSync_s = time(nullptr) - _ntpLastSyncTime.get();
    if (isTimePlausible() && sinceLastSync_s >= 0 && sinceLastSync_s < _ntpResyncInterval_s.get())
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
    //esp_sntp_servermode_dhcp(1);
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, __ntpServer1.c_str());
    if (!__ntpServer2.isEmpty())
    {
        esp_sntp_setservername(1, __ntpServer2.c_str());
    }
    if (!__ntpServer3.isEmpty())
    {
        esp_sntp_setservername(2, __ntpServer3.c_str());
    }
    esp_sntp_set_time_sync_notification_cb(_ntpSyncCallback);
    esp_sntp_init();

    // wait for time to be set
    if (!waitUntilNtpSync(_ntpTimeout_ms.get()))
    {
        log_i("NTP time sync failed: %s", getTimeIso().c_str());
        return false;
    }

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
        + ",\"panicSleep_s\":" + getPanicSleepDuration_s()
        + ",\"time\":\"" + getTimeIso() + "\""
        + ",\"firmware_version\":\"" + getFirmwareVersion() + "\""
        + ",\"firmware_sha256\":\"" + getFirmwareSha256() + "\""
        + "}";
    return postTelemetry(kind, jsonData, apiPath);
}


// **********************************************************************
// Led
// **********************************************************************

void Iot::setLedPin(int ledPin)
{
    _ledPin = ledPin;
}

void Iot::setLed(bool value)
{
    if (_ledPin.get() >= 0)
    {
        pinMode(_ledPin.get(), OUTPUT);
        digitalWrite(_ledPin.get(), value ? HIGH : LOW);
    }
}


// *****************************************************************************
// Battery
// *****************************************************************************

void Iot::setBattery(int batteryPin, int batteryFactor, int batteryDivider, int batteryOffset_mV)
{
    _batteryPin = batteryPin;
    _batteryFactor = batteryFactor;
    _batteryDivider = batteryDivider;
    _batteryOffset_mV = batteryOffset_mV;
    _battery_mV = -1; // reset cached value
}

int Iot::getBatteryVoltage_mV()
{
    if ( _batteryPin.get() < 0 )
    {
        log_i("Battery voltage measurement not configured");
        _battery_mV = -1;
    } else if (_battery_mV <= 0)
    {
        uint32_t raw = analogReadMilliVolts(_batteryPin.get());
        int64_t voltage = raw;
        voltage = voltage * _batteryFactor.get();
        voltage = voltage / _batteryDivider.get();
        voltage = voltage + _batteryOffset_mV.get();
        _battery_mV = voltage;
        log_i("Battery voltage: pin=%d, raw=%u battery_voltage=%d mV", 
            _batteryPin.get(), raw, _battery_mV);
    }
    return _battery_mV;
}


// *****************************************************************************
// Error handling / Panic
// *****************************************************************************

void Iot::setPanic(int initialDuration_s, int factor, int maxDuration_s)
{
    _panicSleepDurationInit_s = initialDuration_s;
    _panicSleepDurationFactor = factor;
    _panicSleepDurationMax_s = maxDuration_s;
}

std::function<void()> Iot::setPanicHandler(std::function<void()> panicHandler)
{
    std::function<void()> oldPanicHandler = _panicHandler;
    _panicHandler = panicHandler;
    return oldPanicHandler;
}

// *****************************************************************************

void Iot::panic(const char* format...)
{
    va_list args;
    va_start(args, format);
    logger.logv(IotLogger::LogLevel::IOT_LOGLEVEL_ERROR, tag, format, args);
    va_end(args);

    delay(10);  // delay to allow log to be written
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

    delay(10);  // delay to allow log to be written
    _panicHandler();
}

// *****************************************************************************

void Iot::escalatingSleepPanicHandler()
{
    if (getPanicSleepDuration_s() <= 0)
    {
        // first panic, last run was successful: immediately restart
        _panicSleepDuration_s = _panicSleepDurationInit_s.get();
        deepSleep(getPanicSleepDuration_s(), true);
    } else {
        _panicSleepDuration_s = getPanicSleepDuration_s() * _panicSleepDurationFactor.get();
        if (getPanicSleepDuration_s() > _panicSleepDurationMax_s.get())
        {
            _panicSleepDuration_s = _panicSleepDurationMax_s.get();
        }
        deepSleep(getPanicSleepDuration_s(), true);
    }
}


// *****************************************************************************
// System management: firmware
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
                + " ARDUINO " + ESP_ARDUINO_VERSION_MAJOR + "." + ESP_ARDUINO_VERSION_MINOR + "." + ESP_ARDUINO_VERSION_PATCH
                + " IOT " + IOT_VERSION_MAJOR + "." + IOT_VERSION_MINOR + "." + IOT_VERSION_PATCH;
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
// System management: watchdog
// *****************************************************************************

void Iot::startWatchdog(int watchdogTimeout_s)
{
    esp_err_t err = esp_task_wdt_init(watchdogTimeout_s, true); // panic=true on watchdog timeout
    if (err != ESP_OK)
    {
        panic("*** PANIC *** Error in esp_task_wdt_init: 0x%x", err);
    }
    log_i("Task watchdog timeout=%d s", watchdogTimeout_s);

    err = esp_task_wdt_add(NULL); // NULL for current task
    if (err != ESP_OK) {
        log_e("Error in esp_task_wdt_add: 0x=%x", err);
    }
    log_d("Task watchdog started for current task");
}

void Iot::stopWatchdog()
{
    esp_err_t err = esp_task_wdt_delete(NULL);
    if (err != ESP_OK)
    {
        panic("*** PANIC *** esp_task_wdt_delete=%d", err);
    }
    log_d("Task watchdog stopped for current task");
}

void Iot::resetWatchdog()
{
    esp_err_t err = esp_task_wdt_reset();
    if (err != ESP_OK)
    {
        panic("*** PANIC *** esp_task_wdt_reset=%d", err);
    }
    log_d("Task watchdog reset");
}


// *****************************************************************************
// System management: sleep, restart, shutdown
// *****************************************************************************

void Iot::setSleepDuration_s(int sleep_duration_s)
{
    _sleepDuration_s = sleep_duration_s;
}

void Iot::setDeepSleepHandler(std::function<void(int duration_s)> deepSleepHandler)
{
    _deepSleepHandler = deepSleepHandler;
}

void Iot::setRestartHandler(std::function<void()> restartHandler)
{
    _restartHandler = restartHandler;
}

void Iot::setShutdownHandler(std::function<void()> shutdownHandler)
{
    _shutdownHandler = shutdownHandler;
}

// *****************************************************************************

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
    _activeDuration_ms = millis();
    log_w("Active for %lld ms, going to deep sleep for %d s", getActiveDuration_ms(), sleep_duration_s);
    delay(10);  // delay to allow log to be written
    setLed(false);
    _deepSleepHandler(sleep_duration_s);
}

// *****************************************************************************

void Iot::restart(bool panic)
{
    if (!panic)
    {
        _panicSleepDuration_s = -1; // regular shutdown, reset panic sleep duration
    }

    _lastSleepDuration_s = 0;
    _activeDuration_ms = millis();
    log_w("Active for %lld ms, restarting", getActiveDuration_ms());
    delay(10);  // delay to allow log to be written
    setLed(false);
    _restartHandler();
}

// *****************************************************************************

void Iot::shutdown(bool panic)
{
    if (!panic)
    {
        _panicSleepDuration_s = -1; // regular shutdown, reset panic sleep duration
    }

    _lastSleepDuration_s = 0;
    _activeDuration_ms = millis();
    log_w("Active for %lld ms, shutting down", getActiveDuration_ms());
    delay(10);  // delay to allow log to be written
    setLed(false);
    _shutdownHandler();
}

// *****************************************************************************
