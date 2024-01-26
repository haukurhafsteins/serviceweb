#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>
#include <map>
#include <list>
#include "esp_log.h"
#include "serviceweb.h"
#include "httpss.h"

#include "cJSON.h"
#include "pp.h"

#define RECV_BUFFER_SIZE 4096
#define MAX_FLOAT_BYTES 80
#define JSON_BUF_SIZE (512 * 10)
#define SEND_TIMOEOUT_MS 500

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

static const char *SUBSCRIBE_RESP = "subscribeResp";
static const char *RESP_MESSAGE = "{\"cmd\":\"%s\",\"data\":{\"name\":\"%s\", \"value\":";
static const char *NEWSTATE_FLOAT = "{\"cmd\":\"newState\",\"data\":{\"name\":\"%s\", \"value\":%f}}";
static const char *NEWSTATE_INT32 = "{\"cmd\":\"newState\",\"data\":{\"name\":\"%s\", \"value\":%ld}}";
static const char *NEWSTATE_FLOAT_ARRAY = "{\"cmd\":\"newState\",\"data\":{\"name\":\"%s\",\"value\":\"float\"}}";
static const char *UNSUBSCRIBE_MESSAGE = "{\"cmd\":\"unsubscribeResp\",\"data\":\"%s\"}";

static const char *NNEWSTATE_FLOAT = "{\"f\":\"%s\"}";

static char TAG[] = "SERVWEB";
static char *json_buf = (char *)malloc(JSON_BUF_SIZE);
static pp_evloop_t evloop;
ESP_EVENT_DEFINE_BASE(SERVWEB_EVENTS);

static void evloop_newstate(void *handler_arg, esp_event_base_t base, int32_t id, void *context);

esp_err_t get_index(httpd_req_t *req)
{
    const int bufsize = 1024;
    char *buf = (char *)calloc(bufsize, sizeof(char));
    size_t len = snprintf(buf, bufsize, "AsgardGrip");
    httpd_resp_send_chunk(req, buf, len);
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send_chunk(req, buf, 0);
    return ESP_OK;
}

esp_err_t get_web(httpd_req_t *req)
{
    int read = 0;
    const int bufsize = 1024;
    char *buf = (char *)calloc(bufsize, sizeof(char));
    bool gzip_supported = false;

    char encoding[64];
    httpd_req_get_hdr_value_str(req, "Accept-Encoding", encoding, sizeof(encoding));
    if (strstr(encoding, "gzip"))
        gzip_supported = true;

    snprintf(buf, bufsize, "/spiffs%s%s", req->uri, gzip_supported ? ".gz" : "");
    FILE *file = fopen(buf, "rb");
    if (file == NULL)
    {
        ESP_LOGE(TAG, "File %s does not exist, going for non .gz file...", buf);
        gzip_supported = false;
        snprintf(buf, bufsize, "/spiffs%s", req->uri);
        file = fopen(buf, "rb");
        if (file == NULL)
        {
            ESP_LOGE(TAG, "File %s does not exist!", buf);
            free(buf);
            return ESP_FAIL;
        }
    }

    // ESP_LOGW(TAG, "Serving %s", buf);

    if (strstr(req->uri, ".html") != NULL)
        httpd_resp_set_type(req, "text/html");
    else if (strstr(req->uri, ".js") != NULL)
        httpd_resp_set_type(req, "application/javascript");
    else if (strstr(req->uri, ".css") != NULL)
        httpd_resp_set_type(req, "text/css");
    else if (strstr(req->uri, ".svg") != NULL)
        httpd_resp_set_type(req, "image/svg+xml");

    if (gzip_supported)
    {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }

    while ((read = fread(buf, 1, bufsize, file)) > 0)
    {
        httpd_resp_send_chunk(req, buf, read);
    }

    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send_chunk(req, buf, 0);

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
            pp_unsubscribe(it->first, &evloop, evloop_newstate);
            it = pp_list.erase(it);
        }
        else
            it++;
    }
}

static void par_send_to_sockets(pp_t pp, char *json)
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

static void par_send_binary_to_sockets(pp_t pp, char *json, size_t json_len, void *bin, size_t bin_len)
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
        const char *name = pp_get_name(pp);
        size_t len = strlen(NEWSTATE_INT32) + strlen(name) + MAX_FLOAT_BYTES;
        char *json = (char *)malloc(len);
        snprintf(json, len, NEWSTATE_INT32, name, i);
        par_send_to_sockets(pp, json);
        free(json);
    }
    return true;
}

static bool web_post_newstate_float(pp_t pp, float f)
{
    if (!par_list_empty())
    {
        const char *name = pp_get_name(pp);
        size_t len = strlen(NEWSTATE_FLOAT) + strlen(name) + MAX_FLOAT_BYTES;
        char *json = (char *)malloc(len);
        snprintf(json, len, NEWSTATE_FLOAT, name, f);
        par_send_to_sockets(pp, json);
        free(json);
    }
    return true;
}

static bool web_post_newstate_float_array(pp_t pp, pp_float_array_t *fsrc)
{
    if (!par_list_empty())
    {
        const char *name = pp_get_name(pp);
        size_t len = 64;
        char *json = (char *)calloc(1, len);
        len = snprintf(json, len, NNEWSTATE_FLOAT, name) + 1;
        len = (len % 4) ? (len + 4) / 4 * 4 : len;
        par_send_binary_to_sockets(pp, json, len, fsrc->data, fsrc->len * sizeof(float));
        free(json);
    }
    return true;
}

static void write_to_json_buf(pp_t pp, const char *format, ...)
{
    memset(json_buf, 0, JSON_BUF_SIZE);

    va_list valist;
    va_start(valist, format);
    int len = vsnprintf(json_buf, JSON_BUF_SIZE, format, valist);
    if (len < 0)
    {
        ESP_LOGE(TAG, "%s: Error, vsnprintf return negative", __func__);
        return;
    }
    va_end(valist);
    size_t read = JSON_BUF_SIZE - len;
    if (pp_get_as_string(pp, &json_buf[len], &read, false))
        strncat(json_buf, "}}", JSON_BUF_SIZE - len - read);
    else
        strncat(json_buf, "\"\"}}", JSON_BUF_SIZE - len);
}

static void evloop_newstate(void *handler_arg, esp_event_base_t base, int32_t id, void *context)
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
        web_post_newstate_int32(pp, *((int32_t *)context));
        break;
    case TYPE_BOOL:
        web_post_newstate_int32(pp, *((bool *)context));
        break;
    case TYPE_FLOAT:
        web_post_newstate_float(pp, *((float *)context));
        break;
    case TYPE_FLOAT_ARRAY:
        web_post_newstate_float_array(pp, ((pp_float_array_t *)context));
        break;
    default:
        ESP_LOGW(TAG, "unsupported type %d", type);
        break;
    }
}

static void evloop_http_event(void *handler_arg, esp_event_base_t base, int32_t id, void *context)
{
    esp_err_t err;
    int socfd = *(int *)context;
    switch (id)
    {
    case HTTP_SERVER_EVENT_DISCONNECTED:
        err = esp_event_post_to(evloop.loop_handle, evloop.base, CMD_SOCKET_CLOSED, &socfd, sizeof(int), pdMS_TO_TICKS(SEND_TIMOEOUT_MS));
        if (err != ESP_OK)
            ESP_LOGE(TAG, "%s: Error posting event: %s", __func__, esp_err_to_name(err));
        break;
    case CMD_SOCKET_CLOSED:
        ESP_LOGI(TAG, "%s: socket %d closed", __func__, socfd);
        socket_closed_list.push_back(socfd);
        par_cleanup();
        break;
    default:
        break;
    }
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    // ESP_LOGI(TAG, "%s: method is %d", __func__, req->method);

    if (req->method == HTTP_GET)
        return ESP_OK;

    uint8_t receive_buffer[RECV_BUFFER_SIZE];
    memset(receive_buffer, 0, RECV_BUFFER_SIZE);
    pp_websocket_data_t *wsdata = (pp_websocket_data_t *)receive_buffer;
    httpd_ws_frame_t ws_pkt = {};
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    ws_pkt.payload = (uint8_t *)wsdata->payload;
    if (ESP_OK != httpd_ws_recv_frame(req, &ws_pkt, RECV_BUFFER_SIZE - 1 - sizeof(pp_websocket_data_t)))
    {
        ESP_LOGE(TAG, "%s: Error receiving websocket frame", __func__);
        return ESP_FAIL;
    }

    ws_pkt.payload[ws_pkt.len] = 0;
    wsdata->socket = httpd_req_to_sockfd(req);

    // Send data to serveiceweb task
    esp_err_t err = esp_event_post_to(evloop.loop_handle, evloop.base, CMD_WS_HANDLER, wsdata, sizeof(pp_websocket_data_t) + ws_pkt.len, pdMS_TO_TICKS(SEND_TIMOEOUT_MS));
    if (err != ESP_OK)
        ESP_LOGE(TAG, "%s: Error posting event to serviceweb task: %s", __func__, esp_err_to_name(err));
    ESP_ERROR_CHECK(err);
    return err;
}

static void evloop_ws_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    pp_websocket_data_t *wsdata = (pp_websocket_data_t *)event_data;
    // ESP_LOGI(TAG, "Payload: %s", wsdata->payload);

    cJSON *doc = cJSON_Parse(wsdata->payload);
    if (doc == NULL)
    {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL)
        {
            ESP_LOGE(TAG, "cJSON error : %s", error_ptr);
        }
        return;
    }

    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(doc, "cmd");
    if (!cJSON_IsString(cmd) || cmd->valuestring == NULL)
        goto exit;

    //----------------- publish -----------------
    if (0 == strcmp(cmd->valuestring, "publish"))
    {
        const cJSON *data = cJSON_GetObjectItemCaseSensitive(doc, "data");
        if (!cJSON_IsObject(data))
            goto exit;

        const cJSON *name = cJSON_GetObjectItemCaseSensitive(data, "name");
        if (!cJSON_IsString(name) || name->valuestring == NULL)
            goto exit;

        pp_t pp = pp_get(name->valuestring);
        if (pp == NULL)
            goto exit;

        const cJSON *value = cJSON_GetObjectItemCaseSensitive(data, "value");

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
        default:
            ESP_LOGE(TAG, "Publish for parameter %s of type %d not supported", pp_get_name(pp), pp_type);
            break;
        }
    }
    else
    {
        const cJSON *parname = cJSON_GetObjectItemCaseSensitive(doc, "data");
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
            if (par_exist(pp) || pp_subscribe(pp, &evloop, evloop_newstate))
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
            snprintf(json_buf, JSON_BUF_SIZE, UNSUBSCRIBE_MESSAGE, parname->valuestring);
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

char *httpss_get_float_array_hdr(char *name, char *buf, size_t buf_size)
{
    size_t len = strlen(NEWSTATE_FLOAT_ARRAY) + strlen(name) + 1; // +1: null term
    if (len > buf_size)
        return NULL;
    snprintf(buf, buf_size, NEWSTATE_FLOAT_ARRAY, name);
    return buf;

    // size_t len = tmp_len;
    // if (tmp_len / 4 * 4 != tmp_len)
    //     len = (tmp_len + 4) / 4 * 4;

    // for (int i= 0;i<)
    // if (len / 4 * 4 != len)
    //     len = (len + 4) / 4 * 4;
}

void register_files(const char *basePath, const char *path)
{
    struct dirent *entry;
    // struct stat statbuf;
    char fullPath[512];

    DIR *dp = opendir(path);
    if (dp == NULL)
    {
        perror("Unable to open directory");
        return;
    }

    while ((entry = readdir(dp)) != NULL)
    {
        if (strcmp(".", entry->d_name) == 0 || strcmp("..", entry->d_name) == 0)
        {
            continue;
        }

        snprintf(fullPath, sizeof(fullPath), "%s/%s", path, entry->d_name);
        // stat(fullPath, &statbuf);

        // if (S_ISDIR(statbuf.st_mode))
        // {
        //     printf("%s/\n", fullPath + strlen(basePath));
        //     httpss_register_url(fullPath + strlen(basePath), false, get_web, HTTP_GET);
        //     // Recur into the subdirectory
        //     register_files(basePath, fullPath);
        // }
        // else
        {
            httpss_register_url(fullPath + strlen(basePath), false, get_web, HTTP_GET);
            // printf("%s\n", fullPath + strlen(basePath));
        }
    }

    closedir(dp);
}

void serviceweb_init()
{
    esp_event_loop_args_t loop_args = {
        .queue_size = 20,
        .task_name = "servweb",
        .task_priority = 4,
        .task_stack_size = 16384,
        .task_core_id = 1};

    evloop.base = SERVWEB_EVENTS;
    esp_event_loop_create(&loop_args, &evloop.loop_handle);
}

void serviceweb_start(void)
{
    httpss_register_url("/", false, get_index, HTTP_GET);
    httpss_register_url("/ws", true, ws_handler, HTTP_GET);

    const char *path = "/spiffs/";
    register_files(path, path);

    ESP_ERROR_CHECK(esp_event_handler_instance_register(ESP_HTTP_SERVER_EVENT, HTTP_SERVER_EVENT_DISCONNECTED, &evloop_http_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register_with(evloop.loop_handle, evloop.base, CMD_SOCKET_CLOSED, &evloop_http_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register_with(evloop.loop_handle, evloop.base, CMD_WS_HANDLER, &evloop_ws_handler, NULL, NULL));
}

void serviceweb_stop(void)
{
}
