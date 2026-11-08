/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

// *****************************************************************************
// Typed result of an API operation.
//
// A transport returns a raw status code where negative values are ESP
// transport errors (e.g. connection refused, timeout) and non-negative values
// are HTTP status codes. IotResult disentangles these three states so that
// application code can write `if (iot.telemetry.post(...))` and still inspect
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
    static constexpr int STATUS_UPDATE_FAILED         = 602; ///< update was available but could not be downloaded/applied

    Kind kind;
    int httpStatus;      ///< HTTP status code for Ok/HttpError, 0 otherwise
    int transportError;  ///< negative ESP HTTPClient error for TransportError, 0 otherwise

    IotResult(): kind(TransportError), httpStatus(0), transportError(0) {}

    /**
     * Classify a raw status code as returned by an IotHttp transport:
     * a value < 0 is an ESP transport error, 2xx is success, any other
     * non-negative value is an HTTP error.
     *
     * Non-explicit, so a raw status and a synthetic STATUS_* code can be
     * returned as an IotResult directly.
     */
    IotResult(int status);

    /// @return true if the operation succeeded (2xx). Explicit, so it only
    /// applies in a boolean context like `if (result)`, never as a silent
    /// conversion to int.
    explicit operator bool() const { return kind == Ok; }

    /// @return the HTTP status, or the negative transport error for a
    /// TransportError: one number for a log line or a telemetry field.
    int code() const { return kind == TransportError ? transportError : httpStatus; }

    bool isOk() const { return kind == Ok; }
    bool isHttpError() const { return kind == HttpError; }
    bool isTransportError() const { return kind == TransportError; }

    /// @return true for HTTP 304 Not Modified. For cache-aware requests (config
    /// and firmware use ETag/If-None-Match) this is not an error but means "the
    /// resource is unchanged, nothing to do".
    bool isNotModified() const { return httpStatus == 304; }

    /// @return true if the operation neither failed nor changed anything, i.e.
    /// success (2xx) or Not Modified (304). Convenient for cache-aware calls
    /// where "up to date" is a normal outcome: `if (!r.isOkOrNotModified())`.
    bool isOkOrNotModified() const { return isOk() || isNotModified(); }
};
