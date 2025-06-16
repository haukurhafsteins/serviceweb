#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include <string.h>
#include "api_priv.hpp"

#define TAG "OTA_UPDATE"
#define BOUNDARY_MAX_LEN 100
#define BUFFSIZE 2048

static char boundary[BOUNDARY_MAX_LEN];

esp_err_t ota_post_handler(httpd_req_t *req)
{
    char buf[BUFFSIZE];
    int received = 0;

    if (!_get_boundary(req, boundary, sizeof(boundary)))
        return ESP_FAIL;

    esp_ota_handle_t ota_handle;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);

    ESP_LOGI(TAG, "OTA Update partition: %s", update_partition->label);

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
                ESP_LOGE(TAG, "Failed to write OTA data: %s", esp_err_to_name(err));
                httpd_resp_send_500(req);
                return ESP_FAIL;
            }
        }
    }

    if (esp_ota_end(ota_handle) != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    if (esp_ota_set_boot_partition(update_partition) != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Update succeeded!");
    httpd_resp_sendstr(req, "Update succeeded!");

    return ESP_OK;
}

esp_err_t web_post_handler(httpd_req_t *req)
{
    const esp_partition_t *web_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, "web");
    if (!web_partition) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Web partition not found");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Web partition found: size=%ld, content_len=%d", web_partition->size, req->content_len);

    // Erase partition
    esp_err_t err = esp_partition_erase_range(web_partition, 0, web_partition->size);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Erase failed");
        return err;
    }

    // Buffers
    char buf[1024];
    size_t total_written = 0;
    size_t max_file_size = web_partition->size;

    // === STEP 1: Skip headers manually ===
    bool found_file_start = false;
    int received;
    while (!found_file_start && (received = httpd_req_recv(req, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < received - 4; ++i) {
            // Look for two newlines that mark the end of the headers
            if ((buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n') ||
                (buf[i] == '\n' && buf[i+1] == '\n')) {
                int header_end = i + ((buf[i] == '\r') ? 4 : 2);
                int body_len = received - header_end;
                if (body_len > 0) {
                    err = esp_partition_write(web_partition, total_written, buf + header_end, body_len);
                    if (err != ESP_OK) goto write_fail;
                    total_written += body_len;
                }
                found_file_start = true;
                break;
            }
        }
    }

    if (!found_file_start) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to find multipart body");
        return ESP_FAIL;
    }

    // === STEP 2: Continue receiving and writing until max_file_size ===
    while (total_written < max_file_size) {
        received = httpd_req_recv(req, buf, sizeof(buf));
        if (received <= 0) break;

        // Avoid writing more than file size
        int bytes_to_write = received;
        if (total_written + bytes_to_write > max_file_size)
            bytes_to_write = max_file_size - total_written;

        err = esp_partition_write(web_partition, total_written, buf, bytes_to_write);
        if (err != ESP_OK) goto write_fail;

        ESP_LOGI(TAG, "Wrote %d bytes to partition at offset %d", bytes_to_write, total_written);

        total_written += bytes_to_write;
        if (bytes_to_write < received) break;  // probably just hit multipart trailer
    }

    ESP_LOGI(TAG, "Upload complete: %d bytes", total_written);
    httpd_resp_sendstr(req, "Upload complete");
    return ESP_OK;

write_fail:
    ESP_LOGE(TAG, "Write failed at %d", total_written);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
    return err;
}

// esp_err_t web_post_handler(httpd_req_t *req)
// {
//     char buf[BUFFSIZE];
//     int received = 0;

//     if (!_get_boundary(req, boundary, sizeof(boundary)))
//         return ESP_FAIL;

//     const esp_partition_t *update_partition = NULL;

//     update_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, "web");

//     if (!update_partition)
//     {
//         ESP_LOGE(TAG, "Partition not found");
//         httpd_resp_send_500(req);
//         return ESP_FAIL;
//     }

//     ESP_LOGI(TAG, "Updating partition: %s", update_partition->label);

//     esp_err_t err = esp_partition_erase_range(update_partition, 0, update_partition->size);
//     if (err != ESP_OK)
//     {
//         ESP_LOGE(TAG, "Failed to erase web partition: %s", esp_err_to_name(err));
//         httpd_resp_send_500(req);
//         return ESP_FAIL;
//     }

//     // Write the received data directly to the web partition
//     bool is_header = true;
//     int offset = 0;
//     while ((received = httpd_req_recv(req, buf, BUFFSIZE)) > 0)
//     {
//         char *data_start = buf;
//         char *data_end = NULL;

//         if (is_header)
//         {
//             data_start = strstr(buf, boundary);
//             if (data_start)
//             {
//                 data_start += strlen(boundary);
//                 data_start = strstr(data_start, "\r\n\r\n");
//                 if (data_start)
//                 {
//                     data_start += 4;   // Move past the "\r\n\r\n" to the binary data start
//                     is_header = false; // subsequent segments are data

//                     // Check if end boundary is also in the buffer
//                     data_end = strstr(data_start, boundary);
//                     if (data_end)
//                     {
//                         received = data_end - data_start;
//                     }
//                     else
//                     {
//                         received -= (data_start - buf);
//                     }
//                 }
//             }
//         }
//         else
//         {
//             // Check for end boundary in buffer
//             data_end = strstr(buf, boundary);
//             if (data_end)
//             {
//                 received = data_end - buf;
//                 is_header = true; // reset for next file (if multi file upload)
//             }
//             data_start = buf;
//         }

//         if (data_start && received > 0)
//         {
//             err = esp_partition_write(update_partition, offset, buf, received);
//             if (err != ESP_OK)
//             {
//                 ESP_LOGE(TAG, "Failed to write data to partition %s : %s", update_partition->label, esp_err_to_name(err));
//                 httpd_resp_send_500(req);
//                 return ESP_FAIL;
//             }
//         }
//     }

//     ESP_LOGI(TAG, "Web update succeeded!");
//     httpd_resp_sendstr(req, "Web update succeeded!");

//     return ESP_OK;
// }
esp_err_t sysmon_get_partition(httpd_req_t *req)
{
    const esp_partition_t *currentPart = esp_ota_get_running_partition();
    const esp_partition_t *nextPart = esp_ota_get_next_update_partition(currentPart);
    esp_app_desc_t currentApp = {}, nextApp = {};

    if (!currentPart || !nextPart)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Partition not found");
        return ESP_FAIL;
    }

    if (esp_ota_get_partition_description(currentPart, &currentApp) != ESP_OK ||
        esp_ota_get_partition_description(nextPart, &nextApp) != ESP_OK)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read partition description");
        return ESP_FAIL;
    }

    currentApp.project_name[31] = 0;
    currentApp.version[15] = 0;
    nextApp.project_name[31] = 0;
    nextApp.version[15] = 0;

    const int bufsize = 512;
    char *buf = (char *)calloc(bufsize, sizeof(char));
    if (!buf)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }

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
            \"size\": %ld,\
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
