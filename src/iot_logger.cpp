/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#include "Arduino.h"

#include "iot_api.h"
#include "iot_logger.h"
#include "iot_rtc_log.h"
#include "iot_rtc_slot.h"
#include "iot_telemetry.h"
#include "iot_text.h"

// *****************************************************************************

IotLogger logger;

#if IOT_RTC_LOG_SIZE > 0
/// What could not be sent, kept across deep sleep and software resets for the
/// next cycle that has connectivity.
static RTC_NOINIT_ATTR IotRtcSlot<IotRtcLog<IOT_RTC_LOG_SIZE>> rtcLog;
#endif

/// Keep @p lines for a later cycle; @return false if there is no RTC buffer.
static bool keepOffline(const std::string& lines)
{
#if IOT_RTC_LOG_SIZE > 0
    rtcLog->append(lines);
    return true;
#else
    (void)lines;
    return false;
#endif
}

/// @return the lines kept by earlier cycles, emptying the RTC buffer.
static std::string takeOffline()
{
#if IOT_RTC_LOG_SIZE > 0
    return rtcLog->isEmpty() ? std::string() : rtcLog->take();
#else
    return std::string();
#endif
}


// *****************************************************************************
// Logging
// *****************************************************************************

IotLogger::IotLogger()
{
    _level = Level::All;
    _buffered = true;
}

void IotLogger::end()
{
    flush();
}

// *****************************************************************************

/// One log line in the ESP log format, "E (1234) tag: message", without newline.
static void formatLine(char* logBuf, size_t logBufLen, IotLogger::Level level,
    const char* tag, const char* format, va_list ap)
{
    const char * ESP_LOG_LEVEL_CHARS = "EWIDV";
    int logLevelIndex = (int)level;
    char logLevelChar = '?';
    if (logLevelIndex >= 0 && logLevelIndex < (int)strlen(ESP_LOG_LEVEL_CHARS))
    {
        logLevelChar = ESP_LOG_LEVEL_CHARS[logLevelIndex];
    }
    int headerChars = snprintf(logBuf, logBufLen, "%c (%lu) %s: ", logLevelChar, millis(), tag);
    if (headerChars < 0 || (size_t)headerChars >= logBufLen) { return; }
    vsnprintf(logBuf + headerChars, logBufLen - headerChars, format, ap);
}

void IotLogger::logToRtc(Level level, const char *tag, const char* format...)
{
    if (level > _level) { return; }
    char logBuf[160];
    va_list args;
    va_start(args, format);
    formatLine(logBuf, sizeof(logBuf), level, tag, format, args);
    va_end(args);
    keepOffline(std::string(logBuf) + '\n');
}

void IotLogger::logv(Level level, const char *tag, const char* format, va_list ap)
{
    // TODO logging is not reentrant - do we need to change this?
    if (level <= _level)
    {
        const int logBufLen = 160;
        char logBuf[logBufLen];
        formatLine(logBuf, logBufLen, level, tag, format, ap);

        // actual log output
        log_v("Logging level=%d tag=%s msg=\"%s\"", level, tag, logBuf);
        if (_buffered)
        {
            // flush before the buffer would exceed the server's size limit;
            // offline, flush() moves it to RTC RAM, so memory stays bounded
            size_t lineLen = strlen(logBuf) + 1; // + newline
            if (_logBuffer.length() + lineLen > IOT_MAX_TELEMETRY_SIZE)
            {
                flush();
            }
            _logBuffer += logBuf;
            _logBuffer += '\n';
        } else if (_isOnline()) {
            postLog(logBuf);
        } else {
            keepOffline(std::string(logBuf) + '\n');
        }
    }
}

// *****************************************************************************

void IotLogger::log(Level level, const char *tag, const char* format...)
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
    logv(Level::Error, tag, format, args);
    va_end(args);
}

void IotLogger::warn(const char *tag, const char* format...)
{
    va_list args;
    va_start(args, format);
    logv(Level::Warning, tag, format, args);
    va_end(args);
}

void IotLogger::info(const char *tag, const char* format...)
{
    va_list args;
    va_start(args, format);
    logv(Level::Info, tag, format, args);
    va_end(args);
}

void IotLogger::debug(const char *tag, const char* format...)
{
    va_list args;
    va_start(args, format);
    logv(Level::Debug, tag, format, args);
    va_end(args);
}

void IotLogger::verbose(const char *tag, const char* format...)
{
    va_list args;
    va_start(args, format);
    logv(Level::Verbose, tag, format, args);
    va_end(args);
}

// *****************************************************************************

void IotLogger::setBuffered(bool enabled)
{
    _buffered = enabled;
}

bool IotLogger::_isOnline() const
{
    return _network == nullptr || _network->isConnected();
}

IotResult IotLogger::flush()
{
    if (!_isOnline())
    {
        if (!_logBuffer.empty())
        {
            log_i("IotLogger::flush: offline, keeping %u log bytes in RTC RAM",
                (unsigned)_logBuffer.length());
            keepOffline(_logBuffer);
            _logBuffer.clear();
        }
        return IotResult();  // transport error: nothing could be sent
    }

    // what earlier cycles could not send goes first, in order
    std::string body = takeOffline();
    body += _logBuffer;
    _logBuffer.clear();
    if (body.empty())
    {
        return IotResult(200);
    }

    IotResult result = postLog(body.c_str());
    if (result.isTransportError() || result.httpStatus >= 500)
    {
        // worth another try next cycle; a 4xx would only fail again
        keepOffline(body);
    }
    return result;
}

// *****************************************************************************

IotResult IotLogger::postLog(const char * body, const char * apiPath)
{
    // the server rejects bodies larger than its max_log_size (default 8 KiB)
    // with HTTP 413, so split large bodies into chunks at line boundaries
    size_t bodyLen = strlen(body);
    IotResult result(200);

    size_t offset = 0;
    do
    {
        size_t chunkLen = iot_text::nextLogChunkLength(body, bodyLen, offset, IOT_MAX_TELEMETRY_SIZE);
        std::string chunk(body + offset, chunkLen);
        result = api.post(apiPath, chunk, { .headers = { { "Content-Type", "text/plain" } } }).result;
        offset += chunkLen;
    } while (offset < bodyLen && result.isOk());

    return result;
}

// *****************************************************************************
