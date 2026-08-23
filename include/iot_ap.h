/**
 * ESP32 generic firmware (Arduino based)
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

#include "Arduino.h"
#include <DNSServer.h>
#include <WebServer.h>

// *****************************************************************************

/**
 * SoftAP + captive-portal first-time provisioning.
 *
 * Iot::begin() calls run() when no WiFi SSID is seeded in NVS: the device
 * opens its own open setup WiFi network and serves a small web form to
 * accept the bootstrap values seedCredentials() would otherwise take from
 * build-time defines. See docs/concepts.md, "First-time provisioning: SoftAP
 * + captive portal".
 */
class IotAp
{
public:
    // disallow copying & assignment
    IotAp(const IotAp&) = delete;
    IotAp& operator=(const IotAp&) = delete;

    IotAp();

    /**
     * Run a blocking SoftAP + captive-portal provisioning session.
     *
     * Never returns: a successful form submit ends in seedCredentials() +
     * iot.restart(); an idle timeout with nothing submitted ends in
     * iot.shutdown() (indefinite deep sleep, no wake timer) - only a
     * physical reset/power-cycle tries again, since nothing can change the
     * triggering "no seed data" condition in the meantime.
     */
    void run();

    /// SSID prefix; the setup AP's SSID is this prefix plus the last 4 hex
    /// characters of the device's WiFi MAC address. Default "arduino4iot-setup-".
    void setSsidPrefix(const String& prefix);

    /// How long the AP waits without an associated client before giving up
    /// and going to indefinite sleep. Default 300 s (5 min).
    void setIdleTimeout_s(int idleTimeout_s);

private:
    String _ssidPrefix;
    int _idleTimeout_s;
    DNSServer _dnsServer;
    WebServer _webServer;

    String _apSsid();
    void _startSoftAp();
    String _renderForm(const String& wifiSsid, const String& wifiPassword,
        const String& apiUrl, const String& project, const String& provisioningToken);
    void _handleRoot();
    void _handleSave();
    void _handleNotFound();
};

extern IotAp apProvisioning;
