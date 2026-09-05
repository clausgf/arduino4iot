/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#include "iot_esp_ports.h"

#include <cstring>

#include <esp_attr.h>
#include <esp_mac.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <sys/time.h>
#include <WiFi.h>
#include <WiFiUdp.h>

// *****************************************************************************
// RTC state
// *****************************************************************************

// RTC_NOINIT_ATTR rather than RTC_DATA_ATTR: the .rtc.data section is
// reinitialized from the image by the bootloader on a software or watchdog
// reset, which would silently reset the failure backoff exactly when it is
// needed. .rtc.noinit is left alone and only loses its contents on a power-on
// reset - which IotRtcState::isValid() detects via the magic.
static RTC_NOINIT_ATTR IotRtcState rtcStateStorage;

IotRtcState& iotRtcState()
{
    if (!rtcStateStorage.isValid())
    {
        rtcStateStorage.reset();
    }
    return rtcStateStorage;
}


// *****************************************************************************
// IotNvsStore
// *****************************************************************************

IotNvsStore::IotNvsStore(const char* nvsNamespace):
    _nvsNamespace(nvsNamespace),
    _isOpen(false)
{
}

IotNvsStore::~IotNvsStore()
{
    end();
}

bool IotNvsStore::begin()
{
    if (_isOpen)
    {
        return true;
    }
    _isOpen = _preferences.begin(_nvsNamespace, false);
    if (!_isOpen)
    {
        log_e("IotNvsStore: cannot open NVS namespace '%s'", _nvsNamespace);
    }
    return _isOpen;
}

void IotNvsStore::end()
{
    if (_isOpen)
    {
        _preferences.end();
        _isOpen = false;
    }
}

bool IotNvsStore::_usable(const char* key) const
{
    if (!_isOpen)
    {
        log_e("IotNvsStore('%s'): not open - call begin() first", _nvsNamespace);
        return false;
    }
    if (!IotStore::isValidKey(key))
    {
        log_e("IotNvsStore('%s'): invalid key (max %u characters)",
            _nvsNamespace, (unsigned)IotStore::MAX_KEY_LENGTH);
        return false;
    }
    return true;
}

bool IotNvsStore::has(const char* key) const
{
    return _usable(key) && _preferences.isKey(key);
}

std::string IotNvsStore::getString(const char* key, const std::string& defaultValue) const
{
    // guarded by isKey(): Preferences logs an unfilterable error for a missing key
    if (!has(key) || _preferences.getType(key) != PT_STR)
    {
        return defaultValue;
    }
    return std::string(_preferences.getString(key, String(defaultValue.c_str())).c_str());
}

bool IotNvsStore::_readInt(const char* key, int64_t& oValue) const
{
    if (!has(key))
    {
        return false;
    }
    // accept every integer width NVS reports, so values written by an older
    // firmware or by an externally generated partition image stay readable
    switch (_preferences.getType(key))
    {
        case PT_I8:  oValue = _preferences.getChar(key);    return true;
        case PT_U8:  oValue = _preferences.getUChar(key);   return true;
        case PT_I16: oValue = _preferences.getShort(key);   return true;
        case PT_U16: oValue = _preferences.getUShort(key);  return true;
        case PT_I32: oValue = _preferences.getInt(key);     return true;
        case PT_U32: oValue = _preferences.getUInt(key);    return true;
        case PT_I64: oValue = _preferences.getLong64(key);  return true;
        case PT_U64: oValue = (int64_t)_preferences.getULong64(key); return true;
        default:     return false;
    }
}

int64_t IotNvsStore::getInt(const char* key, int64_t defaultValue) const
{
    int64_t value = 0;
    return _readInt(key, value) ? value : defaultValue;
}

bool IotNvsStore::getBool(const char* key, bool defaultValue) const
{
    int64_t value = 0;
    return _readInt(key, value) ? (value != 0) : defaultValue;
}

void IotNvsStore::putString(const char* key, const std::string& value)
{
    if (!_usable(key))
    {
        return;
    }
    if (has(key) && _preferences.getType(key) == PT_STR
        && getString(key, value + "\x01") == value)
    {
        return;  // unchanged: do not spend a flash write cycle
    }
    if (_preferences.putString(key, value.c_str()) == 0 && !value.empty())
    {
        // the key exists with a different NVS type - drop it and normalize
        _preferences.remove(key);
        _preferences.putString(key, value.c_str());
    }
}

void IotNvsStore::putInt(const char* key, int64_t value)
{
    if (!_usable(key))
    {
        return;
    }
    int64_t stored = 0;
    if (_readInt(key, stored) && stored == value && _preferences.getType(key) == PT_I64)
    {
        return;  // unchanged and already normalized
    }
    if (_preferences.putLong64(key, value) == 0)
    {
        _preferences.remove(key);
        _preferences.putLong64(key, value);
    }
}

void IotNvsStore::putBool(const char* key, bool value)
{
    if (!_usable(key))
    {
        return;
    }
    int64_t stored = 0;
    if (_readInt(key, stored) && (stored != 0) == value && _preferences.getType(key) == PT_U8)
    {
        return;
    }
    if (_preferences.putBool(key, value) == 0)
    {
        _preferences.remove(key);
        _preferences.putBool(key, value);
    }
}

void IotNvsStore::remove(const char* key)
{
    if (has(key))
    {
        _preferences.remove(key);
    }
}

void IotNvsStore::clear()
{
    if (_isOpen)
    {
        _preferences.clear();
    }
}


// *****************************************************************************
// IotSystemClock
// *****************************************************************************

int64_t IotSystemClock::now() const
{
    return (int64_t)time(nullptr);
}

bool IotSystemClock::isPlausible() const
{
    return now() > IotClock::PLAUSIBLE_THRESHOLD;
}


// *****************************************************************************
// IotNtpClock
// *****************************************************************************

IotNtpClock::IotNtpClock(int64_t& lastSyncTime):
    _lastSyncTime(lastSyncTime),
    _resyncInterval_s(24 * 60 * 60)
{
    _server[0] = "pool.ntp.org";
    _server[1] = "time.nist.gov";
    _server[2] = "time.google.com";
}

void IotNtpClock::setServers(const char* server1, const char* server2, const char* server3)
{
    _server[0] = server1 != nullptr ? server1 : "";
    _server[1] = server2 != nullptr ? server2 : "";
    _server[2] = server3 != nullptr ? server3 : "";
}

int64_t IotNtpClock::now() const
{
    return (int64_t)time(nullptr);
}

bool IotNtpClock::isPlausible() const
{
    return now() > IotClock::PLAUSIBLE_THRESHOLD;
}

bool IotNtpClock::sync(uint32_t timeout_ms)
{
    int64_t sinceLastSync_s = now() - _lastSyncTime;
    if (isPlausible() && sinceLastSync_s >= 0 && sinceLastSync_s < _resyncInterval_s)
    {
        log_i("IotNtpClock: no resync due (last sync %lld s ago)", (long long)sinceLastSync_s);
        return true;
    }
    if (_syncOneShot(timeout_ms))
    {
        return true;
    }
    log_w("IotNtpClock: sync failed, time is %splausible", isPlausible() ? "" : "not ");
    return isPlausible();
}

bool IotNtpClock::_syncOneShot(uint32_t timeout_ms)
{
    // A single SNTP request/response. The lwIP SNTP daemon is deliberately not
    // used: its randomized 0-5 s startup delay dominates the sync time, and it
    // has nothing to offer when this fails (that means no connectivity).
    const uint32_t NTP_UNIX_OFFSET = 2208988800UL;  // seconds between 1900 and 1970
    const uint16_t NTP_PORT = 123;
    const size_t PACKET_SIZE = 48;

    WiFiUDP udp;
    if (!udp.begin(2390))
    {
        log_i("IotNtpClock: UDP begin failed");
        return false;
    }

    bool ok = false;
    uint8_t packet[PACKET_SIZE];
    for (const String& server : _server)
    {
        if (server.isEmpty())
        {
            continue;
        }

        // client request: LI = 0, VN = 3, Mode = 3
        memset(packet, 0, sizeof(packet));
        packet[0] = 0x1b;

        if (!udp.beginPacket(server.c_str(), NTP_PORT))
        {
            log_i("IotNtpClock: cannot resolve/reach %s", server.c_str());
            continue;
        }
        udp.write(packet, sizeof(packet));
        if (!udp.endPacket())
        {
            continue;
        }

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

        // transmit timestamp: seconds since 1900 in bytes 40..43, big endian
        uint32_t ntpSeconds = ((uint32_t)packet[40] << 24) | ((uint32_t)packet[41] << 16)
                            | ((uint32_t)packet[42] << 8)  | (uint32_t)packet[43];
        if (ntpSeconds <= NTP_UNIX_OFFSET)
        {
            continue;
        }
        int64_t unixSeconds = (int64_t)(ntpSeconds - NTP_UNIX_OFFSET);
        if (unixSeconds < IotClock::PLAUSIBLE_THRESHOLD)
        {
            continue;  // reject an implausible response
        }

        // fractional part (bytes 44..47) for sub-second accuracy
        uint32_t fraction = ((uint32_t)packet[44] << 24) | ((uint32_t)packet[45] << 16)
                          | ((uint32_t)packet[46] << 8)  | (uint32_t)packet[47];

        struct timeval tv;
        tv.tv_sec = (time_t)unixSeconds;
        tv.tv_usec = (suseconds_t)(((uint64_t)fraction * 1000000ULL) >> 32);
        settimeofday(&tv, nullptr);
        _lastSyncTime = unixSeconds;
        log_i("IotNtpClock: synced from %s", server.c_str());
        ok = true;
        break;
    }

    udp.stop();
    return ok;
}


// *****************************************************************************
// IotWifiNetwork
// *****************************************************************************

IotWifiNetwork::IotWifiNetwork(IotRtcState& rtcState):
    _rtc(rtcState),
    _fastReconnect(true),
    _staticIpConfigured(false),
    _dhcpCache(false),
    _dhcpMaxReuse(20),
    _connectDuration_ms(0)
{
}

void IotWifiNetwork::setCredentials(const std::string& ssid, const std::string& password)
{
    _ssid = ssid.c_str();
    _password = password.c_str();
}

void IotWifiNetwork::setHostname(const std::string& hostname)
{
    _hostname = hostname.c_str();
}

void IotWifiNetwork::setStaticIp(IPAddress ip, IPAddress gateway, IPAddress subnet,
    IPAddress dns1, IPAddress dns2)
{
    _ip = ip;
    _gw = gateway;
    _mask = subnet;
    _dns1 = dns1;
    _dns2 = dns2;
    _staticIpConfigured = true;
}

void IotWifiNetwork::setDhcpCache(bool enabled, uint32_t maxReuse)
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

void IotWifiNetwork::_invalidateDhcpCache()
{
    _rtc.dhcpIp = 0;
    _rtc.dhcpReuseCount = 0;
}

bool IotWifiNetwork::_waitForConnection(unsigned long deadline_ms) const
{
    while ((WiFi.status() != WL_CONNECTED) && ((long)(millis() - deadline_ms) < 0))
    {
        delay(50);
    }
    return WiFi.status() == WL_CONNECTED;
}

bool IotWifiNetwork::connect(uint32_t timeout_ms)
{
    if (isConnected())
    {
        log_i("IotWifiNetwork: already connected ip=%s", localIp().c_str());
        return true;
    }
    if (_ssid.isEmpty())
    {
        log_e("IotWifiNetwork: no SSID configured");
        return false;
    }

    log_i("IotWifiNetwork: connecting ssid=%s timeout=%u ms", _ssid.c_str(), (unsigned)timeout_ms);
    unsigned long startTime = millis();
    unsigned long deadline = startTime + timeout_ms;

    // do not let the WiFi stack persist credentials on every connect (flash wear)
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    if (!_hostname.isEmpty())
    {
        WiFi.setHostname(_hostname.c_str());
    }
    if (_staticIpConfigured && !WiFi.config(_ip, _gw, _mask, _dns1, _dns2))
    {
        log_e("IotWifiNetwork: static IP config failed, falling back to DHCP");
    }

    // 1. targeted reconnect to the cached AP, skipping the all-channel scan
    bool triedFast = false;
    bool usedCachedLease = false;
    if (_fastReconnect && _rtc.wifiChannel != 0)
    {
        triedFast = true;
        // The cached lease is only valid on this path: it targets the same AP,
        // hence the same subnet. Bounded so a real DHCP bind renews it regularly.
        if (!_staticIpConfigured && _dhcpCache && _rtc.dhcpIp != 0
            && _rtc.dhcpReuseCount < (int32_t)_dhcpMaxReuse)
        {
            if (WiFi.config(IPAddress(_rtc.dhcpIp), IPAddress(_rtc.dhcpGw), IPAddress(_rtc.dhcpMask),
                            IPAddress(_rtc.dhcpDns1), IPAddress(_rtc.dhcpDns2)))
            {
                usedCachedLease = true;
                log_i("IotWifiNetwork: reusing cached DHCP lease ip=%s (reuse %ld/%lu)",
                    IPAddress(_rtc.dhcpIp).toString().c_str(),
                    (long)_rtc.dhcpReuseCount, (unsigned long)_dhcpMaxReuse);
            }
        }
        log_i("IotWifiNetwork: fast reconnect channel=%ld bssid=%02x:%02x:%02x:%02x:%02x:%02x",
            (long)_rtc.wifiChannel, _rtc.wifiBssid[0], _rtc.wifiBssid[1], _rtc.wifiBssid[2],
            _rtc.wifiBssid[3], _rtc.wifiBssid[4], _rtc.wifiBssid[5]);
        WiFi.begin(_ssid.c_str(), _password.c_str(), _rtc.wifiChannel, _rtc.wifiBssid);
        // give the fast attempt at most half the budget before falling back
        _waitForConnection(startTime + timeout_ms / 2);
    }

    // 2. fallback: plain full-scan connect with the remaining budget
    if (WiFi.status() != WL_CONNECTED)
    {
        if (triedFast)
        {
            log_w("IotWifiNetwork: fast reconnect failed, retrying with a full scan");
            _rtc.wifiChannel = 0;
        }
        if (usedCachedLease)
        {
            log_w("IotWifiNetwork: cached DHCP lease may be stale, reverting to DHCP");
            WiFi.config(IPAddress((uint32_t)0), IPAddress((uint32_t)0), IPAddress((uint32_t)0));
            _invalidateDhcpCache();
            usedCachedLease = false;
        }
        WiFi.begin(_ssid.c_str(), _password.c_str());
        _waitForConnection(deadline);
    }

    if (WiFi.status() != WL_CONNECTED)
    {
        log_e("IotWifiNetwork: connection failed");
        return false;
    }

    // refresh the caches for the next wakeup
    _rtc.wifiChannel = WiFi.channel();
    memcpy(_rtc.wifiBssid, WiFi.BSSID(), sizeof(_rtc.wifiBssid));
    if (!_staticIpConfigured && _dhcpCache)
    {
        if (usedCachedLease)
        {
            _rtc.dhcpReuseCount++;
        } else {
            _rtc.dhcpIp   = (uint32_t)WiFi.localIP();
            _rtc.dhcpGw   = (uint32_t)WiFi.gatewayIP();
            _rtc.dhcpMask = (uint32_t)WiFi.subnetMask();
            _rtc.dhcpDns1 = (uint32_t)WiFi.dnsIP(0);
            _rtc.dhcpDns2 = (uint32_t)WiFi.dnsIP(1);
            _rtc.dhcpReuseCount = 0;
        }
    }

    _connectDuration_ms = (uint32_t)(millis() - startTime);
    log_i("IotWifiNetwork: connected ip=%s after %u ms", localIp().c_str(), (unsigned)_connectDuration_ms);
    return true;
}

void IotWifiNetwork::disconnect()
{
    WiFi.disconnect(true);
}

bool IotWifiNetwork::isConnected() const
{
    return WiFi.status() == WL_CONNECTED;
}

std::string IotWifiNetwork::macAddress() const
{
    // read straight from the efuse: this must work before (and without) a
    // connection, because the device id and thus every API URL derives from it
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    char buf[13];
    snprintf(buf, sizeof(buf), "%02x%02x%02x%02x%02x%02x",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return std::string(buf);
}

std::string IotWifiNetwork::localIp() const
{
    if (!isConnected())
    {
        return std::string();
    }
    return std::string(WiFi.localIP().toString().c_str());
}

int IotWifiNetwork::rssi() const
{
    return isConnected() ? (int)WiFi.RSSI() : 0;
}


// *****************************************************************************
// IotAdcBattery
// *****************************************************************************

int IotAdcBattery::readMilliVolts()
{
    if (_config.pin < 0 || _config.divider == 0)
    {
        return -1;
    }
    if (_cached_mV > 0)
    {
        return _cached_mV;
    }

    if (_config.enablePin >= 0)
    {
        pinMode(_config.enablePin, OUTPUT);
        digitalWrite(_config.enablePin, _config.enableActiveHigh ? HIGH : LOW);
        delay(_config.settleTime_ms);
    }

    // average several readings: a single ESP32 ADC sample is noisy
    int samples = _config.samples > 0 ? _config.samples : 1;
    int64_t sum = 0;
    for (int i = 0; i < samples; i++)
    {
        sum += analogReadMilliVolts(_config.pin);
    }

    if (_config.enablePin >= 0)
    {
        digitalWrite(_config.enablePin, _config.enableActiveHigh ? LOW : HIGH);
    }

    int64_t voltage = sum / samples;
    voltage = voltage * _config.factor / _config.divider + _config.offset_mV;
    _cached_mV = (int)voltage;
    log_i("IotAdcBattery: pin=%d samples=%d voltage=%d mV", _config.pin, samples, _cached_mV);
    return _cached_mV;
}


// *****************************************************************************
// IotEspPower
// *****************************************************************************

void IotEspPower::deepSleep(uint32_t duration_s)
{
    esp_deep_sleep((uint64_t)duration_s * 1000000ULL);
}

void IotEspPower::restart()
{
    esp_restart();
}

void IotEspPower::shutdown()
{
    // no wakeup source configured: sleep until a physical reset
    esp_deep_sleep_start();
}

IotResetReason IotEspPower::resetReason() const
{
    switch (esp_reset_reason())
    {
        case ESP_RST_POWERON:   return IotResetReason::PowerOn;
        case ESP_RST_EXT:       return IotResetReason::External;
        case ESP_RST_SW:        return IotResetReason::Software;
        case ESP_RST_PANIC:     return IotResetReason::Panic;
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:       return IotResetReason::Watchdog;
        case ESP_RST_BROWNOUT:  return IotResetReason::Brownout;
        case ESP_RST_DEEPSLEEP: return IotResetReason::DeepSleep;
        default:                return IotResetReason::Unknown;
    }
}

IotWakeupCause IotEspPower::wakeupCause() const
{
    switch (esp_sleep_get_wakeup_cause())
    {
        case ESP_SLEEP_WAKEUP_TIMER:    return IotWakeupCause::Timer;
        case ESP_SLEEP_WAKEUP_EXT0:
        case ESP_SLEEP_WAKEUP_EXT1:
        case ESP_SLEEP_WAKEUP_GPIO:     return IotWakeupCause::Gpio;
        case ESP_SLEEP_WAKEUP_TOUCHPAD: return IotWakeupCause::Touchpad;
        case ESP_SLEEP_WAKEUP_ULP:      return IotWakeupCause::Ulp;
        case ESP_SLEEP_WAKEUP_UART:     return IotWakeupCause::Uart;
        default:                        return IotWakeupCause::Unknown;
    }
}
