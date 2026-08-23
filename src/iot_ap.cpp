/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#include "iot_ap.h"

#include <WiFi.h>
#include <Preferences.h>

#include "iot.h"
#include "iot_ap_text.h"

// *****************************************************************************

IotAp apProvisioning;

// *****************************************************************************

IotAp::IotAp() :
    _ssidPrefix("arduino4iot-setup-"),
    _idleTimeout_s(300)
{
}

void IotAp::setSsidPrefix(const String& prefix)
{
    _ssidPrefix = prefix;
}

void IotAp::setIdleTimeout_s(int idleTimeout_s)
{
    _idleTimeout_s = idleTimeout_s;
}

// *****************************************************************************

static String htmlEscape(const String& s)
{
    String out;
    out.reserve(s.length());
    for (size_t i = 0; i < s.length(); i++)
    {
        char c = s[i];
        switch (c)
        {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            default:   out += c;
        }
    }
    return out;
}

String IotAp::_apSsid()
{
    String mac = WiFi.macAddress(); // "AA:BB:CC:DD:EE:FF"
    mac.replace(":", "");
    return _ssidPrefix + mac.substring(mac.length() - 4);
}

void IotAp::_startSoftAp()
{
    WiFi.mode(WIFI_AP);
    // pin the AP IP to the documented QR-deep-link contract rather than
    // relying on the arduino-esp32 default (currently the same value)
    IPAddress apIp(192, 168, 4, 1);
    WiFi.softAPConfig(apIp, apIp, IPAddress(255, 255, 255, 0));
    String ssid = _apSsid();
    WiFi.softAP(ssid.c_str()); // open network, no password (by design)
    log_w("run: SoftAP '%s' started, ip=%s", ssid.c_str(), WiFi.softAPIP().toString().c_str());

    // wildcard DNS: answer every query with our own IP (captive-portal spoof)
    _dnsServer.start(53, "*", WiFi.softAPIP());

    _webServer.on("/", HTTP_GET, [this]() { _handleRoot(); });
    _webServer.on("/save", HTTP_POST, [this]() { _handleSave(); });
    _webServer.onNotFound([this]() { _handleNotFound(); });
    _webServer.begin();
}

String IotAp::_renderForm(const String& wifiSsid, const String& wifiPassword,
    const String& apiUrl, const String& project, const String& provisioningToken)
{
    String html;
    html.reserve(1024);
    html += "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
            "<title>Device setup</title></head><body>"
            "<h1>Device setup</h1>"
            "<form method=\"POST\" action=\"/save\">";
    html += "<label>WiFi SSID<br><input name=\"wifiSsid\" value=\"" + htmlEscape(wifiSsid) + "\" required></label><br><br>";
    html += "<label>WiFi password<br><input name=\"wifiPassword\" type=\"password\" value=\"" + htmlEscape(wifiPassword) + "\"></label><br><br>";
    html += "<label>API URL<br><input name=\"apiUrl\" value=\"" + htmlEscape(apiUrl) + "\" required></label><br><br>";
    html += "<label>Project<br><input name=\"project\" value=\"" + htmlEscape(project) + "\" required></label><br><br>";
    html += "<label>Provisioning token<br><input name=\"provisioningToken\" value=\"" + htmlEscape(provisioningToken) + "\" required></label><br><br>";
    html += "<button type=\"submit\">Save &amp; reboot</button>"
            "</form></body></html>";
    return html;
}

void IotAp::_handleRoot()
{
    String html = _renderForm(
        _webServer.arg("wifiSsid"), _webServer.arg("wifiPassword"),
        _webServer.arg("apiUrl"), _webServer.arg("project"), _webServer.arg("provisioningToken"));
    _webServer.send(200, "text/html", html);
}

void IotAp::_handleSave()
{
    String wifiSsid = _webServer.arg("wifiSsid");
    String wifiPassword = _webServer.arg("wifiPassword");
    String apiUrl = _webServer.arg("apiUrl");
    String project = _webServer.arg("project");
    String provisioningToken = _webServer.arg("provisioningToken");

    if (wifiSsid.isEmpty() || apiUrl.isEmpty() || project.isEmpty() || provisioningToken.isEmpty())
    {
        _webServer.send(400, "text/plain", "wifiSsid, apiUrl, project and provisioningToken are required");
        return;
    }

    // force-overwrite even a partially-seeded device (e.g. a stale apiUrl
    // left over from an old build) - iotSeedString() only writes a key that
    // is absent or forced, see seedCredentials()
    Preferences preferences;
    uint32_t storedGeneration = 0;
    if (preferences.begin("iot", true))
    {
        storedGeneration = nvramGetUInt(preferences, "seedGen", 0);
        preferences.end();
    }

    IotSeedConfig cfg{
        .wifiSsid          = wifiSsid.c_str(),
        .wifiPassword      = wifiPassword.c_str(),
        .apiUrl            = apiUrl.c_str(),
        .projectName       = project.c_str(),
        .provisioningToken = provisioningToken.c_str(),
        .seedGeneration    = storedGeneration + 1,
    };
    iot.seedCredentials(cfg);
    log_w("_handleSave: provisioned via captive portal, rebooting");

    _webServer.send(200, "text/html",
        "<!DOCTYPE html><html><body><h1>Saved</h1><p>Rebooting&hellip;</p></body></html>");
    delay(500); // let the response flush before the reboot tears down WiFi
    iot.restart();
}

void IotAp::_handleNotFound()
{
    iot_ap_text::CaptiveProbeResponse probe;
    if (iot_ap_text::lookupCaptiveProbeResponse(_webServer.uri().c_str(), probe))
    {
        _webServer.send(probe.status, probe.contentType, probe.body);
        return;
    }
    // catch-all: funnel a manually opened browser to the setup form
    _webServer.sendHeader("Location", "http://192.168.4.1/", true);
    _webServer.send(302, "text/plain", "");
}

// *****************************************************************************

void IotAp::run()
{
    _startSoftAp();

    unsigned long idleDeadline = millis() + (unsigned long)_idleTimeout_s * 1000UL;
    while ((long)(millis() - idleDeadline) < 0)
    {
        _dnsServer.processNextRequest();
        _webServer.handleClient();
        if (WiFi.softAPgetStationNum() > 0)
        {
            idleDeadline = millis() + (unsigned long)_idleTimeout_s * 1000UL;
        }
        iot.resetWatchdog();
        delay(10);
    }

    log_w("run: idle timeout with no successful provisioning, sleeping indefinitely");
    iot.shutdown(); // esp_deep_sleep_start(), no timer - never returns
}
