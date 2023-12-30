/**
 * ESP32 generic firmware
 * Copyright (c) 2023 clausgf@github. See LICENSE.md for legal information.
 */

#include "esp_log.h"
#include "esp_system.h"
#include "esp_tls.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"

#include "iot_ota_internal.h"

// ***************************************************************************

static const char * tag = "IotOtaInternal";

// ***************************************************************************

IotOtaInternal::IotOtaInternal():
    _client_cert_pem(nullptr),
    _client_key_pem(nullptr),
    _client_key_password(nullptr),
    _server_cert_pem(nullptr),
    _skip_server_common_name_check(false),
    _timeout_ms(10000)
{
}

// ***************************************************************************

void IotOtaInternal::setClientCert(const char * cert_pem, const char * key_pem, const char * key_password)
{
    _client_cert_pem = cert_pem;
    _client_key_pem = key_pem;
    _client_key_password = key_password;
}

void IotOtaInternal::setServerCert(const char * cert_pem, bool skip_common_name_check)
{
    _server_cert_pem = cert_pem;
    _skip_server_common_name_check = skip_common_name_check;
}

// ***************************************************************************

static std::map<std::string, std::string> * _headerPtr = nullptr;
static std::string _etag = "";
static std::string _lastModified = "";

static bool equalsIgnoreCase(const char * a, const char * b)
{
    if (a == nullptr || b == nullptr)
    {
        return false;
    }
    return strcasecmp(a, b) == 0;
}

static esp_err_t _http_client_init_cb(esp_http_client_handle_t http_client) noexcept
{
    //IotOtaInternal *ota_ptr = static_cast<IotOtaInternal *>(http_client->user_data);
    esp_err_t err = ESP_OK;
    for ( auto const& header : *_headerPtr )
    {
        err = esp_http_client_set_header(http_client, header.first.c_str(), header.second.c_str());
        ESP_LOGI(tag, "  set header {%s: %s} -> %d", header.first.c_str(), header.second.c_str(), err);
        if (err != ESP_OK) {
            ESP_LOGE(tag, "Failed to set header %s: %s", header.first.c_str(), header.second.c_str());
            return err;
        }
    }
    return err;
}

static esp_err_t _http_event_handler(esp_http_client_event_t *evt) noexcept
{
    //IotOtaInternal *ota_ptr = static_cast<IotOtaInternal *>(evt->user_data);

    switch(evt->event_id) {
        case HTTP_EVENT_ON_HEADER:
            if (equalsIgnoreCase("etag", evt->header_key))
            {
                _etag = evt->header_value; // copy value
            }
            if (equalsIgnoreCase("last-modified", evt->header_key))
            {
                _lastModified = evt->header_value; // copy value
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

// ***************************************************************************

bool IotOtaInternal::updateFirmwareFromUrl(std::string& oEtag, std::string& oDate, const char * url, std::map<std::string, std::string> * headerPtr)
{
    _headerPtr = headerPtr;
    ESP_LOGW(tag, "OTA updating firmware from %s", url);

    esp_http_client_config_t http_cfg;
    memset(&http_cfg, 0, sizeof(http_cfg));
    http_cfg.user_data = this;
    http_cfg.event_handler = _http_event_handler;
    http_cfg.url = url;
    // TLS
    http_cfg.client_cert_pem = _client_cert_pem;
    http_cfg.client_key_pem = _client_key_pem;
    http_cfg.client_key_password = _client_key_password;
    http_cfg.cert_pem = _server_cert_pem;
    http_cfg.skip_cert_common_name_check = _skip_server_common_name_check;
    http_cfg.timeout_ms = _timeout_ms;
    http_cfg.keep_alive_enable = true;

    esp_https_ota_config_t ota_config = {
        .http_config = &http_cfg,
        .http_client_init_cb = _http_client_init_cb,
        .bulk_flash_erase = false,
        .partial_http_download = false,
        .max_http_request_size = 0,
        // .partial_http_download = true,
        // .max_http_request_size = CONFIG_EXAMPLE_HTTP_REQUEST_SIZE,
    };

    // begin OTA update
    _etag = "";
    _lastModified = "";
    esp_https_ota_handle_t https_ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &https_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(tag, "OTA begin failed");
        return false;
    }

    // esp_app_desc_t app_desc;
    // err = esp_https_ota_get_img_desc(https_ota_handle, &app_desc);
    // if (err != ESP_OK) {
    //     ESP_LOGE(tag, TAG, "esp_https_ota_read_img_desc failed. Aborting OTA.");
    //     esp_https_ota_abort(https_ota_handle);
    //     return false;
    // }
    // err = validate_image_header(&app_desc);

    int image_size = esp_https_ota_get_image_size(https_ota_handle);
    do {
        err = esp_https_ota_perform(https_ota_handle);
        ESP_LOGD(tag, "OTA Image bytes read: %d/%d", esp_https_ota_get_image_len_read(https_ota_handle), image_size);
    } while (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

    if (esp_https_ota_is_complete_data_received(https_ota_handle) != true) {
        ESP_LOGE(tag, "OTA data incomplete. Aborting.");
        esp_https_ota_abort(https_ota_handle);
        return false;
    }

    esp_err_t ota_finish_err = esp_https_ota_finish(https_ota_handle);
    if (ota_finish_err == ESP_ERR_OTA_VALIDATE_FAILED) {
        ESP_LOGE(tag, "OTA image validation failed, image is corrupted");
        return false;
    }

    if ((err != ESP_OK) || (ota_finish_err != ESP_OK)) {
        ESP_LOGE(tag, "OTA update failed 0x%x/0x%x", err, ota_finish_err);
        return false;
    }

    ESP_LOGI(tag, "OTA update successful: etag=%s last-modified=%s", _etag.c_str(), _lastModified.c_str());
    oEtag = _etag;
    oDate = _lastModified;
    return true;
}

// ***************************************************************************
