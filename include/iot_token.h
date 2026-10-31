/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

#include <cstdint>
#include <string>

#include "iot_names.h"
#include "iot_ports.h"

// *****************************************************************************
// Device authentication: the tokens, their lifetime and their persistence.
//
// Separate from IotApi so the transport does not know about tokens and the
// token state does not know about HTTP: state and decisions here (Arduino-free
// and unit tested), I/O in IotApi, which performs the exchange and feeds the
// parsed result back in.
// *****************************************************************************


/**
 * The parsed body of a successful `/provision` response.
 */
struct IotProvisioningResponse
{
    std::string tokenType;      ///< e.g. "bearer"
    std::string accessToken;
    int64_t expiresIn_s = -1;   ///< lifetime in seconds; < 0 if the server did not say

    /// @return true if the body carried a usable token.
    bool isValid() const { return !tokenType.empty() && !accessToken.empty(); }

    /// @return the Authorization header value, e.g. "bearer eyJhbGci...".
    std::string authorizationValue() const { return tokenType + " " + accessToken; }
};

/**
 * Parse a `/provision` response body.
 * @return true if it was valid JSON carrying both tokenType and accessToken.
 *         A 2xx that fails this becomes IotResult::STATUS_MALFORMED_RESPONSE.
 */
bool iotParseProvisioningResponse(const char* body, IotProvisioningResponse& oResponse);


// *****************************************************************************

/**
 * The device's credentials: the long-lived provisioning token it was seeded
 * with, and the short-lived device token it exchanges that for.
 *
 * Persists the device token through an IotStore so a device does not
 * re-provision on every wakeup. Knows nothing about HTTP - the caller performs
 * the exchange and passes the parsed response to applyProvisioningResponse().
 */
class IotTokenManager
{
public:
    // NVS keys, part of the documented schema (docs/concepts.md)

    /// Default margin before the expiry at which a new token is requested.
    static constexpr int32_t DEFAULT_EXPIRY_MARGIN_S = 3600;

    /// Load the seeded provisioning token and the persisted device token.
    /// @p store must outlive this object.
    void begin(IotStore& store);

    void end() { _store = nullptr; }

    const std::string& provisioningToken() const { return _provisioningToken; }
    bool hasProvisioningToken() const { return !_provisioningToken.empty(); }

    /// @return the Authorization header value, or "" when unprovisioned.
    const std::string& deviceToken() const { return _deviceToken; }
    bool hasDeviceToken() const { return !_deviceToken.empty(); }

    /// @return the token expiry as unix seconds, or 0 if unknown.
    int64_t deviceTokenExpiresAt() const { return _deviceTokenExpiresAt; }

    /**
     * Margin before the expiry at which a new token is requested. Larger than
     * the sleep interval, so a device never wakes with an expired token - but
     * smaller than the lifetime the server issues, or every wakeup re-provisions.
     */
    void setExpiryMargin_s(int32_t margin_s) { _expiryMargin_s = margin_s; }
    int32_t expiryMargin_s() const { return _expiryMargin_s; }

    /**
     * Decide whether a new device token has to be requested.
     *
     * Two cases deliberately count as "still good", since guessing wrong costs
     * a provisioning round trip on every wakeup: an unknown expiry (0, e.g. a
     * server that sent no expiresIn), and an implausible clock, where the
     * arithmetic would be meaningless. Either way an actually expired token
     * surfaces as an HTTP 401, which re-provisions.
     */
    bool needsNewToken(int64_t now, bool isTimePlausible) const;

    /// Store a device token and its absolute expiry (0 = unknown), persisting
    /// both. Writes nothing when unchanged.
    void setDeviceToken(const std::string& token, int64_t expiresAt);

    void clearDeviceToken() { setDeviceToken("", 0); }

    /**
     * Adopt a parsed provisioning response. The absolute expiry derives from
     * the relative expiresIn, which needs a plausible clock; without one the
     * token is stored with an unknown expiry (0) rather than a wrong one.
     *
     * @return the absolute expiry that was stored, 0 if unknown.
     */
    int64_t applyProvisioningResponse(const IotProvisioningResponse& response,
        int64_t now, bool isTimePlausible);

private:
    IotStore* _store = nullptr;
    std::string _provisioningToken;
    std::string _deviceToken;
    int64_t _deviceTokenExpiresAt = 0;
    int32_t _expiryMargin_s = DEFAULT_EXPIRY_MARGIN_S;
};
