/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#include "iot.h"

#include "cstdio"
#include <esp_task_wdt.h>
#include <esp_ota_ops.h>
#include <nvs_flash.h>
#include <esp_idf_version.h>
#include <esp_app_desc.h>
#include <esp_system.h>
#include <Preferences.h>
#include <ArduinoJson.h>

// Deliberately no #include "iot_ap.h": naming the provisioning module would
// keep DNSServer/WebServer (~42 KB) in every firmware. The application wires it
// in via setNoCredentialsHandler(), and the dependency stays one-way,
// iot_ap -> iot.
#include "iot_esp_ports.h"
#include "iot_text.h"

// *****************************************************************************

Iot iot;
static const char *tag = "iot";

bool Iot::_isWatchdogEnabled = false;

// *****************************************************************************

/**
 * The default power adapter. A function-local static, so it is constructed on
 * first use and Iot's constructor - which runs before main() in an unspecified
 * order across translation units - can safely bind to it.
 */
static IotPower& defaultPower()
{
    static IotEspPower power;
    return power;
}

/// The built-in WiFi station, configured by begin() with the seeded
/// credentials. Replaceable via Iot::setNetwork().
static IotWifiNetwork& defaultNetwork()
{
    static IotWifiNetwork network(iotRtcState());
    return network;
}

/// Reads the system time and never synchronizes it. An application that needs
/// SNTP attaches an IotNtpClock via Iot::setClock(); only then is any SNTP code
/// linked.
static IotClock& defaultClock()
{
    static IotSystemClock clock;
    return clock;
}

/// The server config mirror. Bound here rather than inside IotConfig, so the
/// configuration service itself names no adapter.
static IotStore& configStore()
{
    static IotNvsStore store(iot_nvs_keys::CONFIG_NAMESPACE);
    store.begin();
    return store;
}

IotStore& iotBootstrapStore()
{
    static IotNvsStore store(iot_nvs_keys::NAMESPACE);
    store.begin();
    return store;
}

/// The HTTP client the API talks through.
static IotHttp& defaultHttp()
{
    static IotEspHttp http;
    return http;
}

// *****************************************************************************

Iot::Iot() :
    _lifecycle(defaultPower(), iotRtcState()),

    _logLevel(config, (int32_t)IotLogger::Level::All, iot_config_keys::LOG_LEVEL,
        { .title = "Log level",
          .description = "Remote log threshold: 0 error, 1 warning, 2 info, 3 debug, 4 verbose, 5 all",
          .min = 0, .max = 5 }),
    _sleepDuration_s(config, 5 * 60, iot_config_keys::SLEEP,
        { .title = "Sleep duration (s)", .description = "Deep sleep between wakeups",
          .min = MIN_SLEEP_DURATION_S, .max = 7 * 24 * 60 * 60 }),

    _batteryMin_mV(config, -1, iot_config_keys::BATTERY_MIN,
        { .title = "Battery minimum (mV)",
          .description = "Below this supply voltage the device does not connect; -1 disables the check",
          .min = -1, .max = 10000 }),
    _batterySleep_s(config, 0, iot_config_keys::BATTERY_SLEEP,
        { .title = "Undervoltage sleep (s)",
          .description = "On undervoltage: 0 shuts down until a reset, > 0 sleeps that long "
                         "and measures again (e.g. solar recharging)",
          .min = 0, .max = 7 * 24 * 60 * 60 }),
    _panicSleepDurationInit_s(config, IotBackoffPolicy{}.initialDuration_s, iot_config_keys::PANIC_INIT,
        { .title = "Backoff initial (s)", .description = "First sleep after a failed cycle",
          .min = 1, .max = 7 * 24 * 60 * 60 }),
    _panicSleepDurationFactor(config, IotBackoffPolicy{}.factor, iot_config_keys::PANIC_FACTOR,
        { .title = "Backoff factor", .description = "Multiplier per further failed cycle",
          .min = 1, .max = 16 }),
    _panicSleepDurationMax_s(config, IotBackoffPolicy{}.maxDuration_s, iot_config_keys::PANIC_MAX,
        { .title = "Backoff maximum (s)", .description = "Upper bound of the backoff sleep",
          .min = 1, .max = 7 * 24 * 60 * 60 }),
    _wifiTxPower_qdBm(config, IotWifiNetwork::DEFAULT_TX_POWER_QDBM, iot_config_keys::WIFI_TX_POWER,
        { .title = "WiFi TX power (0.25 dBm)",
          .description = "Maximum transmit power in 0.25 dBm steps, 8..84 (2..21 dBm); "
                         "0 keeps the firmware's setting. Applies from the next connect.",
          .min = 0, .max = 84 })
{
    // initialize variables
    _deviceId = "";
    _watchdogTimeout_s = DEFAULT_WATCHDOG_TIMEOUT_S;
    _clockSyncTimeout_ms = DEFAULT_CLOCK_SYNC_TIMEOUT_MS;
    _network = &defaultNetwork();
    _clock = &defaultClock();
    _battery = nullptr;
    _panicHandler = [this] { defaultPanicHandler(); };
    _undervoltageHandler = [this] { defaultUndervoltageHandler(); };
    _firmwareVersion = "";
    _firmwareId = "";
    _firmwareSha256 = "";
    _exitHooksRegistered = false;

    // Nothing else happens here on purpose: NVS init, boot accounting and the
    // reaction to an abnormal reset all belong in begin(), where the serial
    // console is open and the other singletons are constructed.
}

void Iot::_registerExitHooks()
{
    if (_exitHooksRegistered)
    {
        return;
    }
    _exitHooksRegistered = true;

    // runs before every deep sleep, restart, shutdown and backoff sleep.
    // offline, logger.flush() keeps the log in RTC RAM for a later cycle, so
    // this is safe on the early failure paths and after radioOff() too.
    _lifecycle.addExitHook([] {
        logger.flush();
        api.closeConnection();
        delay(10);  // give the serial log a chance to drain
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

    // the config mirror next: the backoff below, the log level and the WiFi
    // settings of the connect take the server's values from the last cycle
    config.begin(configStore());
    logger.setLevel((IotLogger::Level)_logLevel.get());

    _registerExitHooks();
    _lifecycle.setBackoffPolicy(IotBackoffPolicy{
        _panicSleepDurationInit_s.get(),
        _panicSleepDurationFactor.get(),
        _panicSleepDurationMax_s.get() });
    _lifecycle.begin();

    // lines kept offline are led by a marker naming this cycle: the boot
    // count always, the time when the clock is plausible
    logger._offlineContext = [] { return iot._cycleName(); };

    // the watchdog must already run before the no-credentials handler is
    // reached, since that call can block far longer than the timeout
    startWatchdog(_watchdogTimeout_s);

    log_w("--- Bootup #%lu, reset reason %s, wakeup cause %s after %d s, backoff=%d s",
        (unsigned long)bootCount(),
        iotResetReasonToString(_lifecycle.resetReason()),
        iotWakeupCauseToString(_lifecycle.wakeupCause()),
        lastSleepDuration_s(), _backoffDuration_s());
    log_i("--- Firmware version %s", firmwareVersion().c_str());
    log_i("--- Firmware id %s", firmwareId().c_str());
    log_i("--- SHA256 %s", firmwareSha256().c_str());
    // before any connect: a network with a MAC allow list needs this address
    log_i("--- Device id %s, MAC %s", deviceId().c_str(), macAddress().c_str());

    // before the abnormal-start check: a brownout on a weak battery is an
    // undervoltage, not a failure to back off from; and before any connect,
    // whose current peaks are what browns a weak battery out
    _checkBattery();

    // the previous cycle crashed, hung or browned out: back off rather than
    // retry immediately
    if (_lifecycle.isAbnormalStart())
    {
        _panicEarly("*** PANIC *** Previous cycle ended abnormally: %s",
            iotResetReasonToString(_lifecycle.resetReason()));
    }
}

void Iot::_initSubsystems()
{
    if (!_network->isConnected())
    {
        log_e("Network not connected yet. Connect first.");
    }

    // initialize other components
    logger.setNetwork(*_network);
    api.setDeviceName(deviceId());
    api.setBoardId(boardId());
    api.begin(iotBootstrapStore(), defaultHttp(), *_clock);
}

bool Iot::begin(uint32_t timeout_ms)
{
    _beginCycle();

    // a person asking for the setup handler: only at a manual start, so the
    // check never lengthens a deep-sleep cycle
    const IotResetReason resetReason = _lifecycle.resetReason();
    if (_setupTrigger && _noCredentialsHandler
        && (resetReason == IotResetReason::PowerOn || resetReason == IotResetReason::External)
        && _setupTrigger())
    {
        log_w("begin: setup requested, invoking the setup handler");
        _noCredentialsHandler();
        log_w("begin: the setup handler returned, continuing");
    }

    // WiFi credentials were seeded into the store by seedCredentials()
    const std::string ssid = iotBootstrapStore().getString(iot_nvs_keys::WIFI_SSID, "");
    const std::string password = iotBootstrapStore().getString(iot_nvs_keys::WIFI_PASSWORD, "");

    if (ssid.empty())
    {
        if (_noCredentialsHandler)
        {
            log_w("begin: no WiFi SSID seeded, invoking the no-credentials handler");
            _noCredentialsHandler();  // normally does not return, e.g. apProvisioning.run()
            log_w("begin: the no-credentials handler returned, shutting down");
        } else {
            log_e("begin: no WiFi SSID seeded and no handler registered - this device "
                  "cannot reach the server. Seed credentials via iot.seedCredentials() "
                  "or an NVS partition image, or register a setup flow with "
                  "iot.setNoCredentialsHandler([]{ apProvisioning.run(); }).");
        }
        logger.error(tag, "no WiFi credentials seeded, shutting down");
        // indefinite deep sleep: nothing can change the "no credentials"
        // condition between wakeups, so a timer retry would only drain the
        // battery. Only a physical reset tries again.
        shutdown();  // never returns
    }

    // the built-in adapter takes the seeded credentials; a replacement brings
    // its own configuration
    if (_network == &defaultNetwork())
    {
        defaultNetwork().setCredentials(ssid, password);
        defaultNetwork().setHostname(deviceId());
        if (_wifiTxPower_qdBm.get() != IotWifiNetwork::DEFAULT_TX_POWER_QDBM)
        {
            defaultNetwork().setMaxTxPower_qdBm((int8_t)_wifiTxPower_qdBm.get());
        }
    }

    bool connected = _network->connect((uint32_t)timeout_ms);
    if (!connected)
    {
        // offline, so this goes to RTC RAM and reaches the server with a later
        // cycle - the one place the reason is known
        const std::string reason = _network->lastError();
        logger.error(tag, "network connect failed: %s", reason.empty() ? "unknown reason" : reason.c_str());
    }
    _initSubsystems();

    if (!_syncClock() && _clock != &defaultClock())
    {
        logger.warn(tag, "clock sync failed, the time is not plausible");
    }
    if (!isTimePlausible())
    {
        log_w("begin: the clock is not plausible - device tokens are renewed only "
              "on a 401 and timestamps are meaningless; attach an IotNtpClock via "
              "iot.setClock() if the application needs the time.");
    }
    return connected;
}

void Iot::setNoCredentialsHandler(std::function<void()> handler)
{
    _noCredentialsHandler = std::move(handler);
}

void Iot::setSetupTrigger(std::function<bool()> trigger)
{
    _setupTrigger = std::move(trigger);
}

void Iot::seedCredentials(const IotSeedConfig& cfg)
{
    // NVS has to be up before the store can open, and seedCredentials() is
    // documented to run before begin()
    nvs_flash_init();
    if (iotSeedCredentials(iotBootstrapStore(), cfg))
    {
        log_w("seedCredentials: wrote seed generation %u", (unsigned)cfg.seedGeneration);
    }
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
    for (const char* ns : { iot_nvs_keys::NAMESPACE, iot_nvs_keys::CONFIG_NAMESPACE })
    {
        clearNvsNamespace(ns);
    }
}

void Iot::clearProvisioning()
{
    log_w("clearProvisioning: erasing seed/provisioning state ('iot' namespace only)");
    clearNvsNamespace(iot_nvs_keys::NAMESPACE);
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

void Iot::setNetwork(IotNetwork& network)
{
    _network = &network;
}

void Iot::setClock(IotClock& clock)
{
    _clock = &clock;
}

void Iot::setBattery(IotBattery& battery)
{
    _battery = &battery;
}

IotWifiNetwork& Iot::wifi()
{
    return defaultNetwork();
}

void Iot::radioOff()
{
    logger.flush();
    api.closeConnection();
    _network->disconnect();
    log_i("radioOff: network disconnected for the rest of the cycle");
}

// *****************************************************************************

std::string Iot::deviceId() const
{
    if (_deviceId.empty())
    {
        // The device id doubles as the device name for the API. nice4iot
        // requires device names to match ^[a-zA-Z_][a-zA-Z0-9_]*$ (no hyphen),
        // so use an underscore separator, e.g. "e32_aabbccddeeff".
        _deviceId = "e32_" + _network->macAddress();
    }
    return _deviceId;
}

std::string Iot::macAddress() const
{
    return iot_text::formatMacAddress(_network->macAddress());
}


// *****************************************************************************
// NTP time
// *****************************************************************************

std::string Iot::timeIso(int64_t epoch_s) const
{
    time_t time = (time_t)(epoch_s < 0 ? _clock->now() : epoch_s);
    struct tm timeinfo;
    gmtime_r(&time, &timeinfo);

    // local (not static) buffer: copied into the returned string right away,
    // so there is no shared state to race between concurrent callers
    const int BUFLEN = 32;
    char buf[BUFLEN];
    snprintf(buf, BUFLEN, "%04d-%02d-%02dT%02d:%02d:%02dZ",
        (timeinfo.tm_year + 1900) % 10000, (timeinfo.tm_mon + 1) % 100, timeinfo.tm_mday % 100,
        timeinfo.tm_hour % 100, timeinfo.tm_min % 100, timeinfo.tm_sec % 100);
    return buf;
}

// *****************************************************************************

bool Iot::isTimePlausible() const
{
    return _clock->isPlausible();
}

std::string Iot::_cycleName() const
{
    char name[64];
    snprintf(name, sizeof(name), "boot %lu, %s", (unsigned long)bootCount(),
        isTimePlausible() ? timeIso().c_str() : "clock not set");
    return name;
}

bool Iot::_syncClock()
{
    return _clock->sync(_clockSyncTimeout_ms);
}


// *****************************************************************************
// API
// *****************************************************************************

IotResult IotTelemetryEndpoint::post(const std::string& kind, const std::string& jsonData,
    const std::string& apiPath) const
{
    if (jsonData.length() > IOT_MAX_TELEMETRY_SIZE)
    {
        log_w("Telemetry body size %u exceeds the server's default limit of %u bytes",
            (unsigned)jsonData.length(), (unsigned)IOT_MAX_TELEMETRY_SIZE);
    }
    std::string path = apiPath;
    size_t kindPos = path.find("{kind}");
    if (kindPos != std::string::npos)
    {
        path.replace(kindPos, 6, kind);
    }
    // other variables are replaced by the api
    return api.post(path, jsonData).result;
}

// *****************************************************************************

IotResult IotTelemetryEndpoint::post(const std::string& kind, const IotTelemetry& telemetry,
    const std::string& apiPath) const
{
    std::string jsonData;
    telemetry.serializeTo(jsonData);
    return post(kind, jsonData, apiPath);
}

// *****************************************************************************

IotResult IotTelemetryEndpoint::postSystem(const std::string& kind, const std::string& apiPath) const
{
    IotTelemetry telemetry;
    int battery_mV = iot.battery_mV();
    if (battery_mV >= 0)
    {
        telemetry.add("battery_v", battery_mV / 1000.0);
    }
    telemetry.add("wifi_rssi", iot.network().rssi());
    telemetry.add("wifi_connect_ms", (int32_t)iot.network().lastConnectDuration_ms());
    telemetry.add("boot_count", iot.bootCount());
    telemetry.add("reset_reason", (int32_t)iot.resetReason());
    telemetry.add("active_ms", iot.lastActiveDuration_ms());
    telemetry.add("last_sleep_s", iot.lastSleepDuration_s());
    telemetry.add("panic_sleep_s", iot._backoffDuration_s());
    telemetry.add("panic_count", iot.panicCount());
    telemetry.add("crash_count", iot.crashCount());
    telemetry.add("undervoltage_count", iot.undervoltageCount());
    // strings become labels in nice4iot (at most 16 per post, 128 characters
    // each, change history kept): only slowly changing values belong here,
    // never a timestamp - the server stamps each post with its arrival time
    telemetry.add("firmware_id", iot.firmwareId());
    std::string firmwareVersion = iot.firmwareVersion();
    if (!firmwareVersion.empty())
    {
        telemetry.add("firmware_version", firmwareVersion);
    }
    telemetry.add("firmware_sha256", iot.firmwareSha256());
    std::string boardId = iot.boardId();
    if (!boardId.empty())
    {
        telemetry.add("board", boardId);
    }
    return post(kind, telemetry, apiPath);
}


// *****************************************************************************
// Battery
// *****************************************************************************

int32_t Iot::battery_mV() const
{
    return _battery != nullptr ? _battery->read_mV() : -1;
}


// *****************************************************************************
// Error handling / Panic
// *****************************************************************************

void Iot::setBackoff(const IotBackoffPolicy& policy)
{
    _panicSleepDurationInit_s = policy.initialDuration_s;
    _panicSleepDurationFactor = policy.factor;
    _panicSleepDurationMax_s = policy.maxDuration_s;
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
    _lifecycle.countPanic();
    va_list args;
    va_start(args, format);
    logger.logv(IotLogger::Level::Error, tag, format, args);
    va_end(args);

    // the handler typically ends the cycle, and the exit hooks flush this
    // error together with the handler's own lines in one request (or keep
    // them in RTC RAM when offline); flush here only if it returns
    _panicHandler();
    logger.flush();
}

void Iot::_panicEarly(const char* format...)
{
    va_list args;
    va_start(args, format);
    const int logBufLen = 160;
    char logBuf[logBufLen];
    vsnprintf(logBuf, logBufLen, format, args);
    log_e("%s", logBuf);
    va_end(args);

    // no request here: straight into RTC RAM, sent by a later cycle
    logger.logToRtc(IotLogger::Level::Error, tag, "%s", logBuf);

    delay(10);  // delay to allow log to be written
    _panicHandler();
}

// *****************************************************************************

void Iot::_checkBattery()
{
    if (_battery == nullptr || _batteryMin_mV.get() <= 0)
    {
        return;
    }
    const int32_t battery_mV = this->battery_mV();
    if (battery_mV < 0 || battery_mV >= _batteryMin_mV.get())
    {
        return;
    }
    log_e("Battery voltage too low: %ld mV < %ld mV", (long)battery_mV, (long)_batteryMin_mV.get());
    // offline at this point: kept in RTC RAM, sent by a later cycle
    logger.error(tag, "undervoltage %ld mV < %ld mV", (long)battery_mV, (long)_batteryMin_mV.get());
    _lifecycle.countUndervoltage();
    _undervoltageHandler();
}

std::function<void()> Iot::setUndervoltageHandler(std::function<void()> undervoltageHandler)
{
    std::function<void()> oldUndervoltageHandler = _undervoltageHandler;
    _undervoltageHandler = undervoltageHandler;
    return oldUndervoltageHandler;
}

void Iot::defaultUndervoltageHandler()
{
    const int32_t sleep_s = _batterySleep_s.get();
    if (sleep_s > 0)
    {
        logger.warn(tag, "undervoltage: sleeping %ld s, then measuring again", (long)sleep_s);
        _deepSleep(sleep_s, /*isFailure=*/true);
    }
    logger.warn(tag, "undervoltage: shutting down until a reset");
    _shutdown(/*isFailure=*/true);
}

void Iot::defaultPanicHandler()
{
    // the escalation itself is IotBackoffPolicy (iot_lifecycle.h)
    const int32_t next_s = _lifecycle.backoffPolicy().next(_lifecycle.backoffDuration_s());
    log_w("Entering failure backoff: sleeping %ld s", (long)next_s);
    logger.warn(tag, "failure backoff: sleeping %ld s (boot %lu)", (long)next_s,
        (unsigned long)bootCount());
    _lifecycle.enterBackoff();  // runs the exit hooks and does not return
}


// *****************************************************************************
// System management: firmware
// *****************************************************************************

std::string Iot::firmwareVersion() const
{
    // precedence: explicit setter (_firmwareVersion) > injected build define
    if (_firmwareVersion.empty())
    {
#ifdef IOT_FW_VERSION
        _firmwareVersion = IOT_FW_VERSION;
#endif
    }
    return _firmwareVersion;
}

void Iot::setFirmwareVersion(const std::string& version)
{
    _firmwareVersion = version;
    _firmwareId.clear();  // firmwareId() leads with the version
}

std::string Iot::firmwareId() const
{
    if (_firmwareId.empty())
    {
        // not the app descriptor's project name, version, date and time: in an
        // Arduino build they describe the precompiled framework libraries
#ifdef IOT_FW_DATE
        const std::string date = IOT_FW_DATE;
#else
        const std::string date;
#endif
        char arduinoVersion[16];
        snprintf(arduinoVersion, sizeof(arduinoVersion), "%d.%d.%d",
            ESP_ARDUINO_VERSION_MAJOR, ESP_ARDUINO_VERSION_MINOR, ESP_ARDUINO_VERSION_PATCH);
#ifdef IOT_LIB_VERSION
        const std::string iotVersion = IOT_LIB_VERSION;
#else
        char iotVersion[16];
        snprintf(iotVersion, sizeof(iotVersion), "%d.%d.%d",
            IOT_VERSION_MAJOR, IOT_VERSION_MINOR, IOT_VERSION_PATCH);
#endif
        _firmwareId = iot_text::firmwareId(firmwareVersion(), date, arduinoVersion,
            iot_text::releaseVersion(esp_get_idf_version()), iotVersion,
            esp_app_get_description()->secure_version);
    }
    return _firmwareId;
}

std::string Iot::firmwareSha256() const
{
    if (_firmwareSha256.empty())
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
            _firmwareSha256 = buf;
        }
    }
    return _firmwareSha256;
}

std::string Iot::boardId() const
{
#ifdef IOT_BOARD_ID
    return std::string(IOT_BOARD_ID);
#else
    return std::string();
#endif
}


// *****************************************************************************
// System management: watchdog
// *****************************************************************************

void Iot::startWatchdog(int32_t watchdogTimeout_s)
{
    // remembered, so a call before begin() chooses what begin() uses
    if (watchdogTimeout_s < MIN_WATCHDOG_TIMEOUT_S)
    {
        log_e("startWatchdog(%d) is below the %d s minimum, clamping",
            watchdogTimeout_s, MIN_WATCHDOG_TIMEOUT_S);
        watchdogTimeout_s = MIN_WATCHDOG_TIMEOUT_S;
    }
    _watchdogTimeout_s = watchdogTimeout_s;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    esp_task_wdt_config_t wdtConfig = {
        .timeout_ms = (uint32_t)watchdogTimeout_s * 1000u,
        .idle_core_mask = 0,
        .trigger_panic = true, // panic on watchdog timeout
    };
    // reconfigure first: the Arduino core usually initialized the TWDT already
    // (CONFIG_ESP_TASK_WDT_INIT), and esp_task_wdt_init() would log an error
    esp_err_t err = esp_task_wdt_reconfigure(&wdtConfig);
    if (err == ESP_ERR_INVALID_STATE)
    {
        err = esp_task_wdt_init(&wdtConfig);
    }
#else
    esp_err_t err = esp_task_wdt_init(watchdogTimeout_s, true); // panic=true on watchdog timeout
#endif
    if (err != ESP_OK)
    {
        panic("*** PANIC *** Error in task watchdog setup: 0x%x", err);
    }
    log_i("Task watchdog timeout=%d s", watchdogTimeout_s);

    // subscribe only once; a repeated call just changes the timeout
    if (esp_task_wdt_status(NULL) == ESP_ERR_NOT_FOUND) // NULL for current task
    {
        err = esp_task_wdt_add(NULL);
        if (err != ESP_OK)
        {
            // the cycle runs unsupervised, and resetWatchdog() does nothing
            log_e("Error in esp_task_wdt_add: 0x%x, the task is not supervised", err);
        }
    }
    log_d("Task watchdog started for current task");
}

void Iot::stopWatchdog()
{
    // not subscribed (or no TWDT at all): there is nothing to stop
    if (esp_task_wdt_status(NULL) != ESP_OK)
    {
        return;
    }
    esp_err_t err = esp_task_wdt_delete(NULL);
    if (err != ESP_OK)
    {
        panic("*** PANIC *** esp_task_wdt_delete=%d", err);
    }
    log_d("Task watchdog stopped for current task");
}

void Iot::resetWatchdog()
{
    // not subscribed (e.g. after stopWatchdog(), or another task): nothing to
    // feed - checked first, since esp_task_wdt_reset() would log an error
    if (esp_task_wdt_status(NULL) != ESP_OK)
    {
        return;
    }
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

void Iot::setSleepDuration_s(int32_t sleep_duration_s)
{
    _sleepDuration_s = sleep_duration_s;
}

void Iot::setPower(IotPower& power)
{
    _lifecycle.setPower(power);
}

void Iot::addExitHook(std::function<void()> hook)
{
    _registerExitHooks();  // keeps the library's own hook first
    _lifecycle.addExitHook(std::move(hook));
}

// *****************************************************************************

void Iot::deepSleep()
{
    // a config.json can set this, and 0 would wake the device immediately and
    // flatten the battery - clamp rather than obey
    int32_t duration_s = _sleepDuration_s.get();
    if (duration_s < MIN_SLEEP_DURATION_S)
    {
        log_e("sleep_s=%d is below the %d s minimum, clamping", duration_s, MIN_SLEEP_DURATION_S);
        duration_s = MIN_SLEEP_DURATION_S;
    }
    deepSleep(duration_s);
}

// IotLifecycle records the cycle in the RTC state, runs the exit hooks (log
// flush, connection close) and dispatches to the IotPower adapter. The public
// methods end a cycle cleanly and clear the escalating backoff; a failure exit
// (isFailure) leaves it untouched and is reached only through panic() and the
// undervoltage check.

void Iot::deepSleep(int32_t sleep_duration_s)
{
    _deepSleep(sleep_duration_s, /*isFailure=*/false);
}

void Iot::restart()
{
    _restart(/*isFailure=*/false);
}

void Iot::shutdown()
{
    _shutdown(/*isFailure=*/false);
}

void Iot::_deepSleep(int32_t sleep_duration_s, bool isFailure)
{
    _registerExitHooks();
    log_w("Active for %lu ms, going to deep sleep for %d s",
        (unsigned long)millis(), sleep_duration_s);
    _lifecycle.deepSleep(sleep_duration_s > 0 ? sleep_duration_s : 0, isFailure);
}

void Iot::_restart(bool isFailure)
{
    _registerExitHooks();
    log_w("Active for %lu ms, restarting", (unsigned long)millis());
    _lifecycle.restart(isFailure);
}

void Iot::_shutdown(bool isFailure)
{
    _registerExitHooks();
    log_w("Active for %lu ms, shutting down", (unsigned long)millis());
    _lifecycle.shutdown(isFailure);
}

// *****************************************************************************
