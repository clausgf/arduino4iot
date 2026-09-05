/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#include "iot.h"

#include "cstdio"
#include <esp_task_wdt.h>
#include <esp_wifi.h>
#include <esp_ota_ops.h>
#include <nvs_flash.h>
#include <esp_sntp.h>
#include <esp_idf_version.h>
#include <sys/time.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <ArduinoJson.h>

#include "iot_ap.h"
#include "iot_esp_ports.h"

// *****************************************************************************

Iot iot;
static const char *tag = "iot";

bool Iot::_isWatchdogEnabled = false;

// *****************************************************************************

/**
 * The default power adapter.
 *
 * A function-local static rather than a namespace-scope object: it is
 * constructed on first use, so Iot's own constructor - which runs before
 * main() and in an unspecified order relative to other translation units - can
 * safely bind to it. Replaceable via Iot::setPower().
 */
static IotPower& defaultPower()
{
    static IotEspPower power;
    return power;
}

static void defaultPanicHandler()
{
    iot.escalatingSleepPanicHandler();
}

// *****************************************************************************

Iot::Iot() :
    _lifecycle(defaultPower(), iotRtcState()),

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
    _fastReconnect = true;
    _staticIpConfigured = false;
    _dhcpCache = false;
    _dhcpMaxReuse = 20;
    _wifiConnectDuration_ms = 0;
    _panicHandler = defaultPanicHandler;
    _firmwareVersion = "";
    _firmwareId = "";
    _firmwareSha256 = "";
    _exitHooksRegistered = false;
    __ntpServer1 = _ntpServer1.get();
    __ntpServer2 = _ntpServer2.get();
    __ntpServer3 = _ntpServer3.get();

    // Nothing else happens here on purpose. Up to 3.x this constructor
    // initialized NVS and, after an abnormal reset, went straight into the
    // panic handler - before main(), before the sketch could open a serial
    // console to see the diagnosis, while the persistent values it read were
    // still uninitialized, and while the logger and api singletons of other
    // translation units may not have been constructed yet. All of that now
    // happens in begin().
}

void Iot::_registerExitHooks()
{
    if (_exitHooksRegistered)
    {
        return;
    }
    _exitHooksRegistered = true;

    // runs before every deep sleep, restart, shutdown and backoff sleep.
    // logger.flush() is a no-op without connectivity, so this is also safe on
    // the early failure paths where no network was ever established.
    _lifecycle.addExitHook([this] {
        logger.flush();
        api.closeConnection();
        delay(10);  // give the serial log a chance to drain
        setLed(false);
    });
}

void Iot::_beginCycle()
{
    // NVS first: everything below reads Preferences
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK)
    {
        log_e("nvs_flash_init failed: %s", esp_err_to_name(err));
    }

    _registerExitHooks();
    _lifecycle.setBackoffPolicy(IotBackoffPolicy{
        _panicSleepDurationInit_s.get(),
        _panicSleepDurationFactor.get(),
        _panicSleepDurationMax_s.get() });
    _lifecycle.begin();

    // the watchdog must already run before apProvisioning.run() is reached,
    // since that call can block far longer than _watchdogTimeout_s
    startWatchdog(_watchdogTimeout_s.get());

    log_w("--- Bootup #%lu, reset reason %s, wakeup cause %s after %d s, backoff=%d s",
        (unsigned long)getBootCount(),
        iotResetReasonToString(_lifecycle.resetReason()),
        iotWakeupCauseToString(_lifecycle.wakeupCause()),
        getLastSleepDuration_s(), getPanicSleepDuration_s());
    log_i("--- Firmware version %s", getFirmwareVersion().c_str());
    log_i("--- Firmware id %s", getFirmwareId().c_str());
    log_i("--- SHA256 %s", getFirmwareSha256().c_str());

    // The previous cycle crashed, hung or browned out. Back off instead of
    // retrying immediately - and do it here rather than in the constructor, so
    // the message above has been printed first.
    if (_lifecycle.isAbnormalStart())
    {
        panicEarly("*** PANIC *** Previous cycle ended abnormally: %s",
            iotResetReasonToString(_lifecycle.resetReason()));
    }
}

void Iot::_initSubsystems()
{
    setLed(true);

    if (WiFi.status() != WL_CONNECTED)
    {
        log_e("WiFi not connected yet. Connect WiFi first.");
    }

    // read configuration again to allow overwriting hardcoded parameters with WiFi config
    config.begin();
    logger.begin((IotLogger::LogLevel)_logLevel.get());

    // the backoff parameters may have been changed by the server config
    _lifecycle.setBackoffPolicy(IotBackoffPolicy{
        _panicSleepDurationInit_s.get(),
        _panicSleepDurationFactor.get(),
        _panicSleepDurationMax_s.get() });

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

bool Iot::begin(unsigned long timeout_ms)
{
    _beginCycle();

    // WiFi credentials were seeded into NVS by seedCredentials()
    Preferences preferences;
    preferences.begin("iot", true);
    String ssid = nvramGetString(preferences, "wifiSsid", "");
    String password = nvramGetString(preferences, "wifiPass", "");
    preferences.end();

    if (ssid.isEmpty())
    {
        log_w("begin: no WiFi SSID seeded, entering AP provisioning mode");
        apProvisioning.run(); // never returns
    }

    bool success = connectWifi(ssid.c_str(), password.c_str(), timeout_ms);
    _initSubsystems();
    success = success && syncNtpTime();
    return success;
}

void Iot::seedCredentials(const IotSeedConfig& cfg)
{
    Preferences preferences;
    if (!preferences.begin("iot", false))
    {
        log_e("seedCredentials: cannot open NVS");
        return;
    }
    uint32_t storedGeneration = nvramGetUInt(preferences, "seedGen", 0);
    bool force = cfg.seedGeneration > storedGeneration;

    iotSeedString(preferences, "wifiSsid", cfg.wifiSsid, force);
    iotSeedString(preferences, "wifiPass", cfg.wifiPassword, force);
    api._seedFromConfig(preferences, cfg, force);

    if (force)
    {
        preferences.putUInt("seedGen", cfg.seedGeneration);
        log_w("seedCredentials: applied seed generation %u", (unsigned)cfg.seedGeneration);
    }
    preferences.end();
}

static void clearNvsNamespace(const char* ns)
{
    Preferences preferences;
    if (preferences.begin(ns, false))
    {
        preferences.clear();
        preferences.end();
    }
}

void Iot::factoryReset()
{
    log_w("factoryReset: erasing all persisted NVS state");
    // "iot-var" is gone in 4.0: the values it held (last NTP sync, backoff
    // state) live in the RTC state now, which no longer costs flash writes
    for (const char* ns : { "iot", "iot-cfg" })
    {
        clearNvsNamespace(ns);
    }
}

void Iot::clearProvisioning()
{
    log_w("clearProvisioning: erasing seed/provisioning state ('iot' namespace only)");
    clearNvsNamespace("iot");
}

void Iot::end()
{
    logger.flush();
    api.closeConnection();
    api.end();
    logger.end();
    config.end();
}

// *****************************************************************************

void Iot::setStaticIp(IPAddress ip, IPAddress gateway, IPAddress subnet, IPAddress dns1, IPAddress dns2)
{
    _ip = ip;
    _gw = gateway;
    _mask = subnet;
    _dns1 = dns1;
    _dns2 = dns2;
    _staticIpConfigured = true;
}

void Iot::setFastReconnect(bool enabled)
{
    _fastReconnect = enabled;
}

void Iot::setDhcpCache(bool enabled, uint32_t maxReuse)
{
    _dhcpCache = enabled;
    if (maxReuse > 0)
    {
        _dhcpMaxReuse = maxReuse;
    }
    if (!enabled)
    {
        _invalidateDhcpCache();
    }
}

void Iot::_invalidateDhcpCache()
{
    iotRtcState().dhcpIp = 0;
    iotRtcState().dhcpReuseCount = 0;
}

// wait until connected or the deadline (absolute millis()) is reached
static bool _waitForWifi(unsigned long deadline)
{
    while ( (WiFi.status() != WL_CONNECTED) && ((long)(millis() - deadline) < 0) )
    {
        delay(50);
    }
    return WiFi.status() == WL_CONNECTED;
}

bool Iot::connectWifi(const char *ssid, const char *password, unsigned long timeout_ms)
{
    // immediately return if already connected
    if (WiFi.status() == WL_CONNECTED)
    {
        log_i("WiFi already connected ip=%s", WiFi.localIP().toString().c_str());
        return true;
    }

    log_i("Connecting to WiFi network ssid=%s timeout=%lu ms", ssid, timeout_ms);
    unsigned long startTime = millis();
    unsigned long deadline = startTime + timeout_ms;

    // avoid NVS wear: do not let the WiFi stack persist credentials on connect
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    if (_staticIpConfigured)
    {
        // skip DHCP
        if (!WiFi.config(_ip, _gw, _mask, _dns1, _dns2))
        {
            log_e("WiFi static IP config failed, falling back to DHCP");
        }
    }

    // 1. targeted reconnect to the cached AP (skips the all-channel scan)
    bool triedFast = false;
    bool usedCachedLease = false;
    if (_fastReconnect && iotRtcState().wifiChannel != 0)
    {
        triedFast = true;
        // Reuse the cached DHCP lease only on this fast-reconnect path: it targets
        // the same cached AP, so the previously assigned address is still valid on
        // the same subnet. Bounded by _dhcpMaxReuse to force periodic real DHCP.
        if (!_staticIpConfigured && _dhcpCache && iotRtcState().dhcpIp != 0
            && iotRtcState().dhcpReuseCount < (int32_t)_dhcpMaxReuse)
        {
            if (WiFi.config(IPAddress(iotRtcState().dhcpIp), IPAddress(iotRtcState().dhcpGw), IPAddress(iotRtcState().dhcpMask),
                            IPAddress(iotRtcState().dhcpDns1), IPAddress(iotRtcState().dhcpDns2)))
            {
                usedCachedLease = true;
                log_i("Fast reconnect: reusing cached DHCP lease ip=%s (reuse %ld/%lu)",
                    IPAddress(iotRtcState().dhcpIp).toString().c_str(),
                    (long)iotRtcState().dhcpReuseCount, (unsigned long)_dhcpMaxReuse);
            }
        }
        log_i("Fast reconnect: channel=%ld bssid=%02x:%02x:%02x:%02x:%02x:%02x",
            (long)iotRtcState().wifiChannel, iotRtcState().wifiBssid[0], iotRtcState().wifiBssid[1], iotRtcState().wifiBssid[2],
            iotRtcState().wifiBssid[3], iotRtcState().wifiBssid[4], iotRtcState().wifiBssid[5]);
        WiFi.begin(ssid, password, iotRtcState().wifiChannel, iotRtcState().wifiBssid);
        // give the fast attempt at most half of the budget before falling back
        _waitForWifi(startTime + timeout_ms / 2);
    }

    // 2. fallback: plain full-scan connect with the remaining time budget
    if (WiFi.status() != WL_CONNECTED)
    {
        if (triedFast)
        {
            log_w("Fast reconnect failed, retrying with a full scan");
            iotRtcState().wifiChannel = 0; // invalidate the stale cache
        }
        if (usedCachedLease)
        {
            // a stale lease may be the cause; revert to DHCP and drop the cache
            log_w("Cached DHCP lease may be stale, reverting to DHCP");
            WiFi.config(IPAddress((uint32_t)0), IPAddress((uint32_t)0), IPAddress((uint32_t)0));
            _invalidateDhcpCache();
            usedCachedLease = false;
        }
        WiFi.begin(ssid, password);
        _waitForWifi(deadline);
    }

    // check for failed connection due to timeout
    if (WiFi.status() != WL_CONNECTED)
    {
        log_e("WiFi connection failed");
        return false;
    }

    // refresh the cache for the next wakeup
    iotRtcState().wifiChannel = WiFi.channel();
    memcpy(iotRtcState().wifiBssid, WiFi.BSSID(), 6);

    // refresh the DHCP lease cache (only meaningful when DHCP is in use)
    if (!_staticIpConfigured && _dhcpCache)
    {
        if (usedCachedLease)
        {
            iotRtcState().dhcpReuseCount++;
        }
        else
        {
            // a real DHCP bind happened: capture the fresh lease and reset the
            // reuse counter so the next wakeups can skip DORA
            iotRtcState().dhcpIp   = (uint32_t)WiFi.localIP();
            iotRtcState().dhcpGw   = (uint32_t)WiFi.gatewayIP();
            iotRtcState().dhcpMask = (uint32_t)WiFi.subnetMask();
            iotRtcState().dhcpDns1 = (uint32_t)WiFi.dnsIP(0);
            iotRtcState().dhcpDns2 = (uint32_t)WiFi.dnsIP(1);
            iotRtcState().dhcpReuseCount = 0;
        }
    }

    _wifiConnectDuration_ms = millis() - startTime;

    log_i("WiFi connected ip=%s after %lu ms", WiFi.localIP().toString().c_str(), _wifiConnectDuration_ms);
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

        // The device id doubles as the device name for the API. nice4iot
        // requires device names to match ^[a-zA-Z_][a-zA-Z0-9_]*$ (no hyphen),
        // so use an underscore separator, e.g. "e32_aabbccddeeff".
        // local (not static) buffer: it is copied into _deviceId immediately,
        // and esp_netif_set_hostname() copies the string too, so no shared
        // state is left to race between tasks
        const int ID_MAXLEN = 17;
        char id_buf[ID_MAXLEN];
        snprintf(id_buf, ID_MAXLEN, "e32_%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

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

    // local (not static) buffer: it is copied into the returned String right
    // away, so there is no shared state to race between concurrent callers
    const int BUFLEN = 32;
    char buf[BUFLEN];
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
    iotRtcState().ntpLastSyncTime = (int64_t)now;
}

// *****************************************************************************

bool Iot::_syncNtpOneShot(unsigned long timeout_ms)
{
    // A single SNTP request/response, bypassing the lwIP SNTP daemon whose
    // randomized 0-5 s startup delay (CONFIG_LWIP_SNTP_STARTUP_DELAY) dominates
    // the sync time. Tries each configured server until one answers.
    const uint32_t NTP_UNIX_OFFSET = 2208988800UL; // seconds between 1900 and 1970
    const uint16_t NTP_PORT = 123;
    const time_t plausibleThreshold = 50 * 365 * 24 * 3600L;

    WiFiUDP udp;
    if (!udp.begin(2390))
    {
        log_i("NTP one-shot: UDP begin failed");
        return false;
    }

    const char* servers[3] = {
        __ntpServer1.c_str(),
        __ntpServer2.isEmpty() ? nullptr : __ntpServer2.c_str(),
        __ntpServer3.isEmpty() ? nullptr : __ntpServer3.c_str(),
    };

    bool ok = false;
    uint8_t packet[48];
    for (const char* server : servers)
    {
        if (server == nullptr || server[0] == '\0')
        {
            continue;
        }

        // build the client request (LI=0, VN=3, Mode=3)
        memset(packet, 0, sizeof(packet));
        packet[0] = 0x1B;

        if (!udp.beginPacket(server, NTP_PORT))
        {
            log_i("NTP one-shot: cannot resolve/reach %s", server);
            continue;
        }
        udp.write(packet, sizeof(packet));
        if (!udp.endPacket())
        {
            continue;
        }

        // wait for the reply
        unsigned long start = millis();
        int len = 0;
        while ((millis() - start) < timeout_ms)
        {
            len = udp.parsePacket();
            if (len >= (int)sizeof(packet))
            {
                break;
            }
            delay(10);
        }
        if (len < (int)sizeof(packet))
        {
            continue;
        }
        udp.read(packet, sizeof(packet));

        // transmit timestamp (seconds since 1900) is at bytes 40..43, big-endian
        uint32_t ntpSecs = ((uint32_t)packet[40] << 24) | ((uint32_t)packet[41] << 16)
                         | ((uint32_t)packet[42] << 8)  | (uint32_t)packet[43];
        if (ntpSecs <= NTP_UNIX_OFFSET)
        {
            continue;
        }
        time_t unixSecs = (time_t)(ntpSecs - NTP_UNIX_OFFSET);
        if (unixSecs < plausibleThreshold)
        {
            continue; // reject implausible responses
        }

        // fractional part (bytes 44..47) for sub-second accuracy
        uint32_t frac = ((uint32_t)packet[44] << 24) | ((uint32_t)packet[45] << 16)
                      | ((uint32_t)packet[46] << 8)  | (uint32_t)packet[47];
        suseconds_t usec = (suseconds_t)(((uint64_t)frac * 1000000ULL) >> 32);

        struct timeval tv;
        tv.tv_sec = unixSecs;
        tv.tv_usec = usec;
        settimeofday(&tv, nullptr);
        iotRtcState().ntpLastSyncTime = (int64_t)unixSecs;
        log_i("NTP one-shot sync success (server=%s): %s", server, getTimeIso(unixSecs).c_str());
        ok = true;
        break;
    }

    udp.stop();
    return ok;
}

bool Iot::syncNtpTime()
{
    int64_t sinceLastSync_s = time(nullptr) - iotRtcState().ntpLastSyncTime;
    if (isTimePlausible() && sinceLastSync_s >= 0 && sinceLastSync_s < _ntpResyncInterval_s.get())
    {
        log_i("NTP time should be good enough: time=%s", getTimeIso().c_str());
        return true;
    }

    // fast path: a single manual SNTP query avoids the lwIP daemon startup delay
    if (_syncNtpOneShot(_ntpTimeout_ms.get()))
    {
        return true;
    }
    log_i("NTP one-shot failed, falling back to the SNTP daemon");

    // fallback: the lwIP SNTP daemon
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

IotResult Iot::postTelemetry(const String& kind, const String& jsonData, const String& apiPath)
{
    if (jsonData.length() > IOT_MAX_TELEMETRY_SIZE)
    {
        log_w("Telemetry body size %u exceeds the server's default limit of %u bytes",
            jsonData.length(), IOT_MAX_TELEMETRY_SIZE);
    }
    String path = apiPath;
    path.replace("{kind}", kind);
    // other variables are replaced in apiPost()
    String oResult = "";
    return api.apiPost(oResult, path, jsonData);
}

// *****************************************************************************

IotResult Iot::postTelemetry(const String& kind, const IotTelemetry& telemetry, const String& apiPath)
{
    String jsonData;
    telemetry.serializeTo(jsonData);
    return postTelemetry(kind, jsonData, apiPath);
}

// *****************************************************************************

IotResult Iot::postSystemTelemetry(const String& kind, const String& apiPath)
{
    IotTelemetry telemetry;
    if (_batteryPin.get() >= 0)
    {
        telemetry.add("battery_V", getBatteryVoltage_mV() / 1000.0);
    }
    telemetry.add("wifi_rssi", WiFi.RSSI());
    telemetry.add("wifi_connect_ms", (int32_t)getWifiConnectDuration_ms());
    telemetry.add("boot_count", getBootCount());
    telemetry.add("active_ms", getActiveDuration_ms());
    telemetry.add("lastSleep_s", getLastSleepDuration_s());
    telemetry.add("panicSleep_s", getPanicSleepDuration_s());
    // non-numeric fields are ignored by the nice4iot telemetry backend,
    // but kept for backends which support them
    telemetry.add("time", getTimeIso());
    telemetry.add("firmware_id", getFirmwareId());
    String firmwareVersion = getFirmwareVersion();
    if (!firmwareVersion.isEmpty())
    {
        telemetry.add("firmware_version", firmwareVersion);
    }
    telemetry.add("firmware_sha256", getFirmwareSha256());
    String boardId = getBoardId();
    if (!boardId.isEmpty())
    {
        telemetry.add("board_id", boardId);
    }
    return postTelemetry(kind, telemetry, apiPath);
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

    // flush buffered logs (including this error) before the panic handler,
    // which typically sends the device to sleep
    logger.flush();

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
    // the escalation itself is IotBackoffPolicy (iot_lifecycle.h): plain,
    // unit-tested logic rather than arithmetic scattered over this method
    log_w("Entering failure backoff (current %d s)", _lifecycle.backoffDuration_s());
    _lifecycle.enterBackoff();  // runs the exit hooks and does not return
}


// *****************************************************************************
// System management: firmware
// *****************************************************************************

String Iot::getFirmwareVersion()
{
    // precedence: explicit setter (_firmwareVersion) > injected build define
    if (_firmwareVersion.isEmpty())
    {
#ifdef IOT_FW_VERSION
        _firmwareVersion = IOT_FW_VERSION;
#endif
    }
    return _firmwareVersion;
}

void Iot::setFirmwareVersion(const String& version)
{
    _firmwareVersion = version;
}

String Iot::getFirmwareId()
{
    if (_firmwareId.isEmpty())
    {
        const esp_partition_t *running = esp_ota_get_running_partition();
        esp_app_desc_t app_info;
        if (esp_ota_get_partition_description(running, &app_info) == ESP_OK)
        {
            _firmwareId = String(app_info.project_name)
                + " " + String(app_info.version)
                + " " + String(app_info.date)
                + " " + String(app_info.time)
                + " IDF " + String(app_info.idf_ver)
                + " sec " + String(app_info.secure_version)
                + " ARDUINO " + ESP_ARDUINO_VERSION_MAJOR + "." + ESP_ARDUINO_VERSION_MINOR + "." + ESP_ARDUINO_VERSION_PATCH
                + " IOT " + IOT_VERSION_MAJOR + "." + IOT_VERSION_MINOR + "." + IOT_VERSION_PATCH;
        }
    }
    return _firmwareId;
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

String Iot::getBoardId()
{
#ifdef IOT_BOARD_ID
    return String(IOT_BOARD_ID);
#else
    return String("");
#endif
}


// *****************************************************************************
// System management: watchdog
// *****************************************************************************

void Iot::startWatchdog(int watchdogTimeout_s)
{
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    esp_task_wdt_config_t wdtConfig = {
        .timeout_ms = (uint32_t)watchdogTimeout_s * 1000u,
        .idle_core_mask = 0,
        .trigger_panic = true, // panic on watchdog timeout
    };
    esp_err_t err = esp_task_wdt_init(&wdtConfig);
    if (err == ESP_ERR_INVALID_STATE)
    {
        // watchdog already initialized by the Arduino core
        err = esp_task_wdt_reconfigure(&wdtConfig);
    }
#else
    esp_err_t err = esp_task_wdt_init(watchdogTimeout_s, true); // panic=true on watchdog timeout
#endif
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

void Iot::setPower(IotPower& power)
{
    _lifecycle.setPower(power);
}

void Iot::addExitHook(std::function<void()> hook)
{
    // make sure the library's own hook (log flush, connection close) is first
    _registerExitHooks();
    _lifecycle.addExitHook(std::move(hook));
}

// *****************************************************************************

void Iot::deepSleep()
{
    deepSleep(_sleepDuration_s);
}

// The three exits below are thin: IotLifecycle records the cycle in the RTC
// state, runs the exit hooks (log flush, connection close, LED off) and then
// dispatches to the IotPower adapter. `panic` keeps its 3.x meaning - true
// leaves the escalating backoff state untouched, false clears it because the
// cycle ended in an orderly way.

void Iot::deepSleep(int sleep_duration_s, bool panic)
{
    _registerExitHooks();
    log_w("Active for %lu ms, going to deep sleep for %d s",
        (unsigned long)millis(), sleep_duration_s);
    _lifecycle.deepSleep(sleep_duration_s > 0 ? (uint32_t)sleep_duration_s : 0, panic);
}

// *****************************************************************************

void Iot::restart(bool panic)
{
    _registerExitHooks();
    log_w("Active for %lu ms, restarting", (unsigned long)millis());
    _lifecycle.restart(panic);
}

// *****************************************************************************

void Iot::shutdown(bool panic)
{
    _registerExitHooks();
    log_w("Active for %lu ms, shutting down", (unsigned long)millis());
    _lifecycle.shutdown(panic);
}

// *****************************************************************************
