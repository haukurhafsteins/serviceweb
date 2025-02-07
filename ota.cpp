#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include <string.h>

#define TAG "OTA_UPDATE"
#define BOUNDARY_MAX_LEN 100
#define BUFFSIZE 2048

static char boundary[BOUNDARY_MAX_LEN];

esp_err_t ota_post_handler(httpd_req_t *req)
{
    char buf[BUFFSIZE];
    int received = 0;
    int total_received = 0;
    bool boundary_found = false;

    // Extract the boundary from the content type
    if (ESP_OK == httpd_req_get_hdr_value_str(req, "Content-Type", buf, sizeof(buf)))
    {
        char *boundary_start = strstr(buf, "boundary=");
        if (boundary_start)
        {
            snprintf(boundary, sizeof(boundary), "--%s", boundary_start + 9);
            boundary_found = true;
        }
    }

    if (!boundary_found)
    {
        ESP_LOGE(TAG, "Boundary not found in content type");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);

    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start OTA");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    bool is_header = true;

    while ((received = httpd_req_recv(req, buf, BUFFSIZE)) > 0)
    {
        total_received += received;
        char *data_start = NULL;
        char *data_end = NULL;

        // If we're at the header (first multipart segment)
        if (is_header)
        {
            data_start = strstr(buf, boundary);
            if (data_start)
            {
                data_start += strlen(boundary);
                data_start = strstr(data_start, "\r\n\r\n");
                if (data_start)
                {
                    data_start += 4;   // Move past the "\r\n\r\n" to the binary data start
                    is_header = false; // subsequent segments are data

                    // Check if end boundary is also in the buffer
                    data_end = strstr(data_start, boundary);
                    if (data_end)
                    {
                        received = data_end - data_start;
                    }
                    else
                    {
                        received -= (data_start - buf);
                    }
                }
            }
        }
        else
        {
            // Check for end boundary in buffer
            data_end = strstr(buf, boundary);
            if (data_end)
            {
                received = data_end - buf;
                is_header = true; // reset for next file (if multi file upload)
            }
            data_start = buf;
        }

        if (data_start && received > 0)
        {
            err = esp_ota_write(ota_handle, data_start, received);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to write OTA data");
                httpd_resp_send_500(req);
                return ESP_FAIL;
            }
        }
    }

    if (esp_ota_end(ota_handle) != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_end failed!");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    if (esp_ota_set_boot_partition(update_partition) != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed!");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Update succeeded. Rebooting...");
    httpd_resp_sendstr(req, "Update succeeded. Rebooting...");

    esp_restart();

    return ESP_OK;
}

esp_err_t sysmon_get_partition(httpd_req_t *req)
{
    const esp_partition_t *currentPart = esp_ota_get_running_partition();
    const esp_partition_t *nextPart = esp_ota_get_next_update_partition(currentPart);
    esp_app_desc_t currentApp = {}, nextApp = {};

    esp_ota_get_partition_description(currentPart, &currentApp);
    esp_ota_get_partition_description(nextPart, &nextApp);
    currentApp.project_name[31] = 0;
    currentApp.version[15] = 0;
    nextApp.project_name[31] = 0;
    nextApp.version[15] = 0;
    
    const int bufsize = 512;
    char *buf = (char *)calloc(bufsize, sizeof(char));
    snprintf(buf, bufsize, "{\
        \"current\": {\
            \"type\": \"%X\",\
            \"size\": %ld,\
            \"label\": \"%s\",\
            \"appName\": \"%s\",\
            \"appVersion\": \"%s\"\
        },\
        \"next\": {\
            \"type\": \"%X\",\
            \"size\": \"%ld\",\
            \"label\": \"%s\",\
            \"appName\": \"%s\",\
            \"appVersion\": \"%s\"\
        }\
    }",
             currentPart->type,
             currentPart->size,
             currentPart->label,
             currentApp.project_name,
             currentApp.version,
             nextPart->type,
             nextPart->size,
             nextPart->label,
             nextApp.project_name,
             nextApp.version);

    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    free(buf);

    return ESP_OK;
}
