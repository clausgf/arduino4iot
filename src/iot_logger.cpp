/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#include "iot_logger.h"

#include "iot_api.h"

// *****************************************************************************

IotLogger logger;


// *****************************************************************************
// Logging
// *****************************************************************************

IotLogger::IotLogger()
{
    _logLevel = LogLevel::IOT_LOGLEVEL_NOTSET;
}

void IotLogger::begin()
{
}

void IotLogger::end()
{
}

// *****************************************************************************

void IotLogger::setLogLevel(LogLevel level)
{
    _logLevel = level;
}

void IotLogger::logv(LogLevel level, const char *tag, const char* format, va_list ap)
{
    // TODO logging is not reentrant - do we need to change this?
    if (level <= _logLevel)
    {
        const char * ESP_LOG_LEVEL_CHARS = "_EWIDV";
        const int logBufLen = 160;
        char logBuf[logBufLen];

        // print header
        int logLevelIndex = int(level);
        char logLevelChar = '?';
        if (logLevelIndex < strlen(ESP_LOG_LEVEL_CHARS))
        {
            logLevelChar = ESP_LOG_LEVEL_CHARS[logLevelIndex];
        }
        int headerChars = snprintf(logBuf, logBufLen, "%c (%lu) %s: ", logLevelChar, millis(), tag);

        // print the message itself
        vsnprintf(logBuf + headerChars, logBufLen - headerChars, format, ap);

        // actual log output
        log_printf(logBuf);
        if (WiFi.status() == WL_CONNECTED) {
            postLog(logBuf);
        }
    }
}

// *****************************************************************************

void IotLogger::logf(LogLevel level, const char *tag, const char* format...)
{
    va_list args;
    va_start(args, format);
    logv(level, tag, format, args);
    va_end(args);
}

void IotLogger::error(const char *tag, const char* format...)
{
    va_list args;
    va_start(args, format);
    logv(LogLevel::IOT_LOGLEVEL_ERROR, tag, format, args);
    va_end(args);
}

void IotLogger::warn(const char *tag, const char* format...)
{
    va_list args;
    va_start(args, format);
    logv(LogLevel::IOT_LOGLEVEL_WARNING, tag, format, args);
    va_end(args);
}

void IotLogger::info(const char *tag, const char* format...)
{
    va_list args;
    va_start(args, format);
    logv(LogLevel::IOT_LOGLEVEL_INFO, tag, format, args);
    va_end(args);
}

void IotLogger::debug(const char *tag, const char* format...)
{
    va_list args;
    va_start(args, format);
    logv(LogLevel::IOT_LOGLEVEL_DEBUG, tag, format, args);
    va_end(args);
}

void IotLogger::verbose(const char *tag, const char* format...)
{
    va_list args;
    va_start(args, format);
    logv(LogLevel::IOT_LOGLEVEL_VERBOSE, tag, format, args);
    va_end(args);
}

// *****************************************************************************

int IotLogger::postLog(const char * body, const char * apiPath)
{
    String oResult = "";
    return api.apiPost(oResult, apiPath, body, {{"Content-Type", "text/plain"}});
}

// *****************************************************************************
