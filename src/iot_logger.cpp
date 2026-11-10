/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#include <cstring>

#include "iot_api.h"
#include "iot_log.h"
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

/// Keep @p lines for a later cycle. The first lines a cycle keeps are led by a
/// marker naming the cycle, since a time stamp since boot alone cannot be dated or told
/// apart once several cycles share the buffer.
void IotLogger::_keepOffline(const std::string& lines)
{
#if IOT_RTC_LOG_SIZE > 0
    if (!_offlineMarked)
    {
        _offlineMarked = true;
        const std::string cycle = _offlineContext != nullptr ? _offlineContext() : std::string("unknown cycle");
        char marker[96];
        snprintf(marker, sizeof(marker), "I (%lu) iot: --- kept offline: %s ---\n",
            (unsigned long)iotLogTimestamp_ms(), cycle.c_str());
        rtcLog->append(marker);
    }
    rtcLog->append(lines);
#else
    (void)lines;
#endif
}

/// Put back what takeOffline() returned, unchanged.
void IotLogger::_restoreOffline(const std::string& lines)
{
#if IOT_RTC_LOG_SIZE > 0
    if (!lines.empty()) { rtcLog->append(lines); }
#else
    (void)lines;
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

/// @return true if a failed log request may succeed in a later cycle, so its
/// lines are worth keeping: no connection, a server-side failure, a 401 the
/// re-provisioning could not resolve this time, a timeout or rate limit, or
/// a device not yet configured. Any other 4xx would only fail again.
static bool worthRetrying(const IotResult& result)
{
    if (result.isOk()) { return false; }
    if (result.isTransportError()) { return true; }
    const int status = result.httpStatus;
    return (status >= 500 && status < 600) || status == 401 || status == 408 || status == 429
        || status == IotResult::STATUS_NOT_CONFIGURED;
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
    int headerChars = snprintf(logBuf, logBufLen, "%c (%lu) %s: ", logLevelChar, (unsigned long)iotLogTimestamp_ms(), tag);
    if (headerChars < 0 || (size_t)headerChars >= logBufLen) { return; }
    vsnprintf(logBuf + headerChars, logBufLen - headerChars, format, ap);

    // one entry is one line: drop trailing line breaks, flatten inner ones
    char* message = logBuf + headerChars;
    size_t length = strlen(message);
    while (length > 0 && (message[length - 1] == '\n' || message[length - 1] == '\r'))
    {
        message[--length] = '\0';
    }
    for (char* p = message; *p != '\0'; ++p)
    {
        if (*p == '\n' || *p == '\r') { *p = ' '; }
    }
}

void IotLogger::logToRtc(Level level, const char *tag, const char* format...)
{
    if (level > _level) { return; }
    char logBuf[160];
    va_list args;
    va_start(args, format);
    formatLine(logBuf, sizeof(logBuf), level, tag, format, args);
    va_end(args);
    _keepOffline(std::string(logBuf) + '\n');
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
            const std::string line = std::string(logBuf) + '\n';
            if (worthRetrying(postLog(line))) { _keepOffline(line); }
        } else {
            _keepOffline(std::string(logBuf) + '\n');
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
    // before iot.begin() has initialized the API - e.g. on the early exit
    // paths - a request could only fail
    return api.isInitialized() && (_network == nullptr || _network->isConnected());
}

IotResult IotLogger::flush()
{
    if (!_isOnline())
    {
        if (!_logBuffer.empty())
        {
            log_i("IotLogger::flush: offline, keeping %u log bytes in RTC RAM",
                (unsigned)_logBuffer.length());
            _keepOffline(_logBuffer);
            _logBuffer.clear();
        }
        return IotResult();  // transport error: nothing could be sent
    }

    // what earlier cycles could not send goes first, in order
    const std::string earlier = takeOffline();
    const std::string current = _logBuffer;
    _logBuffer.clear();
    if (earlier.empty() && current.empty())
    {
        return IotResult(200);
    }

    size_t sent = 0;
    IotResult result = postLog(earlier + current, &sent);
    if (worthRetrying(result))
    {
        // only what the server has not accepted yet, so a chunk that went
        // through is not sent twice. The earlier lines go back as they were,
        // with their own markers; this cycle's get one.
        if (sent < earlier.size())
        {
            _restoreOffline(earlier.substr(sent));
            sent = 0;
        } else {
            sent -= earlier.size();
        }
        if (sent < current.size()) { _keepOffline(current.substr(sent)); }
    }
    return result;
}

// *****************************************************************************

IotResult IotLogger::postLog(const std::string& body, size_t* oSent, const char * apiPath)
{
    // the server rejects bodies larger than its max_log_size (default 8 KiB)
    // with HTTP 413, so split large bodies into chunks at line boundaries
    IotResult result(200);

    size_t offset = 0;
    while (offset < body.size())
    {
        size_t chunkLen = iot_text::nextLogChunkLength(body.c_str(), body.size(), offset, IOT_MAX_TELEMETRY_SIZE);
        result = api.post(apiPath, body.substr(offset, chunkLen),
            { .headers = { { "Content-Type", "text/plain" } } }).result;
        if (!result.isOk()) { break; }
        offset += chunkLen;
    }
    if (oSent != nullptr) { *oSent = offset; }
    return result;
}

// *****************************************************************************
