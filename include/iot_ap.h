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
 * build-time defines. The page also shows the device id and the MAC address.
 * A device that already has credentials opens it on request, see
 * waitForButton(). See docs/concepts.md, "First-time provisioning: SoftAP
 * + captive portal".
 */
class IotAp
{
public:
    // disallow copying & assignment
    IotAp(const IotAp&) = delete;
    IotAp& operator=(const IotAp&) = delete;

    IotAp();

    /// The BOOT button of ESP32 and ESP32-S3 boards (GPIO9 on an ESP32-C3).
    static constexpr uint8_t DEFAULT_BUTTON_PIN = 0;
    static constexpr uint32_t DEFAULT_BUTTON_WINDOW_MS = 2000;

    /**
     * Run a blocking SoftAP + captive-portal provisioning session.
     *
     * Never returns: a successful form submit ends in seedCredentials() +
     * iot.restart(). An idle timeout with nothing submitted ends in
     * iot.restart() on a device that has credentials, which then resumes its
     * normal cycle, and otherwise in iot.shutdown() (indefinite deep sleep,
     * no wake timer) - only a physical reset/power-cycle tries again, since
     * nothing can change the "no seed data" condition in the meantime.
     */
    void run();

    /**
     * Wait up to @p window_ms for a button press - the setup trigger for
     * Iot::setSetupTrigger():
     *
     *   iot.setSetupTrigger([] { return apProvisioning.waitForButton(); });
     *
     * Press the button *after* the reset, within the window: a BOOT button
     * held during the reset puts the chip into its serial bootloader instead,
     * since GPIO0 is a strapping pin.
     *
     * @param pin the button's GPIO, configured as an input with pull-up
     *        (pull-down for an active-high button)
     * @param activeLevel the level of the pressed button
     * @return true as soon as the button reads pressed twice in a row
     */
    bool waitForButton(uint8_t pin = DEFAULT_BUTTON_PIN, uint8_t activeLevel = LOW,
        uint32_t window_ms = DEFAULT_BUTTON_WINDOW_MS);

    /// SSID prefix; the setup AP's SSID is this prefix plus the last 4 hex
    /// characters of the device's WiFi MAC address. Default "arduino4iot-setup-".
    void setSsidPrefix(const String& prefix);

    /// How long the AP waits without an associated client before giving up
    /// and going to indefinite sleep. Default 300 s (5 min).
    void setIdleTimeout_s(int32_t idleTimeout_s);

private:
    String _ssidPrefix;
    int32_t _idleTimeout_s;
    DNSServer _dnsServer;
    WebServer _webServer;

    String _apSsid();
    /// @return false if the access point could not be started.
    bool _startSoftAp();
    String _renderForm(const String& wifiSsid, const String& wifiPassword,
        const String& apiUrl, const String& project, const String& provisioningToken);
    void _handleRoot();
    void _handleInfo();
    void _handleSave();
    void _handleRestart();
    /// @return true if a WiFi SSID is seeded, so the device can run without the portal.
    static bool _hasCredentials();
    /// Leave the portal without changes: restart into the normal cycle if the
    /// device has credentials, otherwise sleep until a physical reset.
    void _leave();
    void _handleNotFound();
};

extern IotAp apProvisioning;
