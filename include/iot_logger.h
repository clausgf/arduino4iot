/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */


#include <cstdio>
#include <cstdarg>

#include "Arduino.h"

#pragma once

// *****************************************************************************

class IotLogger
{
public:
    // disallow copying & assignment
    IotLogger(const IotLogger&) = delete;
    IotLogger& operator=(const IotLogger&) = delete;

    IotLogger();
    void begin();
    void end();


    // **********************************************************************
    // Logging
    // **********************************************************************

    enum LogLevel { IOT_LOGLEVEL_ERROR = 0, IOT_LOGLEVEL_WARNING = 1, IOT_LOGLEVEL_INFO = 2, IOT_LOGLEVEL_DEBUG = 3, IOT_LOGLEVEL_VERBOSE = 4, IOT_LOGLEVEL_NOTSET = 5  };

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
};

extern IotLogger logger;