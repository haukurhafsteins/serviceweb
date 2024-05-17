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

static FILE *create_file(httpd_req_t *req, const char *filename, char *destination)
{
    char filepath[FILE_PATH_MAX];
    if (snprintf(filepath, sizeof(filepath), "%s/%s", destination, filename) >= sizeof(filepath))
    {
        ESP_LOGE(TAG, "Filename is too long");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Filename is too long");
        return NULL;
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
                return NULL;
            }
        }
        *slash = '/';
    }

    unlink(filepath);
    FILE *f = fopen(filepath, "w");
    if (!f)
    {
        ESP_LOGE(TAG, "Failed to open file for writing");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open file for writing");
        return NULL;
    }
    printf("File opened: %s\n", filepath);
    return f;
}

static bool extract_filename_from_content_disposition(httpd_req_t req, char *buf, char *filename, size_t filename_len)
{
    char *header_start = strstr(buf, "Content-Disposition: form-data; name=\"files\"; filename=\"");
    if (header_start)
    {
        header_start += strlen("Content-Disposition: form-data; name=\"files\"; filename=\"");
        char *filename_end = strchr(header_start, '"');
        if (filename_end)
        {
            int len = filename_end - header_start;
            if (len < filename_len)
            {
                strncpy(filename, header_start, len);
                filename[len] = '\0';
                printf("Filename: %s\n", filename);
                return true;
            }
        }
    }

    ESP_LOGE(TAG, "Filename not found in content disposition");
    httpd_resp_send_500(&req);
    return false;
}

static bool write_to_file(FILE *f, char *data_start, char *data_end, char *boundary, char *buf, int received)
{
    data_end = strstr(data_start, boundary);
    if (data_end)
    {
        fwrite(data_start, 1, data_end - data_start, f);
        fclose(f);
        ESP_LOGI(TAG, "File reception complete");
        return false;
    }
    fwrite(data_start, 1, buf + received - data_start, f);
    printf("Wrote %d bytes to file\n", buf + received - data_start);
    return true;
}

static void file_upload_complete(httpd_req_t *req)
{
    ESP_LOGI(TAG, "File upload complete, last file closed");
    httpd_resp_sendstr(req, "File uploaded successfully");
}

esp_err_t file_upload_handler(httpd_req_t *req)
{
    esp_err_t res = ESP_OK;
    char destination[64];
    char boundary[BOUNDARY_MAX_LEN];

    if (!get_directory_destination(req, destination, sizeof(destination)))
        return ESP_FAIL;

    if (!get_boundary(req, boundary, sizeof(boundary)))
        return ESP_FAIL;

    bool file_open = false;
    static char buf[BUFFSIZE];
    int received = 0;
    FILE *f = NULL;

    printf("---- Starting file upload handler ----\n");

    while (1)
    {
        received = httpd_req_recv(req, buf, BUFFSIZE);
        if (received == HTTPD_SOCK_ERR_TIMEOUT)
            continue;
        else if (received < 0)
        {
            ESP_LOGE(TAG, "File reception failed: %d", received);
            res = ESP_FAIL;
            break;
        }
        else if (received == 0)
        {
            file_upload_complete(req);
            break;
        }

        char *data_start = buf;
        char *data_end = NULL;

        while (data_start < buf + received)
        {
            if (!file_open)
            {
                char *boundary_start = strstr(data_start, boundary);
                if (boundary_start)
                {
                    data_start = boundary_start + strlen(boundary);
                    char *header_end = strstr(data_start, "\r\n\r\n");
                    if (header_end)
                    {
                        data_start = header_end + 4; // Move past the "\r\n\r\n" to the binary data start
                        char filename[64];
                        if (extract_filename_from_content_disposition(*req, buf, filename, sizeof(filename)))
                        {
                            if ((f = create_file(req, filename, destination)) == NULL)
                            {
                                res = ESP_FAIL;
                                break;
                            }

                            file_open = true;
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
                file_open = write_to_file(f, data_start, data_end, boundary, buf, received);
                data_start =file_open ? buf + received : data_end + strlen(boundary);
                break;
            }
        }
    }

    printf("---- Ending file upload handler ----\n");
    return res;
}
