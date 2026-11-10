/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#include "iot_idf_http.h"

#include "iot.h"          // IOT_VERSION_* for the User-Agent
#include "iot_http1.h"

#include <cerrno>
#include <cstring>
#include <new>
#include <strings.h>

#include <Arduino.h>
#include <esp_crt_bundle.h>
#include <esp_task_wdt.h>
#include <fcntl.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <unistd.h>

// *****************************************************************************
// Helpers
// *****************************************************************************

#define IOT_STRINGIFY_(x) #x
#define IOT_STRINGIFY(x) IOT_STRINGIFY_(x)
static const char* USER_AGENT = "arduino4iot/" IOT_STRINGIFY(IOT_VERSION_MAJOR) "."
    IOT_STRINGIFY(IOT_VERSION_MINOR) "." IOT_STRINGIFY(IOT_VERSION_PATCH);

/// A body up to this size goes out with the head in one write: two small
/// segments would meet Nagle's algorithm and the server's delayed ACK.
static constexpr size_t COALESCE_BODY_LENGTH = 8 * 1024;

static void feedWatchdog()
{
    // only if this task is supervised: a reset otherwise logs an error
    if (esp_task_wdt_status(nullptr) == ESP_OK)
    {
        esp_task_wdt_reset();
    }
}

static bool expired(unsigned long deadline_ms)
{
    return (long)(millis() - deadline_ms) >= 0;
}

/// The earlier of two points in time on the wrapping millis() clock.
static unsigned long earliest(unsigned long a_ms, unsigned long b_ms)
{
    return (long)(a_ms - b_ms) <= 0 ? a_ms : b_ms;
}

static std::string mbedtlsText(int error)
{
    char text[100];
    mbedtls_strerror(error, text, sizeof(text));
    return std::string(text);
}

// *****************************************************************************
// TLS state
// *****************************************************************************

struct IotIdfHttp::_Tls
{
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context entropy;
    mbedtls_x509_crt caCert;
    mbedtls_x509_crt clientCert;
    mbedtls_pk_context clientKey;

    explicit _Tls(int fd)
    {
        mbedtls_net_init(&net);
        net.fd = fd;  // owned by IotIdfHttp, never freed through net
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&conf);
        mbedtls_ctr_drbg_init(&drbg);
        mbedtls_entropy_init(&entropy);
        mbedtls_x509_crt_init(&caCert);
        mbedtls_x509_crt_init(&clientCert);
        mbedtls_pk_init(&clientKey);
    }

    ~_Tls()
    {
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&entropy);
        mbedtls_x509_crt_free(&caCert);
        mbedtls_x509_crt_free(&clientCert);
        mbedtls_pk_free(&clientKey);
    }

    /// Configure for @p tls and @p host. @return 0 or an mbedTLS error.
    int setup(const IotTlsConfig& tls, const std::string& host)
    {
        static const char* PERSONALIZATION = "arduino4iot";
        int ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
            (const unsigned char*)PERSONALIZATION, strlen(PERSONALIZATION));
        if (ret != 0) { return ret; }
        ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
            MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
        if (ret != 0) { return ret; }
        mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);

        switch (tls.mode)
        {
            case IotTlsMode::Bundle:
                mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
                if (esp_crt_bundle_attach(&conf) != ESP_OK)
                {
                    return MBEDTLS_ERR_X509_FATAL_ERROR;
                }
                break;
            case IotTlsMode::Insecure:
                mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
                break;
            case IotTlsMode::CaPin:
                mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
                ret = mbedtls_x509_crt_parse(&caCert, (const unsigned char*)tls.caCertPem.c_str(),
                    tls.caCertPem.size() + 1);
                if (ret != 0) { return ret; }
                mbedtls_ssl_conf_ca_chain(&conf, &caCert, nullptr);
                if (!tls.clientCertPem.empty() && !tls.clientKeyPem.empty())
                {
                    ret = mbedtls_x509_crt_parse(&clientCert,
                        (const unsigned char*)tls.clientCertPem.c_str(), tls.clientCertPem.size() + 1);
                    if (ret != 0) { return ret; }
                    ret = mbedtls_pk_parse_key(&clientKey,
                        (const unsigned char*)tls.clientKeyPem.c_str(), tls.clientKeyPem.size() + 1,
                        nullptr, 0, mbedtls_ctr_drbg_random, &drbg);
                    if (ret != 0) { return ret; }
                    ret = mbedtls_ssl_conf_own_cert(&conf, &clientCert, &clientKey);
                    if (ret != 0) { return ret; }
                }
                break;
            case IotTlsMode::None:
                return MBEDTLS_ERR_SSL_BAD_CONFIG;
        }

        ret = mbedtls_ssl_setup(&ssl, &conf);
        if (ret != 0) { return ret; }
        // SNI, and the name the certificate has to carry
        ret = mbedtls_ssl_set_hostname(&ssl, host.c_str());
        if (ret != 0) { return ret; }
        // net_send/net_recv see the non-blocking socket and answer WANT_READ/WANT_WRITE
        mbedtls_ssl_set_bio(&ssl, &net, mbedtls_net_send, mbedtls_net_recv, nullptr);
        return 0;
    }
};

// *****************************************************************************
// Configuration
// *****************************************************************************

IotIdfHttp::~IotIdfHttp()
{
    close();
}

bool IotIdfHttp::hasServerTrust() const
{
    if (_tlsConfig == nullptr)
    {
        return false;
    }
    switch (_tlsConfig->mode)
    {
        case IotTlsMode::Bundle:
        case IotTlsMode::Insecure:
            return true;
        case IotTlsMode::CaPin:
            return !_tlsConfig->caCertPem.empty();
        case IotTlsMode::None:
            break;
    }
    return false;
}

void IotIdfHttp::setConnectTimeout_ms(uint32_t timeout_ms)
{
    if (timeout_ms > 0) { _connectTimeout_ms = timeout_ms; }
}

void IotIdfHttp::setRequestTimeout_ms(uint32_t timeout_ms)
{
    if (timeout_ms > 0) { _requestTimeout_ms = timeout_ms; }
}

void IotIdfHttp::setHandshakeTimeout_ms(uint32_t timeout_ms)
{
    if (timeout_ms > 0) { _handshakeTimeout_ms = timeout_ms; }
}

void IotIdfHttp::setResponseTimeout_ms(uint32_t timeout_ms)
{
    if (timeout_ms > 0) { _responseTimeout_ms = timeout_ms; }
}

// *****************************************************************************
// Connection
// *****************************************************************************

void IotIdfHttp::close()
{
    if (_tls != nullptr)
    {
        // a courtesy: the server learns the connection ends on purpose
        mbedtls_ssl_close_notify(&_tls->ssl);
        delete _tls;
        _tls = nullptr;
    }
    if (_fd >= 0)
    {
        ::close(_fd);
        _fd = -1;
    }
    _origin.clear();
}

bool IotIdfHttp::_wait(bool forWrite, unsigned long until_ms) const
{
    const long remaining_ms = (long)(until_ms - millis());
    if (remaining_ms <= 0)
    {
        return false;
    }
    fd_set set;
    FD_ZERO(&set);
    FD_SET(_fd, &set);
    struct timeval tv;
    tv.tv_sec = remaining_ms / 1000;
    tv.tv_usec = (remaining_ms % 1000) * 1000;
    const int ready = select(_fd + 1, forWrite ? nullptr : &set, forWrite ? &set : nullptr, nullptr, &tv);
    // an error (the socket broke) counts as ready: the next call reports it
    return ready != 0;
}

bool IotIdfHttp::_usable() const
{
    if (_fd < 0)
    {
        return false;
    }
    if (_tls != nullptr && mbedtls_ssl_get_bytes_avail(&_tls->ssl) > 0)
    {
        return false;  // data nobody asked for
    }
    // an idle connection has nothing to read; something to read is the
    // server's FIN, a TLS alert or garbage - the connection is done for
    fd_set set;
    FD_ZERO(&set);
    FD_SET(_fd, &set);
    struct timeval tv = { 0, 0 };
    return select(_fd + 1, &set, nullptr, nullptr, &tv) == 0;
}

int IotIdfHttp::_connect(const iot_http1::Url& url, IotHttpResponse& oResponse, unsigned long deadline_ms)
{
    close();

    // DNS - blocking, bounded by lwIP's resolver
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* address = nullptr;
    const std::string port = std::to_string(url.port);
    const int resolved = getaddrinfo(url.host.c_str(), port.c_str(), &hints, &address);
    if (resolved != 0 || address == nullptr)
    {
        log_e("IotIdfHttp: cannot resolve '%s' (%d)", url.host.c_str(), resolved);
        if (address != nullptr) { freeaddrinfo(address); }
        oResponse.phase = IotTransportPhase::Dns;
        return ERROR_CONNECT;
    }

    // TCP, non-blocking from here on
    oResponse.phase = IotTransportPhase::Connect;
    _fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (_fd < 0)
    {
        log_e("IotIdfHttp: no socket (errno %d)", errno);
        freeaddrinfo(address);
        return ERROR_CONNECT;
    }
    fcntl(_fd, F_SETFL, fcntl(_fd, F_GETFL, 0) | O_NONBLOCK);
    const int noDelay = 1;
    setsockopt(_fd, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));

    const int connected = ::connect(_fd, address->ai_addr, address->ai_addrlen);
    const int connectErrno = errno;
    freeaddrinfo(address);
    if (connected != 0 && connectErrno != EINPROGRESS)
    {
        log_e("IotIdfHttp: connect to %s:%u failed (errno %d)", url.host.c_str(), url.port, connectErrno);
        close();
        return ERROR_CONNECT;
    }
    if (connected != 0)
    {
        const unsigned long connectUntil_ms = earliest(millis() + _connectTimeout_ms, deadline_ms);
        if (!_wait(true, connectUntil_ms))
        {
            log_e("IotIdfHttp: connect to %s:%u timed out", url.host.c_str(), url.port);
            close();
            return ERROR_CONNECT;
        }
        int socketError = 0;
        socklen_t length = sizeof(socketError);
        if (getsockopt(_fd, SOL_SOCKET, SO_ERROR, &socketError, &length) != 0 || socketError != 0)
        {
            log_e("IotIdfHttp: connect to %s:%u failed (errno %d)", url.host.c_str(), url.port, socketError);
            close();
            return ERROR_CONNECT;
        }
    }

    if (url.https)
    {
        const int status = _handshake(url, oResponse, deadline_ms);
        if (status != 0)
        {
            close();
            return status;
        }
    }
    _origin = url.origin();
    oResponse.phase = IotTransportPhase::None;
    return 0;
}

int IotIdfHttp::_handshake(const iot_http1::Url& url, IotHttpResponse& oResponse, unsigned long deadline_ms)
{
    oResponse.phase = IotTransportPhase::Tls;
    _tls = new (std::nothrow) _Tls(_fd);
    if (_tls == nullptr)
    {
        oResponse.phase = IotTransportPhase::None;
        return ERROR_OUT_OF_MEMORY;
    }
    if (_tlsConfig->mode == IotTlsMode::Insecure)
    {
        log_w("TLS server authentication is DISABLED (tlsMode Insecure) - the "
              "connection is encrypted but the server identity is not verified. "
              "Do not use in production; seed a CA certificate (CaPin) instead.");
    }
    int ret = _tls->setup(*_tlsConfig, url.host);
    if (ret != 0)
    {
        log_e("IotIdfHttp: TLS setup failed: -0x%04X %s", (unsigned)-ret, mbedtlsText(ret).c_str());
        oResponse.tlsError = ret;
        if (ret == MBEDTLS_ERR_SSL_ALLOC_FAILED) { oResponse.phase = IotTransportPhase::None; }
        return ret == MBEDTLS_ERR_SSL_ALLOC_FAILED ? ERROR_OUT_OF_MEMORY : ERROR_CONNECT;
    }

    const unsigned long handshakeUntil_ms = earliest(millis() + _handshakeTimeout_ms, deadline_ms);
    while ((ret = mbedtls_ssl_handshake(&_tls->ssl)) != 0)
    {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            log_e("IotIdfHttp: TLS handshake with %s failed: -0x%04X %s",
                url.host.c_str(), (unsigned)-ret, mbedtlsText(ret).c_str());
            const uint32_t flags = mbedtls_ssl_get_verify_result(&_tls->ssl);
            if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED && flags != 0 && flags != (uint32_t)-1)
            {
                char info[256];
                mbedtls_x509_crt_verify_info(info, sizeof(info), "  ", flags);
                log_e("IotIdfHttp: certificate:\n%s", info);
            }
            oResponse.tlsError = ret;
            if (ret == MBEDTLS_ERR_SSL_ALLOC_FAILED) { oResponse.phase = IotTransportPhase::None; }
            return ret == MBEDTLS_ERR_SSL_ALLOC_FAILED ? ERROR_OUT_OF_MEMORY : ERROR_CONNECT;
        }
        if (!_wait(ret == MBEDTLS_ERR_SSL_WANT_WRITE, handshakeUntil_ms) || expired(handshakeUntil_ms))
        {
            log_e("IotIdfHttp: TLS handshake with %s timed out", url.host.c_str());
            return ERROR_CONNECT;  // phase Tls without a code: a timeout
        }
    }
    return 0;
}

// *****************************************************************************
// I/O
// *****************************************************************************

int IotIdfHttp::_write(const char* data, size_t length, unsigned long deadline_ms)
{
    unsigned long idleUntil_ms = millis() + _requestTimeout_ms;
    while (length > 0)
    {
        int written;
        bool wantWrite = true;
        if (_tls != nullptr)
        {
            written = mbedtls_ssl_write(&_tls->ssl, (const unsigned char*)data, length);
            if (written == MBEDTLS_ERR_SSL_WANT_READ || written == MBEDTLS_ERR_SSL_WANT_WRITE)
            {
                wantWrite = written == MBEDTLS_ERR_SSL_WANT_WRITE;
                written = 0;
            } else if (written < 0)
            {
                log_e("IotIdfHttp: TLS write failed: -0x%04X %s", (unsigned)-written, mbedtlsText(written).c_str());
                _lastTlsError = written;
                return ERROR_CONNECTION_LOST;
            }
        } else
        {
            written = ::send(_fd, data, length, MSG_DONTWAIT);
            if (written < 0)
            {
                if (errno != EAGAIN && errno != EWOULDBLOCK)
                {
                    log_e("IotIdfHttp: write failed (errno %d)", errno);
                    return ERROR_CONNECTION_LOST;
                }
                written = 0;
            }
        }
        if (written > 0)
        {
            data += written;
            length -= (size_t)written;
            idleUntil_ms = millis() + _requestTimeout_ms;
            feedWatchdog();  // progress, e.g. of a large upload
            continue;
        }
        if (expired(deadline_ms))
        {
            return ERROR_RESPONSE_TIMEOUT;
        }
        if (expired(idleUntil_ms))
        {
            return ERROR_READ_TIMEOUT;
        }
        _wait(wantWrite, earliest(idleUntil_ms, deadline_ms));
    }
    return 0;
}

int IotIdfHttp::_read(char* buffer, size_t length, unsigned long deadline_ms)
{
    const unsigned long idleUntil_ms = millis() + _requestTimeout_ms;
    for (;;)
    {
        bool wantWrite = false;
        if (_tls != nullptr)
        {
            // first: mbedTLS may hold decrypted data the socket no longer shows
            const int got = mbedtls_ssl_read(&_tls->ssl, (unsigned char*)buffer, length);
            if (got > 0)
            {
                return got;
            }
            if (got == 0 || got == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
            {
                return 0;
            }
            if (got != MBEDTLS_ERR_SSL_WANT_READ && got != MBEDTLS_ERR_SSL_WANT_WRITE)
            {
                log_e("IotIdfHttp: TLS read failed: -0x%04X %s", (unsigned)-got, mbedtlsText(got).c_str());
                _lastTlsError = got;
                return ERROR_CONNECTION_LOST;
            }
            wantWrite = got == MBEDTLS_ERR_SSL_WANT_WRITE;
        } else
        {
            const int got = ::recv(_fd, buffer, length, MSG_DONTWAIT);
            if (got >= 0)
            {
                return got;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
                log_e("IotIdfHttp: read failed (errno %d)", errno);
                return ERROR_CONNECTION_LOST;
            }
        }
        if (expired(deadline_ms))
        {
            return ERROR_RESPONSE_TIMEOUT;
        }
        if (expired(idleUntil_ms))
        {
            return ERROR_READ_TIMEOUT;
        }
        _wait(wantWrite, earliest(idleUntil_ms, deadline_ms));
    }
}

// *****************************************************************************
// Requests
// *****************************************************************************

int IotIdfHttp::send(const IotHttpRequest& request, IotHttpResponse& oResponse)
{
    oResponse = IotHttpResponse{};

    // each request is bounded by its deadline, the sequence of them is not:
    // a 401 retry alone makes three
    feedWatchdog();
    const unsigned long start_ms = millis();
    const unsigned long deadline_ms = start_ms + _responseTimeout_ms;

    iot_http1::Url url;
    if (!iot_http1::parseUrl(request.url, url))
    {
        log_e("IotIdfHttp: invalid URL '%s'", request.url.c_str());
        oResponse.status = ERROR_INVALID_URL;
        return oResponse.status;
    }
    if (url.https && !hasServerTrust())
    {
        log_e("IotIdfHttp: no TLS server trust for '%s'", request.url.c_str());
        oResponse.status = ERROR_NO_SERVER_TRUST;
        return oResponse.status;
    }

    bool retry = false;
    int status = _exchange(request, url, retry, oResponse, deadline_ms);
    if (retry)
    {
        log_d("IotIdfHttp: the server closed the reused connection, repeating on a new one");
        oResponse = IotHttpResponse{};
        status = _exchange(request, url, retry, oResponse, deadline_ms);
    }

    oResponse.status = status;
    oResponse.elapsed_ms = (uint32_t)(millis() - start_ms);
    if (status < 0)
    {
        oResponse.body.clear();
        oResponse.headers.clear();
        close();  // in whatever state: do not reuse
    } else
    {
        oResponse.phase = IotTransportPhase::None;
    }
    return status;
}

int IotIdfHttp::_exchange(const IotHttpRequest& request, const iot_http1::Url& url, bool& oRetry,
    IotHttpResponse& oResponse, unsigned long deadline_ms)
{
    const bool reused = !oRetry && _origin == url.origin() && _usable();
    oRetry = false;
    if (!reused)
    {
        const int status = _connect(url, oResponse, deadline_ms);
        if (status != 0)
        {
            return status;
        }
    }
    // the server may close an idle connection the moment it is reused; what
    // it has not answered at all it has not processed, and is repeated once
    auto retryable = [&](int error, size_t received)
    {
        return reused && received == 0 && (error == ERROR_CONNECTION_LOST || error == 0);
    };

    // request
    std::map<std::string, std::string> headers;
    for (auto const& kv : request.headers)
    {
        if (kv.second.empty()) { continue; }
        if (!iot_http1::isHeaderSafe(kv.first) || !iot_http1::isHeaderSafe(kv.second))
        {
            log_e("IotIdfHttp: header '%s' contains a line break, not sent", kv.first.c_str());
            continue;
        }
        log_d("  HTTP header: %s=%s", kv.first.c_str(), kv.second.c_str());
        headers[kv.first] = kv.second;
    }
    std::string head = iot_http1::requestHead(request.method, url, headers, request.body.size(), USER_AGENT);
    const bool coalesce = request.body.size() <= COALESCE_BODY_LENGTH;
    if (coalesce)
    {
        head += request.body;
    }
    _lastTlsError = 0;
    oResponse.phase = IotTransportPhase::Send;
    int error = _write(head.data(), head.size(), deadline_ms);
    if (error == 0 && !coalesce)
    {
        error = _write(request.body.data(), request.body.size(), deadline_ms);
        if (error == ERROR_CONNECTION_LOST) { error = ERROR_SEND_BODY; }
    }
    if (error != 0)
    {
        if (retryable(error, 0)) { oRetry = true; return error; }
        oResponse.tlsError = _lastTlsError;
        return error == ERROR_CONNECTION_LOST ? ERROR_SEND_HEADER : error;
    }

    // response
    oResponse.phase = IotTransportPhase::Receive;
    iot_http1::ResponseParser parser;
    parser.begin(strcasecmp(request.method, "HEAD") == 0, request.collectResponseHeaders, oResponse.body);
    char buffer[1024];
    size_t received = 0;
    bool trailing = false;  // bytes after the response: not reusable
    while (!parser.done())
    {
        const int got = _read(buffer, sizeof(buffer), deadline_ms);
        if (got < 0)
        {
            if (retryable(got, received)) { oRetry = true; return got; }
            if (got == ERROR_RESPONSE_TIMEOUT)
            {
                log_e("IotIdfHttp: response not complete within %lu ms, %u bytes received",
                    (unsigned long)_responseTimeout_ms, (unsigned)received);
            }
            oResponse.tlsError = _lastTlsError;
            return got;
        }
        if (got == 0)
        {
            if (retryable(0, received)) { oRetry = true; return ERROR_CONNECTION_LOST; }
            parser.onClose();
            trailing = true;  // closed: nothing to reuse
        } else
        {
            received += (size_t)got;
            feedWatchdog();  // progress
            trailing = parser.feed(buffer, (size_t)got) < (size_t)got;
        }
        if (parser.failed())
        {
            break;
        }
    }

    if (parser.failed())
    {
        switch (parser.error())
        {
            case iot_http1::ResponseParser::Error::OutOfMemory:
                log_e("IotIdfHttp: no memory for the body, %u bytes received", (unsigned)oResponse.body.size());
                oResponse.phase = IotTransportPhase::None;
                return ERROR_OUT_OF_MEMORY;
            case iot_http1::ResponseParser::Error::Encoding:
                log_e("IotIdfHttp: unsupported transfer coding");
                return ERROR_ENCODING;
            case iot_http1::ResponseParser::Error::Truncated:
                log_e("IotIdfHttp: connection closed after %u bytes of the response", (unsigned)received);
                return ERROR_CONNECTION_LOST;
            default:
                log_e("IotIdfHttp: malformed response");
                return ERROR_MALFORMED_RESPONSE;
        }
    }

    oResponse.headers = parser.headers();
    if (trailing || !parser.keepAlive())
    {
        close();
    }
    return parser.status();
}

// *****************************************************************************
// Texts
// *****************************************************************************

std::string IotIdfHttp::errorToString(int status) const
{
    switch (status)
    {
        case ERROR_CONNECT:            return "connect failed";
        case ERROR_SEND_HEADER:        return "send failed";
        case ERROR_SEND_BODY:          return "send of the body failed";
        case ERROR_CONNECTION_LOST:    return "connection lost";
        case ERROR_MALFORMED_RESPONSE: return "malformed response";
        case ERROR_OUT_OF_MEMORY:      return "out of memory";
        case ERROR_ENCODING:           return "unsupported transfer coding";
        case ERROR_READ_TIMEOUT:       return "read timeout";
        case ERROR_RESPONSE_TIMEOUT:   return "response took too long";
        case ERROR_INVALID_URL:        return "invalid URL";
        case ERROR_NO_SERVER_TRUST:    return "no TLS server trust configured";
        default:                       return "transport error " + std::to_string(status);
    }
}

std::string IotIdfHttp::tlsErrorToString(int tlsError) const
{
    return mbedtlsText(tlsError);
}
