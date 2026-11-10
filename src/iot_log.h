/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

// *****************************************************************************
// The ESP logging macros, or nothing; the log time stamp and RTC_NOINIT_ATTR.
//
// Lets a service implementation log on target and still compile - and therefore
// be tested - on the host, without dragging Arduino into it.
// *****************************************************************************

#include <cstdint>

#if defined(ARDUINO) || defined(ESP_PLATFORM)
#include <esp32-hal-log.h>
#include <esp_attr.h>
#include <esp_log.h>

/// Milliseconds since boot, as in the ESP log lines.
inline uint32_t iotLogTimestamp_ms() { return esp_log_timestamp(); }

#else
#include <chrono>

#define log_e(...) ((void)0)
#define log_w(...) ((void)0)
#define log_i(...) ((void)0)
#define log_d(...) ((void)0)
#define log_v(...) ((void)0)

/// On the host there is no RTC RAM: a plain static.
#define RTC_NOINIT_ATTR

/// Milliseconds since the first call, standing in for the time since boot.
inline uint32_t iotLogTimestamp_ms()
{
    static const auto start = std::chrono::steady_clock::now();
    return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
}
#endif
