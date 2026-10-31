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
#include <Preferences.h>
#include <ArduinoJson.h>

// Deliberately no #include "iot_ap.h": naming the provisioning module would
// keep DNSServer/WebServer (~42 KB) in every firmware. The application wires it
// in via setNoCredentialsHandler(), and the dependency stays one-way,
// iot_ap -> iot.
#include "iot_esp_ports.h"

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

    _logLevel(config, IotLogger::LogLevel::IOT_LOGLEVEL_NOTSET, "log_level",
        { .title = "Log level",
          .description = "Remote log threshold: 0 error, 1 warning, 2 info, 3 debug, 4 verbose, 5 all",
          .min = 0, .max = 5 }),
    _sleepDuration_s(config, 5 * 60, "sleep_s",
        { .title = "Sleep duration (s)", .description = "Deep sleep between wakeups",
          .min = MIN_SLEEP_DURATION_S, .max = 7 * 24 * 60 * 60 }),

    _batteryMin_mV(config, -1, "battery_min_mv",
        { .title = "Battery minimum (mV)",
          .description = "Shut down below this supply voltage; -1 disables the check",
          .min = -1, .max = 10000 }),
    _panicSleepDurationInit_s(config, IotBackoffPolicy{}.initialDuration_s, "panic_init_s",
        { .title = "Backoff initial (s)", .description = "First sleep after a failed cycle",
          .min = 1, .max = 7 * 24 * 60 * 60 }),
    _panicSleepDurationFactor(config, IotBackoffPolicy{}.factor, "panic_factor",
        { .title = "Backoff factor", .description = "Multiplier per further failed cycle",
          .min = 1, .max = 16 }),
    _panicSleepDurationMax_s(config, IotBackoffPolicy{}.maxDuration_s, "panic_max_s",
        { .title = "Backoff maximum (s)", .description = "Upper bound of the backoff sleep",
          .min = 1, .max = 7 * 24 * 60 * 60 })
{
    // initialize variables
    _deviceId = "";
    _watchdogTimeout_s = DEFAULT_WATCHDOG_TIMEOUT_S;
    _clockSyncTimeout_ms = DEFAULT_CLOCK_SYNC_TIMEOUT_MS;
    _network = &defaultNetwork();
    _clock = &defaultClock();
    _battery = nullptr;
    _panicHandler = [this] { _enterBackoff(); };
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

    _registerExitHooks();
    _lifecycle.setBackoffPolicy(IotBackoffPolicy{
        _panicSleepDurationInit_s.get(),
        _panicSleepDurationFactor.get(),
        _panicSleepDurationMax_s.get() });
    _lifecycle.begin();

    // the watchdog must already run before the no-credentials handler is
    // reached, since that call can block far longer than the timeout
    startWatchdog(_watchdogTimeout_s);

    log_w("--- Bootup #%lu, reset reason %s, wakeup cause %s after %d s, backoff=%d s",
        (unsigned long)getBootCount(),
        iotResetReasonToString(_lifecycle.resetReason()),
        iotWakeupCauseToString(_lifecycle.wakeupCause()),
        getLastSleepDuration_s(), _backoffDuration_s());
    log_i("--- Firmware version %s", getFirmwareVersion().c_str());
    log_i("--- Firmware id %s", getFirmwareId().c_str());
    log_i("--- SHA256 %s", getFirmwareSha256().c_str());

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

    // read configuration again to allow overwriting hardcoded parameters with WiFi config
    config.begin(configStore());
    logger.begin((IotLogger::LogLevel)_logLevel.get());

    // the backoff parameters may have been changed by the server config
    _lifecycle.setBackoffPolicy(IotBackoffPolicy{
        _panicSleepDurationInit_s.get(),
        _panicSleepDurationFactor.get(),
        _panicSleepDurationMax_s.get() });

    // check the battery voltage
    if (_battery != nullptr && _batteryMin_mV.get() > 0)
    {
        int batteryVoltage_mV = getBatteryVoltage_mV();
        if (batteryVoltage_mV >= 0 && batteryVoltage_mV < _batteryMin_mV.get())
        {
            log_e("Battery voltage too low: %d mV < %d mV", batteryVoltage_mV, _batteryMin_mV.get());
            logger.error(tag, "undervoltage %d mV < %d mV, shutting down",
                batteryVoltage_mV, _batteryMin_mV.get());
            _shutdown(/*isFailure=*/true);
        }
    }

    // initialize other components
    startWatchdog(_watchdogTimeout_s);
    logger.setNetwork(_network);
    api.setDeviceName(getDeviceId());
    api.setBoardId(getBoardId());
    api.begin(iotBootstrapStore(), defaultHttp(), *_clock);
}

bool Iot::begin(uint32_t timeout_ms)
{
    _beginCycle();

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
        defaultNetwork().setHostname(getDeviceId());
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

std::string Iot::getDeviceId()
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


// *****************************************************************************
// NTP time
// *****************************************************************************

std::string Iot::getTimeIso(int64_t epoch_s)
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

bool Iot::isTimePlausible()
{
    return _clock->isPlausible();
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
            jsonData.length(), IOT_MAX_TELEMETRY_SIZE);
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
    int battery_mV = iot.getBatteryVoltage_mV();
    if (battery_mV >= 0)
    {
        telemetry.add("battery_V", battery_mV / 1000.0);
    }
    telemetry.add("wifi_rssi", iot.network().rssi());
    telemetry.add("wifi_connect_ms", (int32_t)iot.network().lastConnectDuration_ms());
    telemetry.add("boot_count", iot.getBootCount());
    telemetry.add("active_ms", iot.getActiveDuration_ms());
    telemetry.add("lastSleep_s", iot.getLastSleepDuration_s());
    telemetry.add("panicSleep_s", iot._backoffDuration_s());
    // non-numeric fields are ignored by the nice4iot telemetry backend,
    // but kept for backends which support them
    telemetry.add("time", iot.getTimeIso());
    telemetry.add("firmware_id", iot.getFirmwareId());
    std::string firmwareVersion = iot.getFirmwareVersion();
    if (!firmwareVersion.empty())
    {
        telemetry.add("firmware_version", firmwareVersion);
    }
    telemetry.add("firmware_sha256", iot.getFirmwareSha256());
    std::string boardId = iot.getBoardId();
    if (!boardId.empty())
    {
        telemetry.add("board_id", boardId);
    }
    return post(kind, telemetry, apiPath);
}


// *****************************************************************************
// Battery
// *****************************************************************************

int Iot::getBatteryVoltage_mV()
{
    return _battery != nullptr ? _battery->readMilliVolts() : -1;
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
    va_list args;
    va_start(args, format);
    logger.logv(IotLogger::LogLevel::IOT_LOGLEVEL_ERROR, tag, format, args);
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
    logger.logToRtc(IotLogger::LogLevel::IOT_LOGLEVEL_ERROR, tag, "%s", logBuf);

    delay(10);  // delay to allow log to be written
    _panicHandler();
}

// *****************************************************************************

void Iot::_enterBackoff()
{
    // the escalation itself is IotBackoffPolicy (iot_lifecycle.h)
    const int32_t next_s = _lifecycle.backoffPolicy().next(_lifecycle.backoffDuration_s());
    log_w("Entering failure backoff: sleeping %ld s", (long)next_s);
    logger.warn(tag, "failure backoff: sleeping %ld s (boot %lu)", (long)next_s,
        (unsigned long)getBootCount());
    _lifecycle.enterBackoff();  // runs the exit hooks and does not return
}


// *****************************************************************************
// System management: firmware
// *****************************************************************************

std::string Iot::getFirmwareVersion()
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
}

std::string Iot::getFirmwareId()
{
    if (_firmwareId.empty())
    {
        const esp_partition_t *running = esp_ota_get_running_partition();
        esp_app_desc_t app_info;
        if (esp_ota_get_partition_description(running, &app_info) == ESP_OK)
        {
            char buf[224];
            snprintf(buf, sizeof(buf), "%s %s %s %s IDF %s sec %u ARDUINO %d.%d.%d IOT %d.%d.%d",
                app_info.project_name, app_info.version, app_info.date, app_info.time,
                app_info.idf_ver, (unsigned)app_info.secure_version,
                ESP_ARDUINO_VERSION_MAJOR, ESP_ARDUINO_VERSION_MINOR, ESP_ARDUINO_VERSION_PATCH,
                IOT_VERSION_MAJOR, IOT_VERSION_MINOR, IOT_VERSION_PATCH);
            _firmwareId = buf;
        }
    }
    return _firmwareId;
}

std::string Iot::getFirmwareSha256()
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

std::string Iot::getBoardId()
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

void Iot::deepSleep(int sleep_duration_s)
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

void Iot::_deepSleep(int sleep_duration_s, bool isFailure)
{
    _registerExitHooks();
    log_w("Active for %lu ms, going to deep sleep for %d s",
        (unsigned long)millis(), sleep_duration_s);
    _lifecycle.deepSleep(sleep_duration_s > 0 ? (uint32_t)sleep_duration_s : 0, isFailure);
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
