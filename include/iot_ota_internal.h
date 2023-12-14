/**
 * ESP32 generic firmware
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#pragma once

#include "esp_http_client.h"
#include <string>
#include <map>
// do not include <Arduino.h> here, it is not compatible with esp_http_client.h,
// esp_https_ota.h and esp_ota_ops.h

// ***************************************************************************

class IotOtaInternal
{
public:
    // disallow copying & assignment
    IotOtaInternal(const IotOtaInternal&) = delete;
    IotOtaInternal& operator=(const IotOtaInternal&) = delete;

    IotOtaInternal();

    void setTimeout(int timeout_ms) { _timeout_ms = timeout_ms; }
    void setClientCert(const char * cert_pem, const char * key_pem, const char * key_password);
    void setServerCert(const char * cert_pem, bool skip_common_name_check);

    bool updateFirmwareFromUrl(std::string& oEtag, std::string& oDate, const char * url, std::map<std::string, std::string> * headerPtr);

private:
    const char * _client_cert_pem;
    const char * _client_key_pem;
    const char * _client_key_password;
    const char * _server_cert_pem;
    bool _skip_server_common_name_check;
    int _timeout_ms;

    static void _setFirmwareHttpEtag(const char* etag) noexcept;
    static void _setFirmwareHttpDate(const char* date) noexcept;

    static esp_err_t _http_client_init_cb(esp_http_client_handle_t http_client) noexcept;
    static esp_err_t _http_event_handler(esp_http_client_event_t *evt) noexcept;
};
