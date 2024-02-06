#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_spiffs.h"
#include "esp_http_server.h"

#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + 64)
#define BUF_SIZE (2048)

const static char *TAG = "spiffs";

esp_err_t spiffs_upload_handler(httpd_req_t *req) {
    char filepath[FILE_PATH_MAX];
    FILE* f = NULL;
    esp_err_t res = ESP_OK;

    printf("#### spiffs_upload_handler: %s\n", req->uri);
    
    // Extract filename from the URI
    // req->uri will contain the path, e.g., /upload/filename.txt
    const char *uri_start = req->uri + strlen("/upload/"); // Skip the /upload/ part to get the filename
    if (*uri_start == '\0') {
        // Handle case where no filename is provided in the URL
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Filename is required");
        ESP_LOGE(TAG, "Filename is required, but not provided in the URL: %s", req->uri);
        return ESP_FAIL;
    }

    // Ensure the filename does not exceed our buffer size and construct the full path
    if (snprintf(filepath, sizeof(filepath), "/spiffs/%s", uri_start) >= sizeof(filepath)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Filename is too long");
        return ESP_FAIL;
    }

    // Open the file for writing
    f = fopen(filepath, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for writing");
        return ESP_FAIL;
    }

    printf("Writing file %s, length: %d\n", filepath, req->content_len);

    int total_len = req->content_len;
    int received = 0;
    char *buf = (char*)malloc(BUF_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate memory for buffer");
        fclose(f);
        return ESP_FAIL;
    }
    while (received < total_len) {
        int cur_len = total_len - received > BUF_SIZE ? BUF_SIZE : total_len - received;
        int read_len = httpd_req_recv(req, buf, cur_len);
        printf("Received %d bytes, %d left\n", read_len, total_len - received);
        if (read_len <= 0) {
            if (read_len == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            res = ESP_FAIL;
            goto cleanup;
        }
        fwrite(buf, 1, read_len, f);
        received += read_len;
    }
    httpd_resp_sendstr(req, "File uploaded successfully");
cleanup:
    fclose(f);
    free(buf);
    return res;
}
