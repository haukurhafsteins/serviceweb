
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
#include <errno.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "httpss.h"
#include "multipart_parser.h"

#define MOUNT_POINT "/spiffs"
#define TAG "FILE_SERVER"

static esp_err_t file_list_all_handler(httpd_req_t *req)
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


#define BOUNDARY_MAX_LEN 100
#define BUFFSIZE 512
#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + 64)

esp_err_t file_upload_handler(httpd_req_t *req)
{
    char filepath[FILE_PATH_MAX];
    esp_err_t res = ESP_OK;
    char destination[64];
    char filename[64];
    char boundary[BOUNDARY_MAX_LEN];

    // Retrieve the URL query string length
    const int buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1)
    {
        char *buf1 = (char *)malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf1, buf_len) != ESP_OK)
        {
            ESP_LOGI(TAG, "Unable to get query string");
            free(buf1);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unable to get query string");
            return ESP_FAIL;
        }
        else
        {
            if (httpd_query_key_value(buf1, "dir", destination, sizeof(destination)) != ESP_OK)
            {
                ESP_LOGI(TAG, "Destination not found in URL query %s", buf1);
                free(buf1);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Destination not found in URL query");
                return ESP_FAIL;
            }
        }
        free(buf1);
    }
    else
    {
        ESP_LOGI(TAG, "Query not found");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Query not found");
        return ESP_FAIL;
    }

    static char buf[BUFFSIZE];
    int received = 0;
    int total_received = 0;
    bool boundary_found = false;

    // Extract the boundary from the content type
    if (!boundary_found)
    {
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
                ESP_LOGI(TAG, "File reception complete");
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

        total_received += received;
        char *data_start = NULL;
        char *data_end = NULL;

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

                    data_end = strstr(data_start, boundary);
                    if (data_end)
                    {
                        received = data_end - data_start;
                    }
                    else
                    {
                        received -= (data_start - buf);
                    }

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
                                printf("File opened: %s\n", filepath);
                                file_open = true;
                            }
                        }
                    }
                }
            }
        }
        else
        {
            data_end = strstr(buf, boundary);
            if (data_end)
            {
                received = data_end - buf;
                is_header = true;
            }
            data_start = buf;
        }

        if (data_start && received > 0 && file_open)
        {
            fwrite(data_start, 1, received, f);
        }
    }

    if (file_open)
    {
        printf("Closing file at end\n");
        fclose(f);
    }

    return res;
}

static esp_err_t file_delete_handler(httpd_req_t *req)
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
    httpss_register_url("/api/list", false, file_list_all_handler, HTTP_GET, NULL);
    httpss_register_url("/api/upload", false, file_upload_handler, HTTP_POST, NULL);
    httpss_register_url("/api/delete", false, file_delete_handler, HTTP_POST, NULL);

    return ESP_OK;
}

void start_api_server(void)
{
    start_file_server();
}
