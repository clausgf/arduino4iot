/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#include "iot_result.h"

// *****************************************************************************

IotResult::IotResult(int status)
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

// *****************************************************************************

const char* iotTransportPhaseToString(IotTransportPhase phase)
{
    switch (phase)
    {
        case IotTransportPhase::Dns:     return "dns";
        case IotTransportPhase::Connect: return "connect";
        case IotTransportPhase::Tls:     return "tls";
        case IotTransportPhase::Send:    return "send";
        case IotTransportPhase::Receive: return "receive";
        case IotTransportPhase::None:    break;
    }
    return "none";
}
