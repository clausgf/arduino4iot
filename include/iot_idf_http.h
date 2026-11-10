/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

#include <string>

#include "iot_ports.h"

namespace iot_http1 { struct Url; }

// *****************************************************************************
// IotIdfHttp - IotHttp over lwIP sockets and mbedTLS
// *****************************************************************************

/**
 * IotHttp over the ESP-IDF directly: an lwIP socket, mbedTLS for https, and
 * the HTTP/1.1 framing of iot_http1. The default HTTP adapter of `iot`.
 *
 * Every step is non-blocking and waits with its own bound, so each phase
 * has a timeout of its own and a transport error says exactly where it
 * happened:
 *
 *   phase      bound                                     error
 *   DNS        lwIP's resolver                           Dns
 *   connect    setConnectTimeout_ms()                    Connect (errno in the log)
 *   handshake  setHandshakeTimeout_ms()                  Tls, with the mbedTLS code
 *   request    setRequestTimeout_ms() per wait for data  Send / Receive
 *   all        setResponseTimeout_ms(), from the start   Receive
 *
 * Only what has arrived is read, so the deadline and the watchdog feed act
 * between any two reads - in the response headers and in a body of any
 * framing (Content-Length, chunked, until close) alike. The watchdog is fed
 * whenever data arrives: a slow but progressing download completes.
 *
 * All requests of a wakeup cycle share one keep-alive connection per
 * origin; close() ends it. A request on a reused connection that the server
 * has closed meanwhile is repeated once on a new one, as long as no byte of
 * a response had arrived.
 *
 * IotEspHttp (HTTPClient) remains available as a fallback, @see Iot::setHttp().
 */
class IotIdfHttp : public IotHttp
{
public:
    IotIdfHttp(const IotIdfHttp&) = delete;
    IotIdfHttp& operator=(const IotIdfHttp&) = delete;

    IotIdfHttp() = default;
    ~IotIdfHttp() override;

    static constexpr uint32_t DEFAULT_CONNECT_TIMEOUT_MS = 5000;
    static constexpr uint32_t DEFAULT_HANDSHAKE_TIMEOUT_MS = 6000;
    /// Per wait for data while sending or receiving.
    static constexpr uint32_t DEFAULT_REQUEST_TIMEOUT_MS = 5000;
    /// The whole request, from the start. Above the watchdog: the watchdog is
    /// fed while data arrives, so a slow but progressing download completes.
    static constexpr uint32_t DEFAULT_RESPONSE_TIMEOUT_MS = 60000;

    /// Transport errors. The codes HTTPClient also has share its values.
    static constexpr int ERROR_CONNECT = -1;            ///< DNS, TCP or TLS; see the phase
    static constexpr int ERROR_SEND_HEADER = -2;
    static constexpr int ERROR_SEND_BODY = -3;
    static constexpr int ERROR_CONNECTION_LOST = -5;
    static constexpr int ERROR_MALFORMED_RESPONSE = -7;
    static constexpr int ERROR_OUT_OF_MEMORY = -8;
    static constexpr int ERROR_ENCODING = -9;           ///< a transfer coding other than chunked
    static constexpr int ERROR_READ_TIMEOUT = -11;
    static constexpr int ERROR_RESPONSE_TIMEOUT = -100;
    static constexpr int ERROR_INVALID_URL = -101;
    static constexpr int ERROR_NO_SERVER_TRUST = -102;  ///< https:// without a TLS mode

    void setTls(const IotTlsConfig& tls) override { _tlsConfig = &tls; }
    bool hasServerTrust() const override;
    void setConnectTimeout_ms(uint32_t timeout_ms) override;
    void setRequestTimeout_ms(uint32_t timeout_ms) override;
    void setHandshakeTimeout_ms(uint32_t timeout_ms) override;
    void setResponseTimeout_ms(uint32_t timeout_ms) override;

    int send(const IotHttpRequest& request, IotHttpResponse& oResponse) override;
    void close() override;
    std::string errorToString(int status) const override;
    std::string tlsErrorToString(int tlsError) const override;

private:
    struct _Tls;   ///< the mbedTLS state of a connection, kept out of this header

    const IotTlsConfig* _tlsConfig = nullptr;   ///< owned by the caller
    int _fd = -1;
    _Tls* _tls = nullptr;
    std::string _origin;   ///< of the open connection
    int _lastTlsError = 0; ///< of the last failed TLS read or write

    uint32_t _connectTimeout_ms = DEFAULT_CONNECT_TIMEOUT_MS;
    uint32_t _handshakeTimeout_ms = DEFAULT_HANDSHAKE_TIMEOUT_MS;
    uint32_t _requestTimeout_ms = DEFAULT_REQUEST_TIMEOUT_MS;
    uint32_t _responseTimeout_ms = DEFAULT_RESPONSE_TIMEOUT_MS;

    /// One attempt of send(). @p oRetry on entry: this is the repetition, so
    /// open a new connection; on return: repeat on a new connection.
    int _exchange(const IotHttpRequest& request, const iot_http1::Url& url, bool& oRetry,
        IotHttpResponse& oResponse, unsigned long deadline_ms);
    int _connect(const iot_http1::Url& url, IotHttpResponse& oResponse, unsigned long deadline_ms);
    int _handshake(const iot_http1::Url& url, IotHttpResponse& oResponse, unsigned long deadline_ms);
    /// @return true if the open connection can carry another request.
    bool _usable() const;
    /// @return 0, or ERROR_CONNECTION_LOST / ERROR_READ_TIMEOUT / ERROR_RESPONSE_TIMEOUT.
    int _write(const char* data, size_t length, unsigned long deadline_ms);
    /// @return the bytes read, 0 at the end of the connection, or an error as _write().
    int _read(char* buffer, size_t length, unsigned long deadline_ms);
    /// Wait until the socket is readable (or writable), at most until @p until_ms.
    /// @return false on a timeout.
    bool _wait(bool forWrite, unsigned long until_ms) const;
};
