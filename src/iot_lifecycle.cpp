/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#include "iot_lifecycle.h"

// *****************************************************************************

int32_t IotBackoffPolicy::next(int32_t current_s) const
{
    // int64 intermediate: current_s * factor overflows int32 within a few
    // escalations of a large initial duration
    int64_t result = (current_s <= 0)
        ? initialDuration_s
        : (int64_t)current_s * (factor > 1 ? factor : 1);

    int64_t cap = maxDuration_s > 0 ? maxDuration_s : 1;
    if (result > cap)
    {
        result = cap;
    }
    if (result < 1)
    {
        result = 1;  // never a zero-length sleep: that would be a busy loop
    }
    return (int32_t)result;
}

// *****************************************************************************

void IotLifecycle::begin()
{
    if (!_rtc.isValid())
    {
        // power-on reset, or a firmware whose RTC layout changed
        _rtc.reset();
    }
    _resetReason = _power->resetReason();
    _wakeupCause = _power->wakeupCause();
    _rtc.bootCount++;
    _begun = true;
}

// *****************************************************************************

int32_t IotLifecycle::enterBackoff()
{
    int32_t duration_s = _backoff.next(_rtc.panicSleepDuration_s);
    _rtc.panicSleepDuration_s = duration_s;
    _exit(Exit::DeepSleep, duration_s, true);
    return duration_s;
}

// *****************************************************************************

void IotLifecycle::_exit(Exit exit, int32_t duration_s, bool isFailure)
{
    if (!isFailure)
    {
        _rtc.panicSleepDuration_s = -1;  // the cycle ended cleanly
    }
    _rtc.lastSleepDuration_s = (exit == Exit::DeepSleep) ? duration_s : 0;
    _rtc.activeDuration_ms = (int64_t)_power->uptime_ms();

    for (auto& hook : _exitHooks)
    {
        if (hook)
        {
            hook();
        }
    }

    switch (exit)
    {
        case Exit::DeepSleep: _power->deepSleep(duration_s); break;
        case Exit::Restart:   _power->restart(); break;
        case Exit::Shutdown:  _power->shutdown(); break;
    }
}
