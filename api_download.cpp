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

static const char* TAG = "FILE_SERVER";


esp_err_t api_file_download_handler(httpd_req_t *req)
{
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

    httpd_resp_set_type(req, "application/octet-stream");

    char *filename = strrchr(filepath, '/');
    if (filename)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "attachment; filename=%s", filename + 1);
        httpd_resp_set_hdr(req, "Content-Disposition", buf);
        snprintf(buf, sizeof(buf), "%ld", file_stat.st_size);
        httpd_resp_set_hdr(req, "Content-Length", buf);
    }

    char *chunk = (char *)malloc(API_BUFFSIZE);
    if (!chunk)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for file download");
        fclose(file);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to allocate memory for file download");
        return ESP_FAIL;
    }

    while ((bytes_read = fread(chunk, 1, API_BUFFSIZE, file)) > 0)
    {
        if (httpd_resp_send_chunk(req, chunk, bytes_read) != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to send file download chunk");
            free(chunk);
            fclose(file);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to send file download chunk");
            return ESP_FAIL;
        }
    }

    free(chunk);
    fclose(file);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;

}
