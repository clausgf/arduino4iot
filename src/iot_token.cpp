/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#include "iot_token.h"

#include <ArduinoJson.h>

// *****************************************************************************

bool iotParseProvisioningResponse(const char* body, IotProvisioningResponse& oResponse)
{
    oResponse = IotProvisioningResponse{};
    if (body == nullptr || body[0] == '\0')
    {
        return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, body))
    {
        return false;
    }
    if (!doc["tokenType"].is<const char*>() || !doc["accessToken"].is<const char*>())
    {
        return false;
    }

    oResponse.tokenType = doc["tokenType"].as<const char*>();
    oResponse.accessToken = doc["accessToken"].as<const char*>();
    if (doc["expiresIn"].is<int64_t>())
    {
        oResponse.expiresIn_s = doc["expiresIn"].as<int64_t>();
    }
    return oResponse.isValid();
}

// *****************************************************************************

void IotTokenManager::begin(IotStore& store)
{
    _store = &store;
    _provisioningToken = store.getString(iot_nvs_keys::PROVISIONING_TOKEN, "");
    _deviceToken = store.getString(iot_nvs_keys::DEVICE_TOKEN, "");
    _deviceTokenExpiresAt = store.getInt(iot_nvs_keys::DEVICE_TOKEN_EXPIRY, 0);
}

// *****************************************************************************

bool IotTokenManager::needsNewToken(int64_t now, bool isTimePlausible) const
{
    if (_deviceToken.empty())
    {
        return true;
    }
    if (_deviceTokenExpiresAt <= 0 || !isTimePlausible)
    {
        return false;
    }
    return now + _expiryMargin_s >= _deviceTokenExpiresAt;
}

// *****************************************************************************

void IotTokenManager::setDeviceToken(const std::string& token, int64_t expiresAt)
{
    if (_deviceToken == token && _deviceTokenExpiresAt == expiresAt)
    {
        return;
    }
    _deviceToken = token;
    _deviceTokenExpiresAt = expiresAt;
    if (_store != nullptr)
    {
        _store->putString(iot_nvs_keys::DEVICE_TOKEN, _deviceToken);
        _store->putInt(iot_nvs_keys::DEVICE_TOKEN_EXPIRY, _deviceTokenExpiresAt);
    }
}

// *****************************************************************************

int64_t IotTokenManager::applyProvisioningResponse(const IotProvisioningResponse& response,
    int64_t now, bool isTimePlausible)
{
    int64_t expiresAt = 0;
    if (response.expiresIn_s >= 0 && isTimePlausible)
    {
        expiresAt = now + response.expiresIn_s;
    }
    setDeviceToken(response.authorizationValue(), expiresAt);
    return expiresAt;
}
