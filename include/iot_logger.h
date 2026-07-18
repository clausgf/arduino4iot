/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

#include <cstdio>
#include <cstdarg>

#include "Arduino.h"

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
     * Call this at the end of a wakeup cycle (or rely on the automatic flush
     * before deep sleep/restart/shutdown). Does nothing if the buffer is empty
     * or buffering is disabled. Requires an active WiFi connection.
     *
     * @return the HTTP status code of the log request, 0 if nothing was sent
     */
    int flush();

    /**
     * Post a log message to the API. The body consist of ESP32 formatted
     * log lines.
     *
     * This method is similar to @see apiGet.
     */
    int postLog(const char * body, const char * apiPath = "log/{project}/{device}");


    // **********************************************************************
    // P r i v a t e
    // **********************************************************************

private:
    LogLevel _logLevel;
    bool _buffered;
    String _logBuffer;
};

extern IotLogger logger;
