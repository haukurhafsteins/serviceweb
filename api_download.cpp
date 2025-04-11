#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "httpss.h"
#include "api_priv.hpp"

static const char *TAG = "FILE_SERVER";

extern void serviceweb_set_content_type(httpd_req_t *req, const char *filename);

esp_err_t api_file_download_handler(httpd_req_t *req)
{
    printf("api_file_download_handler, file: %s\n", req->uri);

    char filepath[FILE_PATH_MAX];
    FILE *file = NULL;
    struct stat file_stat;
    int bytes_read;

    if (get_value_from_query(req, "file", filepath, FILE_PATH_MAX) == false)
        return ESP_FAIL;

    file = fopen(filepath, "r");
    if (!file)
    {
        ESP_LOGE(TAG, "Failed to open file : %s", filepath);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    if (fstat(fileno(file), &file_stat) == -1)
    {
        ESP_LOGE(TAG, "Failed to fstat file : %s", filepath);
        fclose(file);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to fstat file");
        return ESP_FAIL;
    }

    // if file ends with .gz, set gzip_supported to true
    bool gzip_supported = false;
    if (strstr(filepath, ".gz") != NULL)
    {
        if (ESP_OK != _set_gz_support(req, gzip_supported))
        {
            ESP_LOGE(TAG, "Error setting gzip support for file %s", req->uri);
            return ESP_FAIL;
        }
    }

    printf("File path: %s\n", filepath);
    printf("File size: %ld\n", file_stat.st_size);
    char length_header[32];
    snprintf(length_header, sizeof(length_header), "%ld", file_stat.st_size);
    // httpd_resp_set_hdr(req, "Content-Length", length_header);

    serviceweb_set_content_type(req, filepath);

    char *filename = strrchr(filepath, '/');
    if (filename)
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "attachment; filename=\"%s\"", filename + 1);
        httpd_resp_set_hdr(req, "Content-Disposition", buf);
    }

    char *chunk = (char *)malloc(API_BUFFSIZE);
    if (!chunk)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for file download");
        fclose(file);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to allocate memory for file download");
        return ESP_FAIL;
    }

    // If the file is less than the buffer size, send it in one go
    if (file_stat.st_size < API_BUFFSIZE)
    {
        bytes_read = fread(chunk, 1, file_stat.st_size, file);
        esp_err_t err = httpd_resp_send(req, chunk, bytes_read);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to send file download: %s", esp_err_to_name(err));
            free(chunk);
            fclose(file);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to send file download");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "File %s, download complete", filepath);
    }
    else
    {
        while ((bytes_read = fread(chunk, 1, API_BUFFSIZE, file)) > 0)
        {
            esp_err_t err = httpd_resp_send_chunk(req, chunk, bytes_read);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to send file download chunk: %s", esp_err_to_name(err));
                free(chunk);
                fclose(file);
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to send file download chunk");
                return ESP_FAIL;
            }
        }
        httpd_resp_send_chunk(req, NULL, 0);
        ESP_LOGI(TAG, "Chunked file %s, download complete", filepath);
    }

    free(chunk);
    fclose(file);
    return ESP_OK;
}
