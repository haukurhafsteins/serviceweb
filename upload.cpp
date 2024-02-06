#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_spiffs.h"
#include "esp_http_server.h"

#define BOUNDARY_MAX_LEN 100
#define BUFFSIZE 2048
#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + 64)

const static char *TAG = "spiffs";

static char boundary[BOUNDARY_MAX_LEN];
static bool boundary_found = false;

esp_err_t spiffs_upload_handler(httpd_req_t *req)
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

// #include <stdio.h>
// #include <string.h>
// #include <sys/param.h>
// #include <sys/unistd.h>
// #include <sys/stat.h>
// #include <dirent.h>

// #include "esp_err.h"
// #include "esp_log.h"

// #include "esp_vfs.h"
// #include "esp_spiffs.h"
// #include "esp_http_server.h"

// /* Max length a file path can have on storage */
// #define FILE_PATH_MAX (ESP_VFS_PATH_MAX + CONFIG_SPIFFS_OBJ_NAME_LEN)

// /* Max size of an individual file. Make sure this
//  * value is same as that set in upload_script.html */
// #define MAX_FILE_SIZE   (200*1024) // 200 KB
// #define MAX_FILE_SIZE_STR "200KB"

// /* Scratch buffer size */
// #define SCRATCH_BUFSIZE  8192

// static const char *TAG = "file_server";

// esp_err_t spiffs_upload_handler(httpd_req_t *req)
// {
//     static char filepath[FILE_PATH_MAX];
//     FILE *fd = NULL;
//     struct stat file_stat;

//     printf("#### spiffs_upload_handler: %s\n", req->uri);

//     // Extract filename from the URI
//     // req->uri will contain the path, e.g., /upload/filename.txt
//     const char *filename = req->uri + strlen("/upload/"); // Skip the /upload/ part to get the filename
//     if (*filename == '\0')
//     {
//         // Handle case where no filename is provided in the URL
//         httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Filename is required");
//         ESP_LOGE(TAG, "Filename is required, but not provided in the URL: %s", req->uri);
//         return ESP_FAIL;
//     }

//     // Ensure the filename does not exceed our buffer size and construct the full path
//     if (snprintf(filepath, sizeof(filepath), "/spiffs/%s", filename) >= sizeof(filepath))
//     {
//         httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Filename is too long");
//         return ESP_FAIL;
//     }

//     /* Filename cannot have a trailing '/' */
//     if (filename[strlen(filename) - 1] == '/')
//     {
//         ESP_LOGE(TAG, "Invalid filename : %s", filename);
//         httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Invalid filename");
//         return ESP_FAIL;
//     }

//     // if (stat(filepath, &file_stat) == 0)
//     // {
//     //     ESP_LOGE(TAG, "File already exists : %s", filepath);
//     //     /* Respond with 400 Bad Request */
//     //     httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File already exists");
//     //     return ESP_FAIL;
//     // }

//     /* File cannot be larger than a limit */
//     if (req->content_len > MAX_FILE_SIZE)
//     {
//         ESP_LOGE(TAG, "File too large : %d bytes", req->content_len);
//         /* Respond with 400 Bad Request */
//         httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
//                             "File size must be less than " MAX_FILE_SIZE_STR "!");
//         /* Return failure to close underlying connection else the
//          * incoming file content will keep the socket busy */
//         return ESP_FAIL;
//     }

//     fd = fopen(filepath, "w");
//     if (!fd)
//     {
//         ESP_LOGE(TAG, "Failed to create file : %s", filepath);
//         /* Respond with 500 Internal Server Error */
//         httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create file");
//         return ESP_FAIL;
//     }

//     ESP_LOGI(TAG, "Receiving file : %s...", filename);

//     /* Retrieve the pointer to scratch buffer for temporary storage */
//     char *buf = (char *)malloc(SCRATCH_BUFSIZE);
//     int received;

//     /* Content length of the request gives
//      * the size of the file being uploaded */
//     int remaining = req->content_len;

//     while (remaining > 0)
//     {

//         ESP_LOGI(TAG, "Remaining size : %d", remaining);
//         /* Receive the file part by part into a buffer */
//         if ((received = httpd_req_recv(req, buf, MIN(remaining, SCRATCH_BUFSIZE))) <= 0)
//         {
//             if (received == HTTPD_SOCK_ERR_TIMEOUT)
//             {
//                 /* Retry if timeout occurred */
//                 continue;
//             }

//             /* In case of unrecoverable error,
//              * close and delete the unfinished file*/
//             fclose(fd);
//             unlink(filepath);

//             ESP_LOGE(TAG, "File reception failed!");
//             /* Respond with 500 Internal Server Error */
//             httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive file");
//             free(buf);
//             return ESP_FAIL;
//         }

//         /* Write buffer content to file on storage */
//         if (received && (received != fwrite(buf, 1, received, fd)))
//         {
//             /* Couldn't write everything to file!
//              * Storage may be full? */
//             fclose(fd);
//             unlink(filepath);

//             ESP_LOGE(TAG, "File write failed!");
//             /* Respond with 500 Internal Server Error */
//             httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to write file to storage");
//             free(buf);
//             return ESP_FAIL;
//         }

//         /* Keep track of remaining size of
//          * the file left to be uploaded */
//         remaining -= received;
//     }

//     free(buf);

//     /* Close file upon upload completion */
//     fclose(fd);
//     ESP_LOGI(TAG, "File reception complete");

//     /* Redirect onto root to see the updated file list */
//     httpd_resp_set_status(req, "303 See Other");
//     httpd_resp_set_hdr(req, "Location", "/");
// #ifdef CONFIG_EXAMPLE_HTTPD_CONN_CLOSE_HEADER
//     httpd_resp_set_hdr(req, "Connection", "close");
// #endif
//     httpd_resp_sendstr(req, "File uploaded successfully");
//     return ESP_OK;
// }
