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
// Arduino-free orchestration over IotPower and IotRtcState, so the failure
// behaviour of a battery device can be exercised on the host against
// IotFakePower.
//
// The split: the *mechanism* of ending a cycle is a port (IotPower), the
// *policy* of when and how long is plain logic (IotBackoffPolicy). Logging
// happens in the Iot facade around these calls, keeping this tier free of the
// logger and its reentrancy hazard.
// *****************************************************************************


/**
 * Escalating backoff for a device with nobody to look at it.
 *
 * After an abnormal cycle the device sleeps for @ref initialDuration_s; each
 * further failure multiplies that by @ref factor up to @ref maxDuration_s. An
 * orderly exit clears the state, so a recovered device returns to its normal
 * interval at once. Pure value type: no ports, no I/O.
 */
struct IotBackoffPolicy
{
    int32_t initialDuration_s = 60;
    int32_t factor            = 2;
    int32_t maxDuration_s     = 24 * 60 * 60;

    /**
     * @param current_s the running backoff duration, <= 0 meaning "not in backoff"
     * @return the duration to sleep now, always >= 1 s and <= maxDuration_s
     */
    int32_t next(int32_t current_s) const;
};


// *****************************************************************************

/**
 * Bookkeeping and exits of one wakeup cycle.
 *
 * Owns the state that survives a deep sleep but not a power loss (boot count,
 * active duration, backoff - all in IotRtcState) and the three ways a cycle can
 * end. To do something before sleeping, supply an own IotPower or register an
 * exit hook.
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
     * Register a callback to run just before the device sleeps, restarts or
     * shuts down - flush logs, close the connection, park a sensor. Hooks run
     * in registration order, and on the failure path too, so a panic still
     * gets its logs out.
     */
    void addExitHook(std::function<void()> hook) { _exitHooks.push_back(std::move(hook)); }

    /**
     * Start a wakeup cycle: validate the RTC state, count the boot and latch
     * why the device came up.
     *
     * Deliberately does *not* act on an abnormal reset - the caller decides,
     * once its logging works.
     */
    void begin();

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

    // The orderly exits. Each clears the backoff state, so a device that
    // completes a cycle returns to its normal interval - unless @p isFailure
    // says the exit is itself the reaction to a problem (e.g. undervoltage
    // shutdown), which leaves the backoff state as it is.

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
     * Enter or escalate the failure backoff and sleep for its duration. The
     * backoff state is kept, so a device that keeps failing escalates instead
     * of hammering the server. Does not return on a real device.
     *
     * @return the duration slept for, for the caller's log.
     */
    uint32_t enterBackoff();

private:
    enum class Exit { DeepSleep, Restart, Shutdown };

    void _exit(Exit exit, uint32_t duration_s, bool isFailure);

    IotPower* _power;
    IotRtcState& _rtc;
    IotBackoffPolicy _backoff;
    std::vector<std::function<void()>> _exitHooks;
    IotResetReason _resetReason;
    IotWakeupCause _wakeupCause;
    bool _begun;
};
