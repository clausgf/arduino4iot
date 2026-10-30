/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

#include <cstdint>
#include <type_traits>

// *****************************************************************************
// Application state in RTC RAM, validated before use.
//
// RTC_NOINIT_ATTR memory survives deep sleep and software/watchdog resets, but
// holds garbage after a power-on reset - and after an OTA update it holds the
// previous firmware's bytes, possibly for a different struct at that address.
// IotRtcSlot wraps a value with a magic word and a layout tag (size plus a
// version), and hands out a default-constructed value whenever either does not
// match. The same check IotRtcState does for the library's own state.
//
//   struct ImageState { char etag[96]; uint32_t deferredCycles; };
//   static RTC_NOINIT_ATTR IotRtcSlot<ImageState> imageState;
//
//   imageState->deferredCycles++;          // valid, or freshly reset
//   if (!imageState.isValid()) { ... }     // before the first access: lost?
//
// Bump Version when the meaning of T changes without its size changing.
// RTC_DATA_ATTR works too, but is reinitialized on every reset that is not a
// deep sleep wakeup - including the panic/watchdog resets a backoff must count.
// *****************************************************************************

template <typename T, uint32_t Version = 1>
class IotRtcSlot
{
    static_assert(std::is_trivially_copyable<T>::value,
        "IotRtcSlot holds raw RTC memory: T must be trivially copyable");
    static_assert(Version < 0x1000, "IotRtcSlot: Version must fit 12 bits");
    static_assert(sizeof(T) < 0x100000, "IotRtcSlot: T is too large for RTC RAM");

public:
    // No constructor and no member initializers on purpose: either would run
    // at startup and overwrite the RTC contents this exists to keep.

    /// @return true if the slot holds a value written by this layout.
    bool isValid() const { return _magic == MAGIC && _layout == LAYOUT; }

    /// @return the value, reset to T{} first if it did not survive.
    T& get()
    {
        if (!isValid()) { reset(); }
        return _value;
    }

    T* operator->() { return &get(); }
    T& operator*() { return get(); }

    /// Discard the value: T{} from now on.
    void reset()
    {
        _value = T{};
        _magic = MAGIC;
        _layout = LAYOUT;
    }

private:
    static constexpr uint32_t MAGIC = 0x5107A7E1;
    static constexpr uint32_t LAYOUT = (uint32_t)sizeof(T) | (Version << 20);

    uint32_t _magic;
    uint32_t _layout;
    T _value;
};
