/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

#include <cstdint>
#include <cstring>

// *****************************************************************************
// State that has to survive a deep sleep, but not a power loss.
//
// This is deliberately *not* a port (see iot_ports.h). RTC RAM has no runtime
// keys - RTC_NOINIT_ATTR is static allocation at link time - no failure mode,
// no write cost and no capacity to negotiate. Wrapping it in a key/value
// interface would buy a vtable and string keys and nothing else. So it is one
// plain struct, passed to services by reference, which makes it trivially
// testable on the host: a test just puts an IotRtcState on the stack.
//
// The counterpart is IotStore (iot_ports.h) for everything that must survive a
// power loss. Which of the two a value belongs in is a design decision made
// once, where the value is declared - not a global runtime switch that flips
// the storage of every value at once (the pre-4.0 IotPersistentValue model).
//
// Values here: worthless after a power loss, and written on every wakeup.
// Values in IotStore: tokens, cache validators, seeded bootstrap config.
// *****************************************************************************

struct IotRtcState
{
    /// Bumped whenever the layout below changes: a firmware update that
    /// changes the struct must not reinterpret the previous firmware's bytes.
    static constexpr uint32_t MAGIC = 0x104C0001;

    uint32_t magic;

    // --- cycle bookkeeping ---
    uint32_t bootCount;             ///< boots since the last power-on reset
    int64_t  activeDuration_ms;     ///< how long the previous cycle stayed awake
    int32_t  lastSleepDuration_s;   ///< nominal duration of the sleep we just woke from

    // --- failure backoff ---
    /// Current backoff sleep duration in seconds, or -1 when the last cycle
    /// ended in an orderly way. Lives here rather than in IotStore so a device
    /// stuck in a crash loop does not write flash on every attempt; it is kept
    /// across a software/watchdog reset by RTC_NOINIT_ATTR, and a power loss
    /// legitimately starts over.
    int32_t  panicSleepDuration_s;

    // --- clock ---
    int64_t  ntpLastSyncTime;       ///< unix seconds of the last successful sync, 0 = never

    // --- network fast reconnect (adapter-owned, meaningless to the core) ---
    int32_t  wifiChannel;           ///< 0 = no valid cache
    uint8_t  wifiBssid[6];
    uint32_t dhcpIp;                ///< 0 = no valid lease cache
    uint32_t dhcpGw;
    uint32_t dhcpMask;
    uint32_t dhcpDns1;
    uint32_t dhcpDns2;
    int32_t  dhcpReuseCount;

    /// @return true if the contents were written by this firmware's layout and
    /// survived; false after a power-on reset or a layout change.
    bool isValid() const { return magic == MAGIC; }

    /// Bring the struct into a defined initial state. Called automatically by
    /// iotRtcState() when isValid() is false.
    void reset()
    {
        std::memset(this, 0, sizeof(*this));
        magic = MAGIC;
        panicSleepDuration_s = -1;  // -1 means "the last cycle ended cleanly"
    }
};

/**
 * @return the process-wide RTC state, resetting it first if it did not survive
 * (power-on reset, or a firmware whose layout changed).
 *
 * Defined by the ESP32 adapter (iot_esp_ports.cpp) over an RTC_NOINIT_ATTR
 * instance. Services should take an `IotRtcState&` parameter instead of calling
 * this, so they stay testable.
 */
IotRtcState& iotRtcState();
