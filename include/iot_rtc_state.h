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
// Deliberately not a port: RTC RAM has no runtime keys (RTC_NOINIT_ATTR is
// static allocation at link time), no failure mode and no write cost, so a
// key/value interface over it would buy a vtable and nothing else. One plain
// struct, passed to services by reference - a host test just puts one on the
// stack.
//
// Values here: worthless after a power loss, written on every wakeup.
// Values in IotStore: tokens, cache validators, seeded bootstrap config.
// Which of the two a value uses is decided where it is declared.
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
    /// Current backoff sleep duration in seconds, -1 when the last cycle ended
    /// in an orderly way. Here rather than in IotStore so a crash loop does not
    /// write flash on every attempt; RTC_NOINIT_ATTR keeps it across a
    /// software/watchdog reset, and a power loss legitimately starts over.
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
 * @return the process-wide RTC state, reset first if it did not survive (a
 * power-on reset, or a changed layout). Defined by the ESP32 adapter over an
 * RTC_NOINIT_ATTR instance. Services should take an `IotRtcState&` instead of
 * calling this, so they stay testable.
 */
IotRtcState& iotRtcState();
