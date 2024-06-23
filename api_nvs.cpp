#include <stdio.h>
#include <string.h>
#include <nvs_flash.h>
#include "esp_http_server.h"
#include "esp_err.h"
#include "esp_log.h"
#include "api_priv.hpp"

static const char* TAG = "API_NVS";

esp_err_t api_nvs(httpd_req_t* req)
{
    size_t len = httpd_req_get_url_query_len(req) + 1;
    if (len <= 1)
    {
        ESP_LOGE(TAG, "No query specified");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No query specified");
        return ESP_FAIL;
    }
    char query[len + 10];
    char cmd[32];
    httpd_req_get_url_query_str(req, query, len);
    if (ESP_OK != httpd_query_key_value(query, "cmd", cmd, sizeof(cmd)))
    {
        ESP_LOGE(TAG, "No cmd specified");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No cmd specified");
        return ESP_FAIL;
    }

    char nvsnamespace[32];
    if (ESP_OK != httpd_query_key_value(query, "namespace", nvsnamespace, sizeof(nvsnamespace)))
    {
        ESP_LOGE(TAG, "No namespace specified");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No namespace specified");
        return ESP_FAIL;
    }

    if (strcmp(cmd, "erase") == 0)
    {
        char keyval[64];
        if (ESP_OK == httpd_query_key_value(query, "key", keyval, sizeof(keyval)))
        {
            nvs_handle_t h;
            esp_err_t err = nvs_open(nvsnamespace, NVS_READWRITE, &h);
            if (err == ESP_OK)
            {
                ESP_LOGW(TAG, "Erasing NVS parameter %s", keyval);
                ESP_ERROR_CHECK(nvs_erase_key(h, keyval));
                ESP_ERROR_CHECK(nvs_commit(h));
                nvs_close(h);
                httpd_resp_sendstr(req, "OK");
                return ESP_OK;
            }

            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Error opening nvs");
            ESP_LOGE(TAG, "Error opening nvs (%d)", err);
        }
    }
    return ESP_FAIL;
}
