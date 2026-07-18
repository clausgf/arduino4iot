/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#include "Arduino.h"
#include <WiFi.h>

#include "iot_api.h"
#include "iot_logger.h"
#include "iot_telemetry.h"
#include "iot_text.h"

// *****************************************************************************

IotLogger logger;


// *****************************************************************************
// Logging
// *****************************************************************************

IotLogger::IotLogger()
{
    _logLevel = LogLevel::IOT_LOGLEVEL_NOTSET;
    _buffered = true;
}

void IotLogger::begin(LogLevel logLevel)
{
    setLogLevel(logLevel);
}

void IotLogger::end()
{
    flush();
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
        if (_buffered)
        {
            // flush before the buffer would exceed the server's size limit
            size_t lineLen = strlen(logBuf) + 1; // + newline
            if (_logBuffer.length() + lineLen > IOT_MAX_TELEMETRY_SIZE)
            {
                if (flush() <= 0)
                {
                    // sending failed (e.g. no WiFi): drop the buffer to bound
                    // memory usage - remote logs are best-effort
                    _logBuffer = "";
                }
            }
            _logBuffer += logBuf;
            _logBuffer += '\n';
        } else if (WiFi.status() == WL_CONNECTED) {
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

void IotLogger::setBuffered(bool enabled)
{
    _buffered = enabled;
}

int IotLogger::flush()
{
    if (_logBuffer.isEmpty())
    {
        return 0;
    }
    if (WiFi.status() != WL_CONNECTED)
    {
        log_w("IotLogger::flush: WiFi not connected, keeping %u buffered log bytes",
            _logBuffer.length());
        return 0;
    }
    int httpStatusCode = postLog(_logBuffer.c_str());
    _logBuffer = "";
    return httpStatusCode;
}

// *****************************************************************************

int IotLogger::postLog(const char * body, const char * apiPath)
{
    // the server rejects bodies larger than its max_log_size (default 8 KiB)
    // with HTTP 413, so split large bodies into chunks at line boundaries
    size_t bodyLen = strlen(body);
    int httpStatusCode = 0;

    size_t offset = 0;
    do
    {
        size_t chunkLen = iot_text::nextLogChunkLength(body, bodyLen, offset, IOT_MAX_TELEMETRY_SIZE);
        String chunk;
        chunk.concat(body + offset, chunkLen);
        String oResult = "";
        httpStatusCode = api.apiPost(oResult, apiPath, chunk, {{"Content-Type", "text/plain"}});
        offset += chunkLen;
    } while (offset < bodyLen);

    return httpStatusCode;
}

// *****************************************************************************
