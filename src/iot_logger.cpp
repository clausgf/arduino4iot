/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#include "Arduino.h"
#include <WiFi.h>

#include "iot_api.h"
#include "iot_logger.h"
#include "iot_telemetry.h"

// *****************************************************************************

IotLogger logger;


// *****************************************************************************
// Logging
// *****************************************************************************

IotLogger::IotLogger()
{
    _logLevel = LogLevel::IOT_LOGLEVEL_NOTSET;
}

void IotLogger::begin(LogLevel logLevel)
{
    setLogLevel(logLevel);
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
        const char * ESP_LOG_LEVEL_CHARS = "EWIDV";
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
        log_i("Logging level=%d tag=%s msg=\"%s\"", level, tag, logBuf);
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
    // the server rejects bodies larger than its max_log_size (default 8 KiB)
    // with HTTP 413, so split large bodies into chunks at line boundaries
    const size_t maxChunkSize = IOT_MAX_TELEMETRY_SIZE;
    size_t bodyLen = strlen(body);
    int httpStatusCode = 0;

    size_t offset = 0;
    do
    {
        size_t chunkLen = bodyLen - offset;
        if (chunkLen > maxChunkSize)
        {
            // prefer splitting after the last newline within the limit
            chunkLen = maxChunkSize;
            for (size_t i = maxChunkSize; i > 0; i--)
            {
                if (body[offset + i - 1] == '\n')
                {
                    chunkLen = i;
                    break;
                }
            }
        }
        String chunk;
        chunk.concat(body + offset, chunkLen);
        String oResult = "";
        httpStatusCode = api.apiPost(oResult, apiPath, chunk, {{"Content-Type", "text/plain"}});
        offset += chunkLen;
    } while (offset < bodyLen);

    return httpStatusCode;
}

// *****************************************************************************
