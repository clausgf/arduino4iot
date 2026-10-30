/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#include "iot_ports.h"

// *****************************************************************************

bool IotStore::isValidKey(const char* key)
{
    if (key == nullptr || key[0] == '\0')
    {
        return false;
    }
    size_t length = 0;
    for (const char* p = key; *p != '\0'; ++p, ++length)
    {
        if (length >= MAX_KEY_LENGTH || *p < 0x20 || *p > 0x7e)
        {
            return false;
        }
    }
    return true;
}

// *****************************************************************************

const char* iotResetReasonToString(IotResetReason reason)
{
    switch (reason)
    {
        case IotResetReason::PowerOn:   return "POWER_ON";
        case IotResetReason::External:  return "EXTERNAL_PIN";
        case IotResetReason::Software:  return "SOFTWARE";
        case IotResetReason::Panic:     return "PANIC";
        case IotResetReason::Watchdog:  return "WATCHDOG";
        case IotResetReason::Brownout:  return "BROWNOUT";
        case IotResetReason::DeepSleep: return "DEEP_SLEEP";
        case IotResetReason::Unknown:   break;
    }
    return "UNKNOWN";
}

const char* iotWakeupCauseToString(IotWakeupCause cause)
{
    switch (cause)
    {
        case IotWakeupCause::Timer:    return "TIMER";
        case IotWakeupCause::Gpio:     return "GPIO";
        case IotWakeupCause::Touchpad: return "TOUCHPAD";
        case IotWakeupCause::Ulp:      return "ULP";
        case IotWakeupCause::Uart:     return "UART";
        case IotWakeupCause::Unknown:  break;
    }
    return "UNKNOWN";
}
