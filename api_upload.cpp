#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <dirent.h>
#include "esp_vfs.h"
#include <errno.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "httpss.h"

#define TAG "FILE_SERVER"

#define BOUNDARY_MAX_LEN 100
#define BUFFSIZE 512
#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + 64)

static bool get_directory_destination(httpd_req_t *req, char *destination, size_t dest_len)
{
    const int buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1)
    {
        char *buf1 = (char *)malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf1, buf_len) != ESP_OK)
        {
            ESP_LOGI(TAG, "Unable to get query string");
            free(buf1);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unable to get query string");
            return false;
        }
        else
        {
            if (httpd_query_key_value(buf1, "dir", destination, dest_len) != ESP_OK)
            {
                ESP_LOGI(TAG, "Destination not found in URL query %s", buf1);
                free(buf1);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Destination not found in URL query");
                return false;
            }
        }
        free(buf1);
    }
    else
    {
        ESP_LOGI(TAG, "Query not found");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Query not found");
        return false;
    }
    return true;
}

static bool get_boundary(httpd_req_t *req, char *boundary, size_t boundary_len)
{
    static char buf[100];
    if (ESP_OK == httpd_req_get_hdr_value_str(req, "Content-Type", buf, sizeof(buf)))
    {
        char *boundary_start = strstr(buf, "boundary=");
        if (boundary_start)
        {
            snprintf(boundary, boundary_len, "--%s", boundary_start + 9);
            return true;
        }
    }

    ESP_LOGE(TAG, "Boundary not found in content type");
    httpd_resp_send_500(req);
    return false;
}

esp_err_t file_upload_handler(httpd_req_t *req)
{
    char filepath[FILE_PATH_MAX];
    esp_err_t res = ESP_OK;
    char destination[64];
    char filename[64];
    char boundary[BOUNDARY_MAX_LEN];

    if (!get_directory_destination(req, destination, sizeof(destination)))
    {
        return ESP_FAIL;
    }

    static char buf[BUFFSIZE];
    int received = 0;

    if (!get_boundary(req, boundary, sizeof(boundary)))
    {
        return ESP_FAIL;
    }

    bool is_header = true;
    bool file_open = false;
    FILE *f = NULL;

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
                ESP_LOGI(TAG, "File reception complete %d", file_open);
                if (file_open)
                {
                    printf("Closing file\n");
                    fclose(f);
                    file_open = false;
                }
                httpd_resp_sendstr(req, "File(s) uploaded successfully");
                break;
            }
            else
            {
                ESP_LOGE(TAG, "File reception failed: %d", received);
                res = ESP_FAIL;
                break;
            }
        }

        char *data_start = buf;
        char *data_end = NULL;

        while (data_start < buf + received)
        {
            if (is_header)
            {
                char *boundary_start = strstr(data_start, boundary);
                if (boundary_start)
                {
                    data_start = boundary_start + strlen(boundary);
                    char *header_end = strstr(data_start, "\r\n\r\n");
                    if (header_end)
                    {
                        data_start = header_end + 4; // Move past the "\r\n\r\n" to the binary data start
                        is_header = false;           // subsequent segments are data

                        // Extract filename from content-disposition
                        char *header_start = strstr(buf, "Content-Disposition: form-data; name=\"files\"; filename=\"");
                        if (header_start)
                        {
                            header_start += strlen("Content-Disposition: form-data; name=\"files\"; filename=\"");
                            char *filename_end = strchr(header_start, '"');
                            if (filename_end)
                            {
                                int filename_len = filename_end - header_start;
                                if (filename_len < sizeof(filename))
                                {
                                    strncpy(filename, header_start, filename_len);
                                    filename[filename_len] = '\0';
                                    printf("Filename: %s\n", filename);

                                    if (snprintf(filepath, sizeof(filepath), "%s/%s", destination, filename) >= sizeof(filepath))
                                    {
                                        ESP_LOGE(TAG, "Filename is too long");
                                        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Filename is too long");
                                        res = ESP_FAIL;
                                        break;
                                    }

                                    char *slash = strrchr(filepath, '/');
                                    if (slash)
                                    {
                                        *slash = '\0';
                                        if (mkdir(filepath, 0777) != 0)
                                        {
                                            if (errno != EEXIST)
                                            {
                                                ESP_LOGE(TAG, "Failed to create folder %s", filepath);
                                                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create folder");
                                                res = ESP_FAIL;
                                                break;
                                            }
                                        }
                                        *slash = '/';
                                    }

                                    unlink(filepath);
                                    f = fopen(filepath, "w");
                                    if (!f)
                                    {
                                        ESP_LOGE(TAG, "Failed to open file for writing");
                                        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open file for writing");
                                        res = ESP_FAIL;
                                        break;
                                    }
                                    file_open = true;
                                    printf("File opened: %s %d\n", filepath, file_open);
                                }
                            }
                        }
                    }
                }
                else
                {
                    data_start += received; // Move to end of buffer if no boundary is found
                }
            }
            else
            {
                data_end = strstr(data_start, boundary);
                if (data_end)
                {
                    if (file_open)
                    {
                        fwrite(data_start, 1, data_end - data_start, f);
                        fclose(f);
                        file_open = false;
                        printf("File closed in second loop\n");
                    }
                    data_start = data_end + strlen(boundary);
                    is_header = true;
                }
                else
                {
                    if (file_open)
                    {
                        fwrite(data_start, 1, buf + received - data_start, f);
                    }
                    data_start = buf + received; // Move to end of buffer
                }
            }
        }
    }

    if (file_open)
    {
        printf("Closing file at end\n");
        fclose(f);
    }

    return res;
}
