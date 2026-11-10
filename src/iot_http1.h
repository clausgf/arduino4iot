/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// *****************************************************************************
// HTTP/1.1 framing - Arduino-free, so the parsing is tested on the host.
//
// IotIdfHttp moves the bytes; what they mean is decided here: the request
// head, and an incremental response parser that takes whatever the socket
// delivered, however it is split.
// *****************************************************************************

namespace iot_http1
{

struct Url
{
    bool https = false;
    std::string host;      ///< without brackets for an IPv6 literal
    uint16_t port = 0;     ///< the scheme's default if the URL names none
    std::string target;    ///< path and query, at least "/"

    /// "https://host:port", which identifies a connection that can be reused.
    std::string origin() const;
    /// The Host header: the host, with the port unless it is the default.
    std::string hostHeader() const;
};

/// Split an http:// or https:// URL. @return false for another scheme, an
/// empty host, a bad port or a URL with user info.
bool parseUrl(const std::string& url, Url& oUrl);

/// @return true if @p text can go into a header name or value: no CR, LF
/// or NUL, which would let it end the header and inject another.
bool isHeaderSafe(const std::string& text);

/**
 * The request line and headers, ending with the empty line. Adds Host, and
 * Content-Length when there is a body or the method usually has one (POST,
 * PUT, PATCH), unless @p headers sets them; entries with an empty value are
 * skipped, which suppresses a default.
 */
std::string requestHead(const char* method, const Url& url,
    const std::map<std::string, std::string>& headers, size_t bodyLength,
    const std::string& userAgent);

/**
 * Incremental HTTP/1.1 response parser.
 *
 * feed() takes the bytes as they arrive; the parser reports when the head is
 * complete and when the response is. Bodies are framed by Content-Length, by
 * chunked transfer coding, or - without either - by the end of the
 * connection, which onClose() reports. Interim 1xx responses are skipped.
 */
class ResponseParser
{
public:
    enum class Error : uint8_t
    {
        None,
        Malformed,    ///< not an HTTP/1.x response, or a broken frame
        TooLarge,     ///< a status, header or chunk-size line over MAX_LINE_LENGTH
        Encoding,     ///< a transfer coding other than chunked
        OutOfMemory,  ///< the body did not fit
        Truncated,    ///< the connection ended before the response did
    };

    static constexpr size_t MAX_LINE_LENGTH = 4096;

    /**
     * Start a response.
     * @param headRequest the request was a HEAD: the response has no body
     * @param collect     header names to keep (case-insensitive); the
     *                    headers map uses these spellings as keys
     * @param body        receives the decoded body
     */
    void begin(bool headRequest, const std::vector<std::string>& collect, std::string& body);

    /// Parse @p length bytes. @return the number consumed: all of them, or
    /// fewer once the response is complete or the parser failed.
    size_t feed(const char* data, size_t length);

    /// The connection ended: completes a body framed by the connection end,
    /// fails everything else that is not complete yet.
    void onClose();

    bool headComplete() const { return _headComplete; }
    bool done() const { return _state == State::Done; }
    bool failed() const { return _state == State::Failed; }
    Error error() const { return _error; }

    int status() const { return _status; }
    const std::map<std::string, std::string>& headers() const { return _headers; }
    /// The Content-Length, -1 if the response has none.
    int64_t contentLength() const { return _contentLength; }
    bool chunked() const { return _chunked; }
    /// Whether the connection can carry another request after this response.
    bool keepAlive() const;

private:
    enum class State : uint8_t
    {
        StatusLine, Headers, Body, BodyUntilClose,
        ChunkSize, ChunkData, ChunkDataEnd, Trailer, Done, Failed,
    };

    State _state = State::StatusLine;
    Error _error = Error::None;
    bool _headRequest = false;
    bool _headComplete = false;
    const std::vector<std::string>* _collect = nullptr;
    std::string* _body = nullptr;
    std::string _line;
    int _status = 0;
    int _minorVersion = 1;
    bool _connectionClose = false;
    bool _connectionKeepAlive = false;
    bool _chunked = false;
    int64_t _contentLength = -1;
    uint64_t _remaining = 0;   ///< of the body or the current chunk
    std::map<std::string, std::string> _headers;

    void _fail(Error error);
    bool _appendBody(const char* data, size_t length);
    /// @return false if the line got too long; true with @p oComplete once a
    /// line ended (its CR LF removed).
    bool _takeLine(const char*& data, size_t& length, bool& oComplete);
    void _onStatusLine();
    void _onHeaderLine();
    void _onHeadComplete();
    void _onChunkSizeLine();
};

}  // namespace iot_http1
