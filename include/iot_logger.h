/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

#include <cstdarg>
#include <cstdio>
#include <string>

#include "iot_names.h"
#include "iot_ports.h"
#include "iot_result.h"

/// Bytes of RTC RAM that keep log lines across deep sleep while the device is
/// offline, @see IotLogger::flush(). 0 disables it.
#ifndef IOT_RTC_LOG_SIZE
#define IOT_RTC_LOG_SIZE 1536
#endif

// *****************************************************************************

class IotLogger
{
public:
    // disallow copying & assignment
    IotLogger(const IotLogger&) = delete;
    IotLogger& operator=(const IotLogger&) = delete;

    /// The level of a line, and the threshold of setLevel(): a line is kept if
    /// its level is at most the threshold. The numbers are the config.json
    /// values of log_level.
    enum class Level : uint8_t { Error = 0, Warning = 1, Info = 2, Debug = 3, Verbose = 4, All = 5 };

    IotLogger();

    /// Lines above @p level are dropped. Default: All. iot.begin() sets it from
    /// the config value log_level.
    void setLevel(Level level) { _level = level; }

    /// Log with @p level and printf()-style arguments.
    void log(Level level, const char *tag, const char* format...);

    // --- logging helpers ---
    // A message is one line: line breaks in it become spaces.
    void error(const char *tag, const char* format...);
    void warn(const char *tag, const char* format...);
    void info(const char *tag, const char* format...);
    void debug(const char *tag, const char* format...);
    void verbose(const char *tag, const char* format...);

    /**
     * Enable or disable log buffering (enabled by default).
     *
     * When buffering is enabled, log messages are collected in RAM and sent
     * to the server in a single request by @see flush() (called automatically
     * before deep sleep, restart and shutdown). This avoids one HTTP request
     * per log line, which is crucial for short active periods on battery
     * powered devices, and avoids reentrancy problems when logging happens
     * during an API request.
     *
     * When buffering is disabled, each log message is posted immediately
     * (legacy behavior).
     */
    void setBuffered(bool enabled);

    /**
     * Post the buffered log messages to the server and clear the buffer.
     *
     * Call this at the end of a wakeup cycle, or rely on the automatic flush
     * before deep sleep/restart/shutdown.
     *
     * Offline - after iot.radioOff(), or when the connect failed - the buffer
     * moves to RTC RAM (IOT_RTC_LOG_SIZE bytes, oldest lines dropped first)
     * and goes out first with the next flush that has connectivity, in a later
     * cycle. A send that fails with a transport error or 5xx is kept the same
     * way. In unbuffered mode, a line logged offline is kept there too.
     *
     * @return Ok if everything was sent or there was nothing to send, a
     *         transport error when offline, otherwise the failed request
     */
    IotResult flush();

private:
    friend class Iot;

    /// Where to ask whether the buffer can be sent. Without one, flush() tries
    /// and lets the request fail.
    void setNetwork(IotNetwork& network) { _network = &network; }
    void end();

    void logv(Level level, const char *tag, const char* format, va_list ap);

    /// Straight into the RTC log, for a later cycle: never touches the
    /// network or the RAM buffer. For a panic before the network is up.
    void logToRtc(Level level, const char *tag, const char* format...);

    /// Post ESP-formatted log lines, split at the server's size limit.
    IotResult postLog(const char * body, const char * apiPath = iot_paths::LOG);

    Level _level;
    bool _buffered;
    std::string _logBuffer;
    IotNetwork* _network = nullptr;

    /// Without a network adapter flush() tries and lets the request fail.
    bool _isOnline() const;

    /// Names the current cycle in the marker that leads its offline lines,
    /// e.g. "boot 17, 2026-11-08T10:00:00Z". Set by Iot, which knows both.
    std::string (*_offlineContext)() = nullptr;
    bool _offlineMarked = false;
    void _keepOffline(const std::string& lines);
    void _restoreOffline(const std::string& lines);
};

extern IotLogger logger;
