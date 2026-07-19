/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

#include <cstddef>
#include <string>

// *****************************************************************************
// Pure text helpers, intentionally free of Arduino/HTTP dependencies so they
// can be unit-tested on the native platform (see test/test_text).
// *****************************************************************************

namespace iot_text
{

/**
 * Replace every occurrence of @p from with @p to in @p str (in place).
 */
inline void replaceAll(std::string& str, const std::string& from, const std::string& to)
{
    if (from.empty())
    {
        return;
    }
    size_t pos = 0;
    while ((pos = str.find(from, pos)) != std::string::npos)
    {
        str.replace(pos, from.size(), to);
        pos += to.size();
    }
}

/**
 * Build the effective API URL for a given path.
 *
 * A leading slash of @p apiPath is dropped, then @p baseUrl (which is expected
 * to end with a slash) and the path are concatenated, and the placeholders
 * {device} and {project} are substituted.
 *
 * @param baseUrl the API base URL ending with a slash, e.g. "https://host/api/"
 * @param apiPath the path relative to the base URL, e.g. "/foo/{device}/bar"
 * @param project the value substituted for {project}
 * @param device the value substituted for {device}
 * @return the full URL, e.g. "https://host/api/foo/e32_123/bar"
 */
inline std::string buildApiUrl(const std::string& baseUrl, const std::string& apiPath,
    const std::string& project, const std::string& device)
{
    std::string path = apiPath;
    if (!path.empty() && path.front() == '/')
    {
        path.erase(0, 1);
    }
    std::string url = baseUrl + path;
    replaceAll(url, "{device}", device);
    replaceAll(url, "{project}", project);
    return url;
}

/**
 * Determine the length of the next log chunk to send.
 *
 * The server rejects bodies larger than its size limit, so a large log body is
 * split into chunks. This function returns how many bytes starting at @p offset
 * form the next chunk: at most @p maxChunkSize bytes, preferring to split right
 * after the last newline within the limit. If no newline is found within the
 * limit (a single very long line), a hard split at @p maxChunkSize is returned.
 *
 * The result is always at least 1 (when @p offset < @p bodyLen), so iterating
 * over the body terminates.
 *
 * @return the number of bytes in the next chunk, or 0 if offset >= bodyLen
 */
inline size_t nextLogChunkLength(const char* body, size_t bodyLen, size_t offset, size_t maxChunkSize)
{
    if (offset >= bodyLen)
    {
        return 0;
    }
    size_t remaining = bodyLen - offset;
    if (remaining <= maxChunkSize)
    {
        return remaining;
    }
    // prefer splitting after the last newline within the limit
    for (size_t i = maxChunkSize; i > 0; i--)
    {
        if (body[offset + i - 1] == '\n')
        {
            return i;
        }
    }
    // no newline within the limit: hard split
    return maxChunkSize;
}

} // namespace iot_text
