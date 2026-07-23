/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

// *****************************************************************************
// Typed result of an API operation.
//
// The api* methods return a raw status code where negative values are ESP
// transport errors (e.g. connection refused, timeout) and non-negative values
// are HTTP status codes. IotResult disentangles these three states so that
// application code can write `if (iot.postTelemetry(...))` and still inspect
// the underlying status on failure.
//
// This type is intentionally free of Arduino dependencies so it can be
// unit-tested on the native platform (see test/test_result).
// *****************************************************************************

struct IotResult
{
    enum Kind { Ok, HttpError, TransportError };

    // Synthetic status codes for client-side failures that never reach the
    // network. They are chosen above the real HTTP range (>= 600) so that
    // IotResult classifies them as HttpError and they never collide with a
    // server status (1xx-5xx) or a (negative) transport error. Callers can
    // test for them via .httpStatus.
    static constexpr int STATUS_NO_PROVISIONING_TOKEN = 600; ///< no provisioning token configured
    static constexpr int STATUS_MALFORMED_RESPONSE    = 601; ///< 2xx but missing/invalid body

    Kind kind;
    int httpStatus;      ///< HTTP status code for Ok/HttpError, 0 otherwise
    int transportError;  ///< negative ESP HTTPClient error for TransportError, 0 otherwise

    IotResult(): kind(TransportError), httpStatus(0), transportError(0) {}

    /**
     * Classify a raw status code as returned by the api* methods:
     * a value < 0 is an ESP transport error, 2xx is success, any other
     * non-negative value is an HTTP error.
     *
     * The constructor is intentionally non-explicit so that a function
     * returning IotResult can `return api.apiPost(...);` directly.
     */
    IotResult(int status)
    {
        if (status < 0)
        {
            kind = TransportError;
            httpStatus = 0;
            transportError = status;
        } else if (status >= 200 && status < 300)
        {
            kind = Ok;
            httpStatus = status;
            transportError = 0;
        } else {
            kind = HttpError;
            httpStatus = status;
            transportError = 0;
        }
    }

    /// @return true if the operation succeeded (2xx). Explicit, so it only
    /// applies in a boolean context like `if (result)`, never as a silent
    /// conversion to int.
    explicit operator bool() const { return kind == Ok; }

    bool isOk() const { return kind == Ok; }
    bool isHttpError() const { return kind == HttpError; }
    bool isTransportError() const { return kind == TransportError; }
};
