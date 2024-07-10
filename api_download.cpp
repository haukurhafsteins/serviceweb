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

    printf("File path: %s\n", filepath);
    printf("File size: %ld\n", file_stat.st_size);

    // Check the file ending and set the appropriate content type
    char *file_ending = strrchr(filepath, '.');
    if (file_ending)
    {
        if (strcmp(file_ending, ".pdf") == 0)
            httpd_resp_set_type(req, "application/pdf");
        else if (strcmp(file_ending, ".jpg") == 0)
            httpd_resp_set_type(req, "image/jpeg");
        else if (strcmp(file_ending, ".png") == 0)
            httpd_resp_set_type(req, "image/png");
        else if (strcmp(file_ending, ".gif") == 0)
            httpd_resp_set_type(req, "image/gif");
        else if (strcmp(file_ending, ".html") == 0)
            httpd_resp_set_type(req, "text/html");
        else if (strcmp(file_ending, ".css") == 0)
            httpd_resp_set_type(req, "text/css");
        else if (strcmp(file_ending, ".js") == 0)
            httpd_resp_set_type(req, "application/javascript");
        else if (strcmp(file_ending, ".json") == 0)
            httpd_resp_set_type(req, "application/json");
        else if (strcmp(file_ending, ".xml") == 0)
            httpd_resp_set_type(req, "application/xml");
        else if (strcmp(file_ending, ".zip") == 0)
            httpd_resp_set_type(req, "application/zip");
        else if (strcmp(file_ending, ".gz") == 0)
            httpd_resp_set_type(req, "application/gzip");
        else if (strcmp(file_ending, ".tar") == 0)
            httpd_resp_set_type(req, "application/x-tar");
        else if (strcmp(file_ending, ".txt") == 0)
            httpd_resp_set_type(req, "text/plain");
        else if (strcmp(file_ending, ".csv") == 0)
            httpd_resp_set_type(req, "text/csv");
        else
            httpd_resp_set_type(req, "application/octet-stream");
    }
    else
    {
        httpd_resp_set_type(req, "application/octet-stream");
    }

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

    free(chunk);
    fclose(file);
    httpd_resp_send_chunk(req, NULL, 0);
    ESP_LOGI(TAG, "File %s, download complete", filepath);
    return ESP_OK;
}
