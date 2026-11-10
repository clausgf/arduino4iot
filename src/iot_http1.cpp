/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#include "iot_http1.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>

namespace iot_http1
{

// *****************************************************************************
// Helpers
// *****************************************************************************

static bool equalsIgnoreCase(const std::string& a, const char* b)
{
    size_t i = 0;
    for (; i < a.size() && b[i] != '\0'; ++i)
    {
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) { return false; }
    }
    return i == a.size() && b[i] == '\0';
}

static bool equalsIgnoreCase(const std::string& a, const std::string& b)
{
    return a.size() == b.size() && equalsIgnoreCase(a, b.c_str());
}

static std::string trim(const std::string& text)
{
    const size_t first = text.find_first_not_of(" \t");
    if (first == std::string::npos) { return std::string(); }
    const size_t last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

/// @return true if the comma-separated @p list contains @p token.
static bool hasToken(const std::string& list, const char* token)
{
    size_t start = 0;
    while (start <= list.size())
    {
        size_t comma = list.find(',', start);
        if (comma == std::string::npos) { comma = list.size(); }
        if (equalsIgnoreCase(trim(list.substr(start, comma - start)), token)) { return true; }
        start = comma + 1;
    }
    return false;
}

/// @return true if all of @p text are decimal digits, in @p oValue then.
static bool parseDecimal(const std::string& text, uint64_t& oValue)
{
    if (text.empty() || text.size() > 18) { return false; }
    uint64_t value = 0;
    for (char c : text)
    {
        if (c < '0' || c > '9') { return false; }
        value = value * 10 + (uint64_t)(c - '0');
    }
    oValue = value;
    return true;
}

// *****************************************************************************
// URL and request
// *****************************************************************************

std::string Url::origin() const
{
    return std::string(https ? "https://" : "http://") + host + ":" + std::to_string(port);
}

std::string Url::hostHeader() const
{
    const bool ipv6 = host.find(':') != std::string::npos;
    std::string out = ipv6 ? "[" + host + "]" : host;
    if (port != (https ? 443 : 80))
    {
        out += ":" + std::to_string(port);
    }
    return out;
}

bool parseUrl(const std::string& url, Url& oUrl)
{
    oUrl = Url{};
    size_t start;
    if (url.compare(0, 8, "https://") == 0)
    {
        oUrl.https = true;
        oUrl.port = 443;
        start = 8;
    } else if (url.compare(0, 7, "http://") == 0)
    {
        oUrl.port = 80;
        start = 7;
    } else
    {
        return false;
    }

    const size_t end = url.find_first_of("/?#", start);
    const std::string authority = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (authority.find('@') != std::string::npos)
    {
        return false;  // credentials do not belong into a URL here
    }

    std::string portText;
    if (!authority.empty() && authority[0] == '[')
    {
        const size_t close = authority.find(']');
        if (close == std::string::npos) { return false; }
        oUrl.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size())
        {
            if (authority[close + 1] != ':') { return false; }
            portText = authority.substr(close + 2);
            if (portText.empty()) { return false; }
        }
    } else
    {
        const size_t colon = authority.find(':');
        oUrl.host = authority.substr(0, colon);
        if (colon != std::string::npos)
        {
            portText = authority.substr(colon + 1);
            if (portText.empty()) { return false; }
        }
    }
    if (oUrl.host.empty())
    {
        return false;
    }
    if (!portText.empty())
    {
        uint64_t port = 0;
        if (!parseDecimal(portText, port) || port == 0 || port > 65535) { return false; }
        oUrl.port = (uint16_t)port;
    }

    if (end == std::string::npos)
    {
        oUrl.target = "/";
    } else
    {
        const size_t hash = url.find('#', end);
        oUrl.target = url.substr(end, hash == std::string::npos ? std::string::npos : hash - end);
        if (oUrl.target.empty() || oUrl.target[0] != '/')
        {
            oUrl.target.insert(0, "/");
        }
    }
    return true;
}

bool isHeaderSafe(const std::string& text)
{
    return text.find_first_of(std::string("\r\n\0", 3)) == std::string::npos;
}

std::string requestHead(const char* method, const Url& url,
    const std::map<std::string, std::string>& headers, size_t bodyLength,
    const std::string& userAgent)
{
    bool hasHost = false;
    bool hasContentLength = false;
    bool hasUserAgent = false;
    for (auto const& kv : headers)
    {
        hasHost |= equalsIgnoreCase(kv.first, "Host");
        hasContentLength |= equalsIgnoreCase(kv.first, "Content-Length");
        hasUserAgent |= equalsIgnoreCase(kv.first, "User-Agent");
    }

    std::string head;
    head.reserve(256);
    head += method;
    head += ' ';
    head += url.target;
    head += " HTTP/1.1\r\n";
    if (!hasHost)
    {
        head += "Host: " + url.hostHeader() + "\r\n";
    }
    if (!hasUserAgent && !userAgent.empty())
    {
        head += "User-Agent: " + userAgent + "\r\n";
    }
    const std::string m(method);
    const bool usuallyHasBody = m == "POST" || m == "PUT" || m == "PATCH";
    if (!hasContentLength && (bodyLength > 0 || usuallyHasBody))
    {
        head += "Content-Length: " + std::to_string(bodyLength) + "\r\n";
    }
    for (auto const& kv : headers)
    {
        if (kv.second.empty()) { continue; }
        head += kv.first + ": " + kv.second + "\r\n";
    }
    head += "\r\n";
    return head;
}

// *****************************************************************************
// ResponseParser
// *****************************************************************************

void ResponseParser::begin(bool headRequest, const std::vector<std::string>& collect, std::string& body)
{
    *this = ResponseParser{};
    _headRequest = headRequest;
    _collect = &collect;
    _body = &body;
}

bool ResponseParser::keepAlive() const
{
    if (_state != State::Done || _connectionClose)
    {
        return false;
    }
    // HTTP/1.0 keeps the connection only when asked to
    return _minorVersion >= 1 || _connectionKeepAlive;
}

void ResponseParser::_fail(Error error)
{
    _state = State::Failed;
    _error = error;
}

bool ResponseParser::_appendBody(const char* data, size_t length)
{
    try
    {
        _body->append(data, length);
    } catch (const std::bad_alloc&)
    {
        _fail(Error::OutOfMemory);
        return false;
    }
    return true;
}

bool ResponseParser::_takeLine(const char*& data, size_t& length, bool& oComplete)
{
    oComplete = false;
    const char* lf = (const char*)memchr(data, '\n', length);
    const size_t take = lf != nullptr ? (size_t)(lf - data) + 1 : length;
    if (_line.size() + take > MAX_LINE_LENGTH)
    {
        _fail(Error::TooLarge);
        return false;
    }
    _line.append(data, take);
    data += take;
    length -= take;
    if (lf != nullptr)
    {
        _line.pop_back();  // LF
        if (!_line.empty() && _line.back() == '\r') { _line.pop_back(); }
        oComplete = true;
    }
    return true;
}

size_t ResponseParser::feed(const char* data, size_t length)
{
    const size_t total = length;
    while (length > 0 && _state != State::Done && _state != State::Failed)
    {
        switch (_state)
        {
            case State::StatusLine:
            case State::Headers:
            case State::ChunkSize:
            case State::ChunkDataEnd:
            case State::Trailer:
            {
                bool complete = false;
                if (!_takeLine(data, length, complete)) { break; }
                if (!complete) { break; }
                switch (_state)
                {
                    case State::StatusLine: _onStatusLine(); break;
                    case State::Headers:    _onHeaderLine(); break;
                    case State::ChunkSize:  _onChunkSizeLine(); break;
                    case State::ChunkDataEnd:
                        if (_line.empty()) { _state = State::ChunkSize; }
                        else { _fail(Error::Malformed); }
                        break;
                    case State::Trailer:
                        if (_line.empty()) { _state = State::Done; }
                        break;
                    default: break;
                }
                _line.clear();
                break;
            }
            case State::Body:
            case State::ChunkData:
            {
                const size_t take = (size_t)std::min<uint64_t>(_remaining, length);
                if (!_appendBody(data, take)) { break; }
                data += take;
                length -= take;
                _remaining -= take;
                if (_remaining == 0)
                {
                    _state = _state == State::Body ? State::Done : State::ChunkDataEnd;
                }
                break;
            }
            case State::BodyUntilClose:
                if (!_appendBody(data, length)) { break; }
                data += length;
                length = 0;
                break;
            case State::Done:
            case State::Failed:
                break;
        }
    }
    return total - length;
}

void ResponseParser::onClose()
{
    if (_state == State::BodyUntilClose)
    {
        _state = State::Done;
    } else if (_state != State::Done && _state != State::Failed)
    {
        _fail(Error::Truncated);
    }
}

void ResponseParser::_onStatusLine()
{
    // "HTTP/1.1 200 OK"; a stray empty line before it is tolerated
    if (_line.empty())
    {
        return;
    }
    if (_line.size() < 12 || _line.compare(0, 7, "HTTP/1.") != 0
        || !std::isdigit((unsigned char)_line[7]) || _line[8] != ' ')
    {
        _fail(Error::Malformed);
        return;
    }
    uint64_t status = 0;
    if (!parseDecimal(_line.substr(9, 3), status) || status < 100 || status > 999
        || (_line.size() > 12 && _line[12] != ' '))
    {
        _fail(Error::Malformed);
        return;
    }
    _minorVersion = _line[7] - '0';
    _status = (int)status;
    _state = State::Headers;
}

void ResponseParser::_onHeaderLine()
{
    if (_line.empty())
    {
        _onHeadComplete();
        return;
    }
    const size_t colon = _line.find(':');
    if (colon == std::string::npos || colon == 0
        || _line[0] == ' ' || _line[0] == '\t')  // no obsolete line folding
    {
        _fail(Error::Malformed);
        return;
    }
    const std::string name = _line.substr(0, colon);
    if (name.find_first_of(" \t") != std::string::npos)
    {
        _fail(Error::Malformed);
        return;
    }
    const std::string value = trim(_line.substr(colon + 1));

    if (equalsIgnoreCase(name, "Content-Length"))
    {
        uint64_t contentLength = 0;
        if (!parseDecimal(value, contentLength)
            || (_contentLength >= 0 && (uint64_t)_contentLength != contentLength))
        {
            _fail(Error::Malformed);
            return;
        }
        _contentLength = (int64_t)contentLength;
    } else if (equalsIgnoreCase(name, "Transfer-Encoding"))
    {
        // chunked has to be the last coding; nothing else is supported
        if (!equalsIgnoreCase(value, "chunked"))
        {
            _fail(equalsIgnoreCase(value, "identity") ? Error::Malformed : Error::Encoding);
            return;
        }
        _chunked = true;
    } else if (equalsIgnoreCase(name, "Connection"))
    {
        _connectionClose |= hasToken(value, "close");
        _connectionKeepAlive |= hasToken(value, "keep-alive");
    }

    if (_collect != nullptr)
    {
        for (auto const& key : *_collect)
        {
            if (equalsIgnoreCase(name, key))
            {
                // a repeated header is combined, as HTTP defines it
                auto it = _headers.find(key);
                if (it == _headers.end()) { _headers[key] = value; }
                else { it->second += ", " + value; }
            }
        }
    }
}

void ResponseParser::_onHeadComplete()
{
    if (_status >= 100 && _status < 200)
    {
        // an interim response (100 Continue, 103 Early Hints): the real one follows
        _status = 0;
        _contentLength = -1;
        _chunked = false;
        _connectionClose = false;
        _connectionKeepAlive = false;
        _headers.clear();
        _state = State::StatusLine;
        return;
    }
    _headComplete = true;
    if (_headRequest || _status == 204 || _status == 304)
    {
        _state = State::Done;
    } else if (_chunked)
    {
        _contentLength = -1;  // chunked framing wins over a Content-Length
        _state = State::ChunkSize;
    } else if (_contentLength >= 0)
    {
        _remaining = (uint64_t)_contentLength;
        _state = _remaining == 0 ? State::Done : State::Body;
        // at once: growing step by step would need the old and the doubled
        // buffer together, which a fragmented heap often cannot provide
        try
        {
            _body->reserve(_body->size() + (size_t)_remaining);
        } catch (const std::exception&)  // bad_alloc, or length_error beyond max_size()
        {
            _fail(Error::OutOfMemory);
        }
    } else
    {
        _state = State::BodyUntilClose;
    }
}

void ResponseParser::_onChunkSizeLine()
{
    // "1a2b;extension"
    const std::string sizeText = trim(_line.substr(0, _line.find(';')));
    if (sizeText.empty() || sizeText.size() > 15)
    {
        _fail(Error::Malformed);
        return;
    }
    uint64_t size = 0;
    for (char c : sizeText)
    {
        if (!std::isxdigit((unsigned char)c))
        {
            _fail(Error::Malformed);
            return;
        }
        size = size * 16 + (uint64_t)(std::isdigit((unsigned char)c) ? c - '0' : std::tolower((unsigned char)c) - 'a' + 10);
    }
    if (size == 0)
    {
        _state = State::Trailer;
        return;
    }
    _remaining = size;
    _state = State::ChunkData;
}

}  // namespace iot_http1
