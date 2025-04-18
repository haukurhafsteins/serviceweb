#include <dirent.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <map>
#include <list>
#include <string>
#include "esp_log.h"
#include "serviceweb.h"
#include "httpss.h"
#include "api_priv.hpp"

#include "cJSON.h"
#include "pp.h"

#define MAX_FLOAT_BYTES 80
#define SEND_TIMOEOUT_MS 100

// #define DEBUG_PARAMETER 1

typedef struct
{
    int socket;
    char payload[0];
} pp_websocket_data_t;

enum
{
    CMD_SOCKET_CLOSED = 0x1000,
    CMD_WS_HANDLER,
};

typedef struct
{
    const uint8_t* html_start;
    const uint8_t* html_end;
    bool gzip;
} file_get_t;

extern esp_err_t ota_post_handler(httpd_req_t* req);
extern esp_err_t web_post_handler(httpd_req_t* req);
extern esp_err_t sysmon_get_handler(httpd_req_t* req);
extern esp_err_t sysmon_get_info(httpd_req_t* req);
extern esp_err_t sysmon_get_partition(httpd_req_t* req);
extern void start_api_server(void);

static const char* SUBSCRIBE_RESP = "subscribeResp";
static const char* RESP_MESSAGE = "{\"cmd\":\"%s\",\"data\":{\"name\":\"%s\", \"value\":";
static const char* NEWSTATE_FLOAT = "{\"cmd\":\"newState\",\"data\":{\"name\":\"%s\", \"value\":%f}}";
static const char* NEWSTATE_INT32 = "{\"cmd\":\"newState\",\"data\":{\"name\":\"%s\", \"value\":%ld}}";
static const char* NEWSTATE_INT64 = "{\"cmd\":\"newState\",\"data\":{\"name\":\"%s\", \"value\":%lld}}";
static const char* NEWSTATE_STRING = "{\"cmd\":\"newState\",\"data\":{\"name\":\"%s\", \"value\":\"%s\"}}";
static const char* NEWSTATE_JSON = "{\"cmd\":\"newState\",\"data\":{\"name\":\"%s\", \"value\":%s}}";
static const char* UNSUBSCRIBE_MESSAGE = "{\"cmd\":\"unsubscribeResp\",\"data\":\"%s\"}";

static const char* NNEWSTATE_FLOAT = "{\"f\":\"%s\"}";
// static const char* NNEWSTATE_BINARY = "{\"bin\":\"%s\"}";

static char TAG[] = "SERVWEB";
static char* json_buf = 0;
static size_t json_buf_size = 0;
static char* rx_buf = 0;
static size_t rx_buf_size = 0;
static pp_evloop_t* evloop;
static std::map<std::string, file_get_t> file_get_map;
static const char* web_root = NULL;

static void evloop_newstate(void* handler_arg, esp_event_base_t base, int32_t id, void* context);

void serviceweb_set_content_type(httpd_req_t* req, const char* filename)
{
    // Find the file ending
    char* file_ending = strrchr(filename, '.');
    if (file_ending)
    {
        if (strcmp(file_ending, ".html") == 0)
            httpd_resp_set_type(req, "text/html");
        else if (strcmp(file_ending, ".js") == 0)
            httpd_resp_set_type(req, "application/javascript");
        else if (strcmp(file_ending, ".css") == 0)
            httpd_resp_set_type(req, "text/css");
        else if (strcmp(file_ending, ".svg") == 0)
            httpd_resp_set_type(req, "image/svg+xml");
        else if (strcmp(file_ending, ".json") == 0)
            httpd_resp_set_type(req, "application/json");
        else if (strcmp(file_ending, ".jpg") == 0)
            httpd_resp_set_type(req, "image/jpeg");
        else if (strcmp(file_ending, ".png") == 0)
            httpd_resp_set_type(req, "image/png");
        else if (strcmp(file_ending, ".gif") == 0)
            httpd_resp_set_type(req, "image/gif");
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
        else if (strcmp(file_ending, ".glb") == 0)
            httpd_resp_set_type(req, "model/gltf-binary");
        else if (strcmp(file_ending, ".gltf") == 0)
            httpd_resp_set_type(req, "model/gltf+json");
        else if (strcmp(file_ending, ".stl") == 0)
            httpd_resp_set_type(req, "model/stl");
        else if (strcmp(file_ending, ".obj") == 0)
            httpd_resp_set_type(req, "model/obj");
        else
            httpd_resp_set_type(req, "application/octet-stream");
    }
    else
    {
        httpd_resp_set_type(req, "application/octet-stream");
    }

    //    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000"); // One year cache      
}

esp_err_t _set_gz_support(httpd_req_t* req, bool& gzip_supported)
{
    esp_err_t err = ESP_OK;
    char encoding[64];
    gzip_supported = false;
    if (httpd_req_get_hdr_value_str(req, "Accept-Encoding", encoding, sizeof(encoding)) == ESP_OK)
    {
        if (strstr(encoding, "gzip"))
        {
            gzip_supported = true;
            err = httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
            if (err != ESP_OK)
                return ESP_FAIL;
        }
    }
    return err;
}
esp_err_t _set_keepalive_support(httpd_req_t* req, bool& keep_alive)
{
    esp_err_t err = ESP_OK;
    char connection[32];
    keep_alive = false;
    if (httpd_req_get_hdr_value_str(req, "Connection", connection, sizeof(connection)) == ESP_OK)
    {
        if (strstr(connection, "keep-alive"))
        {
            keep_alive = true;
            if (keep_alive)
                err = httpd_resp_set_hdr(req, "Connection", "keep-alive");
            else
                err = httpd_resp_set_hdr(req, "Connection", "close");
        }
    }
    return err;
}

void remove_query_parameters(char* buf)
{
    char* p = strstr(buf, "?");
    if (p != NULL)
        *p = 0;
}

static esp_err_t resp_memory_file(httpd_req_t* req)
{
    printf("resp_memory_file, uri: %s\n", req->uri);

    const int bufsize = sizeof(req->uri);
    char* buf = (char*)calloc(bufsize, 1);
    if (buf == NULL)
    {
        ESP_LOGE(TAG, "Error allocating memory for buffer when serving memory file %s", req->uri);
        return ESP_FAIL;
    }

    snprintf(buf, bufsize, "%s", req->uri);
    remove_query_parameters(buf);
    serviceweb_set_content_type(req, buf);

    if (!file_get_map.contains(buf))
    {
        ESP_LOGE(TAG, "File %s not found as memory file", req->uri);
        free(buf);
        return httpd_resp_send_404(req);
    }

    bool gzip_supported, keep_alive;
    file_get_t file = file_get_map[buf];

    _set_gz_support(req, gzip_supported);
    _set_keepalive_support(req, keep_alive);
    httpd_resp_send_chunk(req, (char*)file.html_start, file.html_end - file.html_start);
    esp_err_t ret = httpd_resp_send_chunk(req, buf, 0);
    free(buf);
    return ret;
}

static size_t get_file_size(FILE* file)
{
    fseek(file, 0, SEEK_END);
    size_t size = ftell(file);
    fseek(file, 0, SEEK_SET);
    return size;
}

static esp_err_t resp_disk_file(httpd_req_t* req)
{
    esp_err_t err;
    int read = 0;
    const int bufsize = 1460;               // Default buffer size.
    char* buf = (char*)calloc(bufsize, 1); // Allocate memory for the buffer.
    if (buf == NULL)
    {
        ESP_LOGE(TAG, "Error allocating memory for buffer when serving file %s", req->uri);
        return ESP_FAIL;
    }

    bool gzip_supported = false;

    if (ESP_OK != _set_gz_support(req, gzip_supported))
    {
        ESP_LOGE(TAG, "Error setting gzip support for file %s", req->uri);
        free(buf);
        return ESP_FAIL;
    }

    // Construct the file path.
    snprintf(buf, bufsize, "%s/%s", web_root, req->uri);

    remove_query_parameters(buf);
    serviceweb_set_content_type(req, buf);

    printf("resp_disk_file, uri: %s\n", buf);

    // Check for gzip file existence before trying it.
    FILE* file = NULL;
    if (gzip_supported)
    {
        // Attempt to open .gz file.
        strncat(buf, ".gz", bufsize - strlen(buf) - 1);
        file = fopen(buf, "rb");
    }

    if (file == NULL)
    {
        // Retry with the original file if .gz was not found.
        ESP_LOGE(TAG, "File %s does not exist, going for non .gz file...", buf);
        gzip_supported = false;

        file = fopen(buf, "rb");
        if (file == NULL)
        {
            ESP_LOGE(TAG, "File %s does not exist!", buf);
            free(buf);
            return ESP_FAIL;
        }
    }

    size_t file_size = get_file_size(file);
    if (file_size == (size_t)-1)
    {
        ESP_LOGE(TAG, "Failed to get file size for %s", buf);
        fclose(file);
        free(buf);
        return ESP_FAIL;
    }

    if (file_size < bufsize)
    {
        if ((read = fread(buf, 1, bufsize, file)) > 0)
            httpd_resp_send(req, buf, file_size);
    }
    else
    {
        // Send the file in chunks
        while ((read = fread(buf, 1, bufsize, file)) > 0)
        {
            err = httpd_resp_send_chunk(req, buf, read);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Error sending chunk for file %s: %s", req->uri, esp_err_to_name(err));
                break;
            }
        }
        err = httpd_resp_send_chunk(req, buf, 0); // End of chunked transfer.
        if (err != ESP_OK)
            ESP_LOGE(TAG, "Error sending last chunk for file %s: %s", req->uri, esp_err_to_name(err));
    }

    fclose(file);
    free(buf);

    return ESP_OK;
}

//-- Parameter handling --------------------------------------------------------

// pp_t -> socket
// A map of all parameters that are subscribed to by serviceweb.
// Each parameter has a list of sockets that are subscribed to it.
static std::map<pp_t, std::list<int>> pp_list;
static std::list<int> socket_closed_list;

static bool par_list_empty() { return pp_list.empty(); }
static bool par_exist(pp_t pp) { return pp_list.find(pp) != pp_list.end(); }

// Go through the list of sockets that have been closed and remove them from the
// list of sockets for each parameter. If the list of sockets for a parameter
// becomes empty, unsubscribe the parameter.
static void par_cleanup()
{
    for (auto it = socket_closed_list.begin(); it != socket_closed_list.end(); it++)
    {
        // ESP_LOGI(TAG, "%s: Socket %d is closed", __func__, *it);
        for (auto it2 = pp_list.begin(); it2 != pp_list.end(); it2++)
            it2->second.remove(*it);
    }

    socket_closed_list.clear();

    for (auto it = pp_list.begin(); it != pp_list.end();)
    {
        if (it->second.size() == 0)
        {
            // ESP_LOGI(TAG, "%s: Unsubscribing parameter %s", __func__, pp_get_name(it->first));
            pp_unsubscribe(it->first, evloop, evloop_newstate);
            it = pp_list.erase(it);
        }
        else
            it++;
    }
}

static void par_send_to_sockets(pp_t pp, char* json)
{
    if (!pp_is_enabled(pp))
        return;

    for (auto it = pp_list[pp].begin(); it != pp_list[pp].end(); it++)
    {
        if (!httpss_websocket_send(*it, json))
            socket_closed_list.push_back(*it);
    }
    if (socket_closed_list.size() > 0)
        par_cleanup();
}

static void par_send_binary_to_sockets(pp_t pp, const char* json, size_t json_len, const void* bin, size_t bin_len)
{
    if (!pp_is_enabled(pp))
        return;

    for (auto it = pp_list[pp].begin(); it != pp_list[pp].end(); it++)
    {
        if (!httpss_websocket_send_binary(*it, json, json_len, bin, bin_len))
            socket_closed_list.push_back(*it);
    }
    if (socket_closed_list.size() > 0)
        par_cleanup();
}
static void par_remove_socket(pp_t pp, int socket)
{
    // ESP_LOGI(TAG, "%s: Removing socket %d from parameter %s", __func__, socket, pp_get_name(pp));
    pp_list[pp].remove(socket);
}
static void par_add_socket(pp_t pp, int socket)
{
    // ESP_LOGI(TAG, "%s: Adding socket %d to parameter %s", __func__, socket, pp_get_name(pp));
    pp_list[pp].push_back(socket);
}
static bool par_socket_exist(pp_t pp, int socket)
{
    for (auto it = pp_list[pp].begin(); it != pp_list[pp].end(); it++)
    {
        if (*it == socket)
            return true;
    }
    return false;
}

//-----------------------------------------------------------------------------
static bool web_post_newstate_int32(pp_t pp, int32_t i)
{
    if (!par_list_empty())
    {
        const char* name = pp_get_name(pp);
#ifdef DEBUG_PARAMETER
        ESP_LOGI(TAG, "%s: %s = %ld", __func__, name, i);
#endif
        size_t len = strlen(NEWSTATE_INT32) + strlen(name) + MAX_FLOAT_BYTES;
        char* json = (char*)malloc(len);
        if (json == NULL)
        {
            ESP_LOGE(TAG, "%s: Failed to allocate memory for json", __func__);
            return false;
        }
        snprintf(json, len, NEWSTATE_INT32, name, i);
        par_send_to_sockets(pp, json);
        free(json);
    }
    return true;
}

static bool web_post_newstate_int64(pp_t pp, int64_t i)
{
    if (!par_list_empty())
    {
        const char* name = pp_get_name(pp);
#ifdef DEBUG_PARAMETER
        ESP_LOGI(TAG, "%s: %s = %lld", __func__, name, i);
#endif
        size_t len = strlen(NEWSTATE_INT64) + strlen(name) + MAX_FLOAT_BYTES;
        char* json = (char*)malloc(len);
        if (json == NULL)
        {
            ESP_LOGE(TAG, "%s: Failed to allocate memory for json", __func__);
            return false;
        }
        snprintf(json, len, NEWSTATE_INT64, name, i);
        par_send_to_sockets(pp, json);
        free(json);
    }
    return true;
}

static bool web_post_newstate_string(pp_t pp, const char* str)
{
    if (!par_list_empty())
    {
        const char* format = NEWSTATE_STRING;
        if (str[0] == '{')
            format = NEWSTATE_JSON;
        const char* name = pp_get_name(pp);
#ifdef DEBUG_PARAMETER
        ESP_LOGI(TAG, "%s: %s = %s", __func__, name, str);
#endif
        size_t len = strlen(format) + strlen(name) + strlen(str) + 1;
        char* json = (char*)malloc(len);
        if (json == NULL)
        {
            ESP_LOGE(TAG, "%s: Failed to allocate memory for json", __func__);
            return false;
        }
        snprintf(json, len, format, name, str);
        par_send_to_sockets(pp, json);
        free(json);
    }
    return true;
}

static bool web_post_newstate_float(pp_t pp, float f)
{
    if (!par_list_empty())
    {
        const char* name = pp_get_name(pp);
#ifdef DEBUG_PARAMETER
        ESP_LOGI(TAG, "%s: %s = %f", __func__, name, f);
#endif
        size_t len = strlen(NEWSTATE_FLOAT) + strlen(name) + MAX_FLOAT_BYTES;
        char* json = (char*)malloc(len);
        if (json == NULL)
        {
            ESP_LOGE(TAG, "%s: Failed to allocate memory for json", __func__);
            return false;
        }
        snprintf(json, len, NEWSTATE_FLOAT, name, f);
        par_send_to_sockets(pp, json);
        free(json);
    }
    return true;
}

static bool web_post_newstate_float_array(pp_t pp, pp_float_array_t* fsrc)
{
    if (!par_list_empty())
    {
        const char* name = pp_get_name(pp);
#ifdef DEBUG_PARAMETER
        ESP_LOGI(TAG, "%s: %s", __func__, name);
#endif
        size_t len = 64;
        char* json = (char*)calloc(1, len);
        if (json == NULL)
        {
            ESP_LOGE(TAG, "%s: Failed to allocate memory for json", __func__);
            return false;
        }
        len = snprintf(json, len, NNEWSTATE_FLOAT, name) + 1;
        len = (len % 4) ? (len + 4) / 4 * 4 : len;
        par_send_binary_to_sockets(pp, json, len, fsrc->data, fsrc->len * sizeof(float));
        free(json);
    }
    return true;
}

// static bool web_post_newstate_binary(pp_t pp, const void* bin, size_t bin_size)
// {
//     if (!par_list_empty())
//     {
//         const char* name = pp_get_name(pp);
// #ifdef DEBUG_PARAMETER
//         ESP_LOGI(TAG, "%s: %s", __func__, name);
// #endif
//         size_t len = 64;
//         char* json = (char*)calloc(1, len);
//         len = snprintf(json, len, NNEWSTATE_BINARY, name) + 1;
//         par_send_binary_to_sockets(pp, json, len, bin, bin_size);
//         free(json);
//     }
//     return true;
// }

static void write_to_json_buf(pp_t pp, const char* format, ...)
{
    memset(json_buf, 0, json_buf_size);

    va_list valist;
    va_start(valist, format);
    int len = vsnprintf(json_buf, json_buf_size, format, valist);
    if (len < 0)
    {
        ESP_LOGE(TAG, "%s: Error, vsnprintf return negative", __func__);
        return;
    }
    va_end(valist);
    size_t read = json_buf_size - len;
    if (pp_to_string(pp, NULL, &json_buf[len], &read))
        strncat(json_buf, "}}", json_buf_size - len - read);
    else
        strncat(json_buf, "\"\"}}", json_buf_size - len);
}

static void evloop_newstate(void* handler_arg, esp_event_base_t base, int32_t id, void* context)
{
    pp_t pp = (pp_t)handler_arg;

    if (context == NULL)
    {
        ESP_LOGW(TAG, "%s: context is NULL", __func__);
        return;
    }

    // It is possible that the parameter has been unsubscribed from the web client but
    // a message is still in the queue. In that case, the parameter is not in the list.
    if (!par_exist(pp))
    {
        ESP_LOGW(TAG, "%s: Parameter %s not found but still in queue", __func__, pp_get_name(pp));
        return;
    }

    parameter_type_t type = pp_get_type(pp);
    switch (type)
    {
    case TYPE_INT32:
        web_post_newstate_int32(pp, *((int32_t*)context));
        break;
    case TYPE_INT64:
        web_post_newstate_int64(pp, *((int64_t*)context));
        break;
    case TYPE_BOOL:
        web_post_newstate_int32(pp, *((bool*)context));
        break;
    case TYPE_FLOAT:
        web_post_newstate_float(pp, *((float*)context));
        break;
    case TYPE_FLOAT_ARRAY:
        web_post_newstate_float_array(pp, ((pp_float_array_t*)context));
        break;
    case TYPE_STRING:
        web_post_newstate_string(pp, (char*)context);
        break;
    case TYPE_BINARY:
        // web_post_newstate_binary(pp, (char *)context); // TODO, length is missing
        break;
    default:
        ESP_LOGW(TAG, "unsupported type %d", type);
        break;
    }
}

static esp_err_t evloop_post(esp_event_loop_handle_t loop_handle, esp_event_base_t loop_base, int32_t id, void* data, size_t data_size)
{
    if (loop_handle == NULL)
        return esp_event_post(loop_base, id, data, data_size, pdMS_TO_TICKS(SEND_TIMOEOUT_MS));
    return esp_event_post_to(loop_handle, loop_base, id, data, data_size, pdMS_TO_TICKS(SEND_TIMOEOUT_MS));
}

static void evloop_http_event(void* handler_arg, esp_event_base_t base, int32_t id, void* context)
{
    esp_err_t err;
    int socfd = *(int*)context;
    switch (id)
    {
    case HTTP_SERVER_EVENT_DISCONNECTED:
        err = evloop_post(evloop->loop_handle, evloop->base, CMD_SOCKET_CLOSED, &socfd, sizeof(int));
        if (err != ESP_OK)
            ESP_LOGE(TAG, "%s: Error posting event: %s", __func__, esp_err_to_name(err));
        break;
    case CMD_SOCKET_CLOSED:
        socket_closed_list.push_back(socfd);
        par_cleanup();
        break;
    case HTTP_SERVER_EVENT_ON_CONNECTED:
        break;
    default:
        break;
    }
}

static esp_err_t ws_handler(httpd_req_t* req)
{
    if (req->method == HTTP_GET)
        return ESP_OK;

    pp_websocket_data_t* wsdata = (pp_websocket_data_t*)rx_buf;
    httpd_ws_frame_t ws_pkt = {};
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    ws_pkt.payload = (uint8_t*)wsdata->payload;
    if (ESP_OK != httpd_ws_recv_frame(req, &ws_pkt, rx_buf_size - 1 - sizeof(pp_websocket_data_t)))
    {
        ESP_LOGE(TAG, "%s: Error receiving websocket frame", __func__);
        return ESP_FAIL;
    }

    ws_pkt.payload[ws_pkt.len] = 0;
    wsdata->socket = httpd_req_to_sockfd(req);

    // Send data to serveiceweb task
    esp_err_t err = esp_event_post_to(evloop->loop_handle, evloop->base, CMD_WS_HANDLER, wsdata, sizeof(pp_websocket_data_t) + ws_pkt.len, pdMS_TO_TICKS(SEND_TIMOEOUT_MS));
    if (err != ESP_OK)
        ESP_LOGE(TAG, "%s: Error posting event to serviceweb task: %s", __func__, esp_err_to_name(err));
    return err;
}

static void evloop_ws_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    pp_websocket_data_t* wsdata = (pp_websocket_data_t*)event_data;
    // ESP_LOGI(TAG, "Payload: %s", wsdata->payload);

    cJSON* doc = cJSON_Parse(wsdata->payload);
    if (doc == NULL)
    {
        const char* error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL)
        {
            ESP_LOGE(TAG, "cJSON error : %s", error_ptr);
        }
        return;
    }

    const cJSON* cmd = cJSON_GetObjectItemCaseSensitive(doc, "cmd");
    if (!cJSON_IsString(cmd) || cmd->valuestring == NULL)
        goto exit;

    //----------------- publish -----------------
    if (0 == strcmp(cmd->valuestring, "publish"))
    {
        const cJSON* data = cJSON_GetObjectItemCaseSensitive(doc, "data");
        if (!cJSON_IsObject(data))
            goto exit;

        const cJSON* name = cJSON_GetObjectItemCaseSensitive(data, "name");
        if (!cJSON_IsString(name) || name->valuestring == NULL)
            goto exit;

        pp_t pp = pp_get(name->valuestring);
        if (pp == NULL)
            goto exit;

        const cJSON* value = cJSON_GetObjectItemCaseSensitive(data, "value");

        parameter_type_t pp_type = pp_get_type(pp);
        switch (pp_type)
        {
        case TYPE_STRING:
            if (cJSON_IsString(value) && value->valuestring != NULL)
                pp_post_write_string(pp, value->valuestring);
            else
                ESP_LOGW(TAG, "%s: Parameter %s is not string", __func__, pp_get_name(pp));
            break;
        case TYPE_BOOL:
            if (cJSON_IsNumber(value) || cJSON_IsBool(value))
                pp_post_write_bool(pp, value->valueint != 0);
            else
                ESP_LOGW(TAG, "%s: Parameter %s is not bool", __func__, pp_get_name(pp));
            break;
        case TYPE_FLOAT:
            if (cJSON_IsNumber(value))
                pp_post_write_float(pp, value->valuedouble);
            else
                ESP_LOGW(TAG, "%s: Parameter %s is not float", __func__, pp_get_name(pp));
            break;
        case TYPE_INT32:
            if (cJSON_IsNumber(value))
                pp_post_write_int32(pp, value->valueint);
            else
                ESP_LOGW(TAG, "%s: Parameter %s is not int32", __func__, pp_get_name(pp));
            break;
        case TYPE_INT64:
            if (cJSON_IsNumber(value))
                pp_post_write_int64(pp, value->valueint);
            else
                ESP_LOGW(TAG, "%s: Parameter %s is not int64", __func__, pp_get_name(pp));
            break;
        default:
            ESP_LOGE(TAG, "Publish for parameter %s of type %d not supported", pp_get_name(pp), pp_type);
            break;
        }
    }
    else
    {
        const cJSON* parname = cJSON_GetObjectItemCaseSensitive(doc, "data");
        if (!cJSON_IsString(parname) || parname->valuestring == NULL)
            goto exit;

        pp_t pp = pp_get(parname->valuestring);
        if (pp == NULL)
            goto exit;

        //----------------- subscribe -----------------
        if (0 == strcmp(cmd->valuestring, "subscribe"))
        {
            // If the parameter is already subscribed to or if subscribing is
            // successful, add the socket to the list of sockets for this parameter.
            if (par_exist(pp) || pp_subscribe(pp, evloop, evloop_newstate))
            {
                if (!par_socket_exist(pp, wsdata->socket))
                    par_add_socket(pp, wsdata->socket);
                write_to_json_buf(pp, RESP_MESSAGE, SUBSCRIBE_RESP, parname->valuestring);
                if (!httpss_websocket_send(wsdata->socket, json_buf))
                {
                    socket_closed_list.push_back(wsdata->socket);
                    par_cleanup();
                }
            }
            else
            {
                ESP_LOGI(TAG, "%s: Parameter %s does not exist", __func__, parname->valuestring);
                write_to_json_buf(pp, RESP_MESSAGE, SUBSCRIBE_RESP, "error");
                if (!httpss_websocket_send(wsdata->socket, json_buf))
                {
                    socket_closed_list.push_back(wsdata->socket);
                    par_cleanup();
                }
            }
        }
        //----------------- unsubscribe -----------------
        else if (0 == strcmp(cmd->valuestring, "unsubscribe"))
        {
            snprintf(json_buf, json_buf_size, UNSUBSCRIBE_MESSAGE, parname->valuestring);
            httpss_websocket_send(wsdata->socket, json_buf);
            par_remove_socket(pp, wsdata->socket);
            par_cleanup();
        }
        else
            ESP_LOGW(TAG, "%s: Unhandled command: %s", __func__, cmd->valuestring);
    }
exit:
    cJSON_Delete(doc);
}

void serviceweb_register_files(const char* basePath, const char* path)
{
    struct dirent* entry;
    struct stat statbuf;
    char* fullPath = (char*)malloc(10000);

    DIR* dp = opendir(path);
    if (dp == NULL)
    {
        printf("Unable to open directory %s: %s", path, strerror(errno));
        free(fullPath);
        return;
    }

    while ((entry = readdir(dp)) != NULL)
    {
        if (strcmp(".", entry->d_name) == 0 || strcmp("..", entry->d_name) == 0)
        {
            continue;
        }

        snprintf(fullPath, 10000, "%s/%s", path, entry->d_name);

        // Use stat to fill in the statbuf structure with information about the file/directory
        if (stat(fullPath, &statbuf) == -1)
        {
            perror("Failed to get file status");
            continue;
        }

        // If the entry is a directory, recurse into it
        if (S_ISDIR(statbuf.st_mode))
        {
            serviceweb_register_files(basePath, fullPath);
        }
        else
        {
            // Register URL for files, adjusting the path as before
            httpss_register_url(fullPath + strlen(basePath), false, resp_disk_file, HTTP_GET, NULL);

            // If the url ends with .gz, register the url without the .gz
            if (strstr(fullPath, ".gz") != NULL)
            {
                char* dot = strrchr(fullPath, '.');
                *dot = '\0';
                httpss_register_url(fullPath + strlen(basePath), false, resp_disk_file, HTTP_GET, NULL);
            }
        }
    }
    free(fullPath);
    closedir(dp);
}

bool serviceweb_register_memory_file(const char* path, const uint8_t* start, const uint8_t* end, bool gzip)
{
    file_get_map[path] = { start, end, gzip };
    return httpss_register_url(path, false, resp_memory_file, HTTP_GET, NULL);
}

void serviceweb_init(pp_evloop_t* evloop, char* buffer, size_t size, char* rxbuf, size_t rxsize, const char* root)
{
    web_root = root;

    ::evloop = evloop;
    json_buf = buffer;
    json_buf_size = size;

    rx_buf = rxbuf;
    rx_buf_size = rxsize;
}

void serviceweb_start(void)
{
    httpss_register_url("/ws", true, ws_handler, HTTP_GET, NULL);
    httpss_register_url("/update/web", false, web_post_handler, HTTP_POST, NULL);
    httpss_register_url("/update/firmware", false, ota_post_handler, HTTP_POST, NULL);
    httpss_register_url("/metrics", false, sysmon_get_handler, HTTP_GET, NULL);
    httpss_register_url("/info", false, sysmon_get_info, HTTP_GET, NULL);
    httpss_register_url("/partition", false, sysmon_get_partition, HTTP_GET, NULL);

    start_api_server();

    ESP_ERROR_CHECK(esp_event_handler_instance_register(ESP_HTTP_SERVER_EVENT, HTTP_SERVER_EVENT_ON_CONNECTED, &evloop_http_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(ESP_HTTP_SERVER_EVENT, HTTP_SERVER_EVENT_DISCONNECTED, &evloop_http_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register_with(evloop->loop_handle, evloop->base, CMD_SOCKET_CLOSED, &evloop_http_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register_with(evloop->loop_handle, evloop->base, CMD_WS_HANDLER, &evloop_ws_handler, NULL, NULL));
}

void serviceweb_stop(void)
{
}
