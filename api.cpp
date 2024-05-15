
// #include <stdio.h>
// #include <stdlib.h>
// #include <string.h>
// #include <dirent.h>
// #include <sys/stat.h>
// #include "cJSON.h"
// #include "esp_http_server.h"
// #include <dirent.h>
// #include <sys/stat.h>
// #include <cJSON.h>

// static const char *TAG = "api";

// void list_files_recursive(const char *dir_name, cJSON *json_array) {
//     DIR *dir = opendir(dir_name);
//     if (dir == NULL) {
//         printf("Error opening directory: %s\n", dir_name);
//         perror("opendir");
//         return;
//     }

//     struct dirent *entry;
//     while ((entry = readdir(dir)) != NULL) {
//         if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
//             continue;
//         }

//         char path[1024];
//         snprintf(path, sizeof(path), "%s/%s", dir_name, entry->d_name);

//         struct stat statbuf;
//         if (stat(path, &statbuf) == -1) {
//             perror("stat");
//             continue;
//         }

//         if (S_ISDIR(statbuf.st_mode)) {
//             list_files_recursive(path, json_array);
//         } else {
//             cJSON *file_obj = cJSON_CreateObject();
//             cJSON_AddStringToObject(file_obj, "path", path);
//             cJSON_AddNumberToObject(file_obj, "size", statbuf.st_size);
//             cJSON_AddNumberToObject(file_obj, "modification_time", statbuf.st_mtime);

//             cJSON_AddItemToArray(json_array, file_obj);
//         }
//     }

//     closedir(dir);
// }

// char* get_files_json(const char *dir_name) {
//     cJSON *json_array = cJSON_CreateArray();
//     list_files_recursive(dir_name, json_array);
//     char *json_str = cJSON_Print(json_array);
//     cJSON_Delete(json_array);  // free the JSON object
//     return json_str;
// }

// char* remove_file(const char *file_path) {
//     if (remove(file_path) == 0) {
//         return "{\"status\": \"ok\"}";
//     } else {
//         return "{\"status\": \"error\"}";
//     }
// }

#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <dirent.h>
#include "esp_vfs.h"
//#include "esp_vfs_fat.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "httpss.h"

#define MOUNT_POINT "/spiffs"
#define TAG "FILE_SERVER"

static esp_err_t list_files_handler(httpd_req_t *req)
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

    cJSON *json_array = cJSON_CreateArray();

    DIR *dir = opendir(directory);
    if (dir == NULL)
    {
        ESP_LOGE(TAG, "Failed to open directory : %s", directory);
        // httpd_resp_send_500(req);
        httpd_resp_sendstr(req, "{\"message\": \"Failed to open directory\"}");

        cJSON_Delete(json_array);
        return ESP_FAIL;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        if (entry->d_type == DT_REG)
        { // Check if it is a regular file
            char filepath[512];
            snprintf(filepath, sizeof(filepath), "%s/%s", directory, entry->d_name);
            struct stat file_stat;
            stat(filepath, &file_stat);

            cJSON *file_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(file_obj, "path", filepath);
            cJSON_AddNumberToObject(file_obj, "size", file_stat.st_size);
            cJSON_AddNumberToObject(file_obj, "modification_time", file_stat.st_mtime);
            cJSON_AddItemToArray(json_array, file_obj);
        }
    }
    closedir(dir);

    const char *json_str = cJSON_Print(json_array);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    cJSON_Delete(json_array);
    free((void *)json_str);

    return ESP_OK;
}

static esp_err_t upload_file_handler(httpd_req_t *req) {
    char filepath[128];
    snprintf(filepath, sizeof(filepath), "%s/%s", MOUNT_POINT, req->uri + sizeof("/api/upload/") - 1);

    FILE *f = fopen(filepath, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file for writing : %s", filepath);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open file for writing");
        return ESP_FAIL;
    }

    char buf[512];
    int ret;
    while ((ret = httpd_req_recv(req, buf, sizeof(buf))) > 0) {
        fwrite(buf, 1, ret, f);
    }
    fclose(f);

    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "message", "File uploaded successfully");

    const char *response_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, response_str);

    cJSON_Delete(response);
    free((void *)response_str);

    return ESP_OK;
}

static esp_err_t delete_files_handler(httpd_req_t *req)
{
    char buf[512];
    int total_len = req->content_len;
    int cur_len = 0;
    int received = 0;

    char *json_str = (char *)malloc(total_len + 1);
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

    cJSON *json = cJSON_Parse(json_str);
    if (!json)
    {
        ESP_LOGE(TAG, "JSON parse error");
        free(json_str);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON *files = cJSON_GetObjectItem(json, "files");
    if (!cJSON_IsArray(files))
    {
        ESP_LOGE(TAG, "JSON format error: files is not an array");
        cJSON_Delete(json);
        free(json_str);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    cJSON *file = NULL;
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
    httpss_register_url("/api/list", false, list_files_handler, HTTP_GET, NULL);
    httpss_register_url("/api/upload/*", false, upload_file_handler, HTTP_POST, NULL);
    httpss_register_url("/api/delete", false, delete_files_handler, HTTP_POST, NULL);

    return ESP_OK;
}

void start_api_server(void)
{
    start_file_server();
}
