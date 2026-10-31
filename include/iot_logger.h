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

    enum LogLevel { IOT_LOGLEVEL_ERROR = 0, IOT_LOGLEVEL_WARNING = 1, IOT_LOGLEVEL_INFO = 2, IOT_LOGLEVEL_DEBUG = 3, IOT_LOGLEVEL_VERBOSE = 4, IOT_LOGLEVEL_NOTSET = 5  };

    IotLogger();
    void begin(LogLevel logLevel = IOT_LOGLEVEL_NOTSET);
    void end();

    /// Where to ask whether the buffer can be sent. Without one, flush() tries
    /// and lets the request fail.
    void setNetwork(IotNetwork* network) { _network = network; }


    // **********************************************************************
    // Logging
    // **********************************************************************

 
    /**
     * Set minimum criticality of log info to output. Info with lower
     * criticality is suppressed.
     */
    void setLogLevel(LogLevel level);

    /**
     * Log output with given level, format and arguments referenced by ap.
     */
    void logv(LogLevel level, const char *tag, const char* format, va_list ap);

    /**
     * Log output with given level, format and printf()-style arguments.
     */
    void logf(LogLevel level, const char *tag, const char* format...);

    /**
     * Log a line straight into the RTC log, for a later cycle: never touches
     * the network or the RAM buffer. For paths where a request is unsafe or
     * pointless, e.g. a panic before the network is up.
     */
    void logToRtc(LogLevel level, const char *tag, const char* format...);

    // --- logging helpers ---
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

    /**
     * Post a log message to the API. The body consist of ESP32 formatted
     * log lines.
     *
     * Goes through api.post(), so the device token and the 401 retry apply.
     */
    IotResult postLog(const char * body, const char * apiPath = iot_paths::LOG);


    // **********************************************************************
    // P r i v a t e
    // **********************************************************************

private:
    LogLevel _logLevel;
    bool _buffered;
    std::string _logBuffer;
    IotNetwork* _network = nullptr;

    /// Without a network adapter flush() tries and lets the request fail.
    bool _isOnline() const;
};

extern IotLogger logger;
