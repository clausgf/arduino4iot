/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

// *****************************************************************************
// The ESP logging macros, or nothing.
//
// Lets a service implementation log on target and still compile - and therefore
// be tested - on the host, without dragging Arduino into it.
// *****************************************************************************

#if defined(ARDUINO) || defined(ESP_PLATFORM)
#include <esp32-hal-log.h>
#else
#define log_e(...) ((void)0)
#define log_w(...) ((void)0)
#define log_i(...) ((void)0)
#define log_d(...) ((void)0)
#define log_v(...) ((void)0)
#endif
