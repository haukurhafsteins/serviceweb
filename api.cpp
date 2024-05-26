

#include <string.h>
#include <libgen.h>
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
#include "api_priv.hpp"

#define TAG "FILE_SERVER"

static esp_err_t file_rename_handler(httpd_req_t* req)
{
    // char from[64];
    // char to[64];
    // if (get_value_from_query(req, "from", from, sizeof(from)) == false)
    //     return ESP_FAIL;
    // if (get_value_from_query(req, "to", to, sizeof(to)) == false)
    //     return ESP_FAIL;

    // ESP_LOGI(TAG, "Renaming file: %s to %s", from, to);

    // char to_path[128];
    // snprintf(to_path, sizeof(to_path), "%s/%s", dirname(from), to);

    // if (rename(from, to_path) != 0)
    // {
    //     ESP_LOGE(TAG, "Failed to rename file: %s", from);
    //     httpd_resp_sendstr(req, "{\"message\": \"Failed to rename file\"}");
    //     return ESP_FAIL;
    // }

    // httpd_resp_sendstr(req, "{\"message\": \"File renamed successfully\"}");
    return ESP_OK;
}

static esp_err_t file_copy_handler(httpd_req_t* req)
{
    return ESP_OK;
}

static esp_err_t file_list_all_handler(httpd_req_t* req)
{
    char directory[128];
    size_t len = httpd_req_get_url_query_len(req) + 1;
    if (len > 1)
    {
        char query[255];
        httpd_req_get_url_query_str(req, query, len);
        httpd_query_key_value(query, "dir", directory, sizeof(directory));
    }
    else
    {
        strcpy(directory, "/spiffs");
    }

    cJSON* json_array = cJSON_CreateArray();

    DIR* dir = opendir(directory);
    if (dir == NULL)
    {
        ESP_LOGE(TAG, "Failed to open directory : %s", directory);
        // httpd_resp_send_500(req);
        httpd_resp_sendstr(req, "{\"message\": \"Failed to open directory\"}");

        cJSON_Delete(json_array);
        return ESP_FAIL;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL)
    {
        if (entry->d_type == DT_REG)
        { // Check if it is a regular file
            char filepath[512];
            snprintf(filepath, sizeof(filepath), "%s/%s", directory, entry->d_name);
            struct stat file_stat;
            stat(filepath, &file_stat);

            cJSON* file_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(file_obj, "path", filepath);
            cJSON_AddNumberToObject(file_obj, "size", file_stat.st_size);
            cJSON_AddNumberToObject(file_obj, "modification_time", file_stat.st_mtime);
            cJSON_AddItemToArray(json_array, file_obj);
        }
    }
    closedir(dir);

    const char* json_str = cJSON_Print(json_array);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    cJSON_Delete(json_array);
    cJSON_free((void*)json_str);

    return ESP_OK;
}

static esp_err_t file_delete_handler(httpd_req_t* req)
{
    char buf[512];
    int total_len = req->content_len;
    int cur_len = 0;
    int received = 0;

    char* json_str = (char*)malloc(total_len + 1);
    if (!json_str)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for JSON string");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    while (cur_len < total_len)
    {
        received = httpd_req_recv(req, buf, MIN(total_len - cur_len, sizeof(buf)));
        if (received <= 0)
        {
            if (received == HTTPD_SOCK_ERR_TIMEOUT)
            {
                continue;
            }
            free(json_str);
            ESP_LOGE(TAG, "File deletion failed, received error: %d", received);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        memcpy(json_str + cur_len, buf, received);
        cur_len += received;
    }
    json_str[total_len] = '\0';

    cJSON* json = cJSON_Parse(json_str);
    if (!json)
    {
        ESP_LOGE(TAG, "JSON parse error");
        free(json_str);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON* files = cJSON_GetObjectItem(json, "files");
    if (!cJSON_IsArray(files))
    {
        ESP_LOGE(TAG, "JSON format error: files is not an array");
        cJSON_Delete(json);
        free(json_str);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON* file = NULL;
    cJSON_ArrayForEach(file, files)
    {
        if (cJSON_IsString(file))
        {
            if (remove(file->valuestring) != 0)
            {
                ESP_LOGE(TAG, "Failed to delete file: %s", file->valuestring);
            }
        }
    }

    cJSON_Delete(json);
    free(json_str);

    httpd_resp_sendstr(req, "{\"message\": \"Files deleted successfully\"}");

    return ESP_OK;
}

static esp_err_t start_file_server()
{
    httpss_register_url("/api/list", false, file_list_all_handler, HTTP_GET, NULL);
    httpss_register_url("/api/upload", false, api_file_upload_handler, HTTP_POST, NULL);
    httpss_register_url("/api/download", false, api_file_download_handler, HTTP_GET, NULL);
    httpss_register_url("/api/delete", false, file_delete_handler, HTTP_POST, NULL);
    httpss_register_url("/api/rename", false, file_rename_handler, HTTP_POST, NULL);
    httpss_register_url("/api/copy", false, file_copy_handler, HTTP_POST, NULL);

    return ESP_OK;
}

void start_api_server(void)
{
    start_file_server();
}
