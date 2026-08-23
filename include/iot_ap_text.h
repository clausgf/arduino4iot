/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

#include <string>

// *****************************************************************************
// Pure text helper, intentionally free of Arduino/HTTP dependencies so it can
// be unit-tested on the native platform (see test/test_ap_text). Used by
// IotAp's captive-portal WebServer to answer known OS connectivity-probe
// requests truthfully (i.e. "you have real internet"), so the OS does not
// launch its own restricted captive-portal mini-browser - see docs/concepts.md.
// *****************************************************************************

namespace iot_ap_text
{

/**
 * A canned response for a known OS connectivity-probe request.
 */
struct CaptiveProbeResponse
{
    int status;
    const char* contentType;
    const char* body;
};

/**
 * Look up the canned "no captive portal here" response for a known OS
 * connectivity-probe path (Android, Apple, Windows NCSI, Firefox).
 *
 * @param path the request path, e.g. "/generate_204"
 * @param out set to the matching response if found
 * @return true if @p path is a known probe path (out is set), false otherwise
 *         (the caller should fall back to the captive-portal redirect-all
 *         behavior for any other path)
 */
inline bool lookupCaptiveProbeResponse(const std::string& path, CaptiveProbeResponse& out)
{
    static const struct { const char* path; CaptiveProbeResponse response; } table[] = {
        { "/generate_204",              { 204, "text/plain", "" } },
        { "/gen_204",                   { 204, "text/plain", "" } },
        { "/hotspot-detect.html",       { 200, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>" } },
        { "/library/test/success.html", { 200, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>" } },
        { "/connecttest.txt",           { 200, "text/plain", "Microsoft Connect Test" } },
        { "/ncsi.txt",                  { 200, "text/plain", "Microsoft NCSI" } },
        { "/success.txt",               { 200, "text/plain", "success\n" } },
    };
    for (const auto& entry : table)
    {
        if (path == entry.path)
        {
            out = entry.response;
            return true;
        }
    }
    return false;
}

} // namespace iot_ap_text
