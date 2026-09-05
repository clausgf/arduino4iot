/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "iot_ports.h"
#include "iot_rtc_state.h"

// *****************************************************************************
// The wakeup cycle: its bookkeeping, its exits and its failure backoff.
//
// Arduino-free on purpose. Everything here is orchestration over IotPower and
// IotRtcState, so the whole failure behaviour of a battery device - the part
// that used to be untestable because reaching it ended the process in a real
// esp_deep_sleep() - can be exercised on the host against IotFakePower.
//
// The split this file embodies: the *mechanism* of ending a cycle is a port
// (IotPower), the *policy* of when and for how long is plain logic
// (IotBackoffPolicy). Logging is not done here; the Iot facade logs around
// these calls, which keeps this tier free of the logger and of the reentrancy
// hazard that comes with it.
// *****************************************************************************


/**
 * Escalating backoff for a device with nobody to look at it.
 *
 * After a cycle that ended abnormally, the device sleeps for
 * @ref initialDuration_s. If the next cycle fails again before any orderly
 * exit, the duration is multiplied by @ref factor, up to @ref maxDuration_s.
 * An orderly exit clears the state, so a device that recovers returns to its
 * normal interval immediately.
 *
 * Pure value type: no ports, no I/O, fully unit tested (test/test_lifecycle).
 */
struct IotBackoffPolicy
{
    int32_t initialDuration_s = 60;
    int32_t factor            = 2;
    int32_t maxDuration_s     = 24 * 60 * 60;

    /**
     * @param current_s the running backoff duration, <= 0 meaning "not in
     *        backoff" (the previous cycle ended in an orderly way)
     * @return the duration to sleep now, always >= 1 s and <= maxDuration_s
     */
    int32_t next(int32_t current_s) const
    {
        int64_t result;
        if (current_s <= 0)
        {
            result = initialDuration_s;
        } else {
            // int64 intermediate: current_s * factor overflows int32 within a
            // few escalations of a large initial duration
            result = (int64_t)current_s * (factor > 1 ? factor : 1);
        }

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
};


// *****************************************************************************

/**
 * Bookkeeping and exits of one wakeup cycle.
 *
 * Owns the values that have to survive a deep sleep but not a power loss (boot
 * count, active duration, backoff state - all in IotRtcState) and the three
 * ways a cycle can end. Replaces the pre-4.0 IotPersistentValue plus the three
 * handler setters (setDeepSleepHandler/setRestartHandler/setShutdownHandler):
 * an application that has to do something before sleeping now supplies its own
 * IotPower, or registers an exit hook.
 */
class IotLifecycle
{
public:
    IotLifecycle(const IotLifecycle&) = delete;
    IotLifecycle& operator=(const IotLifecycle&) = delete;

    IotLifecycle(IotPower& power, IotRtcState& rtcState):
        _power(&power),
        _rtc(rtcState),
        _resetReason(IotResetReason::Unknown),
        _wakeupCause(IotWakeupCause::Unknown),
        _begun(false)
    {
    }

    /// Replace the power adapter, e.g. to park peripherals before sleeping.
    /// Must be called before begin().
    void setPower(IotPower& power) { _power = &power; }

    void setBackoffPolicy(const IotBackoffPolicy& policy) { _backoff = policy; }
    const IotBackoffPolicy& backoffPolicy() const { return _backoff; }

    /**
     * Register a callback to run at every exit, just before the device sleeps,
     * restarts or shuts down - flushing buffered logs, closing the API
     * connection, parking a sensor. Hooks run in registration order and run on
     * the failure path too, so a panic still gets its logs out.
     */
    void addExitHook(std::function<void()> hook) { _exitHooks.push_back(std::move(hook)); }

    /**
     * Start a wakeup cycle: validate the RTC state, count the boot and latch
     * why the device came up.
     *
     * Deliberately does *not* act on an abnormal reset - the caller decides,
     * once its logging works. That is the fix for the pre-4.0 behaviour of
     * reacting inside the Iot constructor, where the panic path ran before
     * Serial.begin() and before the other singletons were constructed.
     */
    void begin()
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

    bool isBegun() const { return _begun; }

    // --- what the previous cycle left behind ---

    IotResetReason resetReason() const { return _resetReason; }
    IotWakeupCause wakeupCause() const { return _wakeupCause; }

    /// @return true if the previous cycle ended in a crash, a watchdog timeout
    /// or a brownout rather than an orderly exit. The trigger for the backoff.
    bool isAbnormalStart() const { return iotIsAbnormalReset(_resetReason); }

    /// @return boots since the last power-on reset, this one included.
    uint32_t bootCount() const { return _rtc.bootCount; }

    /// @return how long the *previous* cycle stayed awake, in milliseconds.
    int64_t lastActiveDuration_ms() const { return _rtc.activeDuration_ms; }

    /// @return the nominal duration of the sleep this cycle woke up from, in
    /// seconds; 0 after a restart or shutdown.
    int32_t lastSleepDuration_s() const { return _rtc.lastSleepDuration_s; }

    /// @return the running backoff duration in seconds, or -1 when not in
    /// backoff (the previous cycle ended in an orderly way).
    int32_t backoffDuration_s() const { return _rtc.panicSleepDuration_s; }

    // --- ending the cycle ---

    // The three orderly exits. Each clears the backoff state, so a device that
    // completes a cycle returns to its normal interval - unless @p isFailure
    // says the exit is itself the reaction to a problem (e.g. shutting down on
    // undervoltage), in which case the backoff state is left as it is.

    /// Sleep for @p duration_s and wake through a full boot. Does not return
    /// on a real device.
    void deepSleep(uint32_t duration_s, bool isFailure = false)
    {
        _exit(Exit::DeepSleep, duration_s, isFailure);
    }

    /// Restart now. Does not return on a real device.
    void restart(bool isFailure = false) { _exit(Exit::Restart, 0, isFailure); }

    /// Sleep with no wakeup source, i.e. until a physical reset. Does not
    /// return on a real device.
    void shutdown(bool isFailure = false) { _exit(Exit::Shutdown, 0, isFailure); }

    /**
     * Enter or escalate the failure backoff and sleep for its duration.
     *
     * Unlike the orderly exits, the backoff state is kept, so a device that
     * keeps failing escalates instead of hammering the server or draining its
     * battery. Does not return on a real device.
     *
     * @return the duration slept for - useful in tests and for the caller's log.
     */
    uint32_t enterBackoff()
    {
        int32_t duration_s = _backoff.next(_rtc.panicSleepDuration_s);
        _rtc.panicSleepDuration_s = duration_s;
        _exit(Exit::DeepSleep, (uint32_t)duration_s, true);
        return (uint32_t)duration_s;
    }

private:
    enum class Exit { DeepSleep, Restart, Shutdown };

    void _exit(Exit exit, uint32_t duration_s, bool isFailure)
    {
        if (!isFailure)
        {
            _rtc.panicSleepDuration_s = -1;  // the cycle ended cleanly
        }
        _rtc.lastSleepDuration_s = (exit == Exit::DeepSleep) ? (int32_t)duration_s : 0;
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

    IotPower* _power;
    IotRtcState& _rtc;
    IotBackoffPolicy _backoff;
    std::vector<std::function<void()>> _exitHooks;
    IotResetReason _resetReason;
    IotWakeupCause _wakeupCause;
    bool _begun;
};
