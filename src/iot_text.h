/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
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
 * {device}, {project} and {board} are substituted.
 *
 * @param baseUrl the API base URL ending with a slash, e.g. "https://host/api/"
 * @param apiPath the path relative to the base URL, e.g. "/foo/{device}/bar"
 * @param project the value substituted for {project}
 * @param device the value substituted for {device}
 * @param board the value substituted for {board}, e.g. the IOT_BOARD_ID build
 *        default; empty if the board id is undefined
 * @return the full URL, e.g. "https://host/api/foo/e32_123/bar"
 */
inline std::string buildApiUrl(const std::string& baseUrl, const std::string& apiPath,
    const std::string& project, const std::string& device, const std::string& board = "")
{
    std::string path = apiPath;
    if (!path.empty() && path.front() == '/')
    {
        path.erase(0, 1);
    }
    std::string url = baseUrl + path;
    replaceAll(url, "{device}", device);
    replaceAll(url, "{project}", project);
    replaceAll(url, "{board}", board);
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

/**
 * Format a hardware address given as hex digits ("aabbccddeeff", as
 * IotNetwork::macAddress() returns it) the usual way, "aa:bb:cc:dd:ee:ff".
 * Inserts a colon after every second character; does not validate.
 */
inline std::string formatMacAddress(const std::string& hex)
{
    std::string out;
    out.reserve(hex.size() + hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i++)
    {
        if (i > 0 && i % 2 == 0)
        {
            out += ':';
        }
        out += hex[i];
    }
    return out;
}

/**
 * The host part of @p url, without scheme, credentials, port and path:
 * "https://user@[::1]:8443/api" -> "::1", "http://host:80/x" -> "host".
 */
inline std::string hostOfUrl(const std::string& url)
{
    size_t start = url.find("://");
    start = (start == std::string::npos) ? 0 : start + 3;
    const size_t end = url.find_first_of("/?#", start);
    std::string authority = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    const size_t at = authority.rfind('@');
    if (at != std::string::npos) { authority.erase(0, at + 1); }
    if (!authority.empty() && authority[0] == '[')
    {
        const size_t close = authority.find(']');
        return authority.substr(1, close == std::string::npos ? std::string::npos : close - 1);
    }
    const size_t colon = authority.find(':');
    return colon == std::string::npos ? authority : authority.substr(0, colon);
}

/**
 * The release part of a version string: "v5.4.1-1-g2f7dcd862a-dirty" ->
 * "5.4.1". Drops a leading "v" and everything from the first "-".
 */
inline std::string releaseVersion(const std::string& version)
{
    const size_t start = (!version.empty() && version[0] == 'v') ? 1 : 0;
    const size_t dash = version.find('-', start);
    return version.substr(start, dash == std::string::npos ? std::string::npos : dash - start);
}

/**
 * The firmware identifier: what identifies a build in one string, empty parts
 * left out, e.g.
 *   "1.4.0-3-gcce20b9 2026-11-10 13:03 ARDUINO 3.2.0 IDF 5.4.1 IOT 4.0.0-5-ga57b942"
 * "sec <n>" is appended only for a secure version other than 0.
 */
inline std::string firmwareId(const std::string& firmwareVersion, const std::string& firmwareDate,
    const std::string& arduinoVersion, const std::string& idfVersion,
    const std::string& iotVersion, uint32_t secureVersion)
{
    std::string id;
    auto append = [&id](const char* label, const std::string& value)
    {
        if (value.empty()) { return; }
        if (!id.empty()) { id += ' '; }
        if (label != nullptr) { id += label; id += ' '; }
        id += value;
    };
    append(nullptr, firmwareVersion);
    append(nullptr, firmwareDate);
    append("ARDUINO", arduinoVersion);
    append("IDF", idfVersion);
    append("IOT", iotVersion);
    if (secureVersion != 0) { append("sec", std::to_string(secureVersion)); }
    return id;
}

} // namespace iot_text
