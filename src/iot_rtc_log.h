/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// *****************************************************************************
// Log lines kept in RTC RAM while the device is offline.
//
// IotLogger appends here what it cannot send - after iot.radioOff(), or when
// the connect failed - and prepends it to the first log request of a later
// cycle. Bounded: when full, the oldest whole lines go first, so the most
// recent ones, typically those explaining a failure, are kept. A plain struct
// without a constructor, to live in an IotRtcSlot.
// *****************************************************************************

template <size_t Capacity>
struct IotRtcLog
{
    static_assert(Capacity > 0 && Capacity < 0x10000, "IotRtcLog: Capacity must fit uint16_t");

    uint16_t length;
    uint16_t droppedLines;
    char data[Capacity];

    bool isEmpty() const { return length == 0 && droppedLines == 0; }

    /// Append @p text, a sequence of '\n'-terminated lines, dropping the oldest
    /// lines as needed to make it fit.
    void append(const std::string& text)
    {
        size_t from = 0;
        // more than fits at all: keep the tail of the text
        while (text.size() - from > Capacity)
        {
            size_t nl = text.find('\n', from);
            from = (nl == std::string::npos) ? text.size() - Capacity : nl + 1;
            _countDropped();
        }
        const size_t needed = text.size() - from;

        while (length + needed > Capacity)
        {
            const void* nl = std::memchr(data, '\n', length);
            size_t lineEnd = (nl == nullptr) ? length : (size_t)((const char*)nl - data) + 1;
            std::memmove(data, data + lineEnd, length - lineEnd);
            length = (uint16_t)(length - lineEnd);
            _countDropped();
        }
        std::memcpy(data + length, text.data() + from, needed);
        length = (uint16_t)(length + needed);
    }

    /// @return the kept lines, led by a note if some were dropped, and empty
    /// the buffer.
    std::string take()
    {
        std::string out;
        if (droppedLines > 0)
        {
            char note[80];
            snprintf(note, sizeof(note), "W (0) iot: %u earlier offline log line(s) dropped\n",
                (unsigned)droppedLines);
            out = note;
        }
        out.append(data, length);
        length = 0;
        droppedLines = 0;
        return out;
    }

    void _countDropped() { if (droppedLines < UINT16_MAX) { droppedLines++; } }
};
