#pragma once

#include <stdbool.h>
#include <esp_err.h>
#include <esp_vfs.h>
#include "esp_http_server.h"

#define BOUNDARY_MAX_LEN 100
#define API_BUFFSIZE 512
#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + 64)

bool get_value_from_query(httpd_req_t *req, const char* value_name, char *destination, size_t dest_len);
esp_err_t api_file_upload_handler(httpd_req_t *req);
esp_err_t api_file_download_handler(httpd_req_t *req);
esp_err_t api_file_list_all_handler(httpd_req_t *req);
