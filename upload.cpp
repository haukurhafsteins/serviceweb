#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_http_server.h"

#define BOUNDARY_MAX_LEN 100
#define BUFFSIZE 2048
#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + 64)

const static char *TAG = "spiffs";

static char boundary[BOUNDARY_MAX_LEN];

esp_err_t file_upload_handler(httpd_req_t *req)
{
    char filepath[FILE_PATH_MAX];
    FILE *f = NULL;
    esp_err_t res = ESP_OK;

    // Extract filename from the URI
    // req->uri will contain the path, e.g., /upload/filename.txt
    const char *uri_start = req->uri + strlen("/upload/"); // Skip the /upload/ part to get the filename
    if (*uri_start == '\0')
    {
        // Handle case where no filename is provided in the URL
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Filename is required");
        ESP_LOGE(TAG, "Filename is required, but not provided in the URL: %s", req->uri);
        return ESP_FAIL;
    }

    // Ensure the filename does not exceed our buffer size and construct the full path
    if (snprintf(filepath, sizeof(filepath), "/spiffs/%s", uri_start) >= sizeof(filepath))
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Filename is too long");
        return ESP_FAIL;
    }

    static char buf[BUFFSIZE];
    int received = 0;
    int total_received = 0;
    bool boundary_found = false;

    if (!boundary_found)
    {
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
    }

    if (!boundary_found)
    {
        ESP_LOGE(TAG, "Boundary not found in content type");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    boundary_found = false;

    // Delete the file if it already exists
    unlink(filepath);

    // Open the file for writing
    f = fopen(filepath, "w");
    if (!f)
    {
        ESP_LOGE(TAG, "Failed to open file for writing");
        return ESP_FAIL;
    }

    bool is_header = true;

    printf("Receiving file %s\n", filepath);

    while (1)
    {
        received = httpd_req_recv(req, buf, BUFFSIZE);
        if (received <= 0)
        {
            if (received == HTTPD_SOCK_ERR_TIMEOUT)
            {
                continue;
            }
            else if (received == 0)
            {
                httpd_resp_sendstr(req, "File uploaded successfully");
                ESP_LOGI(TAG, "File reception complete");
                break;
            }
            else
            {
                ESP_LOGE(TAG, "File reception failed: %d", received);
                break;
            }
        }

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
            fwrite(data_start, 1, received, f);
            printf("Wrote %d bytes\n", received);
        }
    }

    fclose(f);
    return res;
}

