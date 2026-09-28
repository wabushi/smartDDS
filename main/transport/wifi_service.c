#include "wifi_service.h"

#include <stdlib.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "transport/command_api.h"

static const char *TAG = "WIFI_API";
static httpd_handle_t server;

#define WIFI_API_COMMAND_BUFFER_SIZE 512
#define WIFI_API_RESPONSE_BUFFER_SIZE 3072

/* URI handlers run serially in the HTTP server task. Keeping these large
 * buffers outside that task's stack avoids stack-protection faults. */
static char s_command_buffer[WIFI_API_COMMAND_BUFFER_SIZE];
static char s_response_buffer[WIFI_API_RESPONSE_BUFFER_SIZE];

static void set_json_headers(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET,POST,OPTIONS");
}

static esp_err_t options_handler(httpd_req_t *req)
{
    set_json_headers(req);
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t health_handler(httpd_req_t *req)
{
    set_json_headers(req);
    return httpd_resp_sendstr(req, "{\"ok\":true,\"service\":\"AD9834-DDS\"}");
}

static esp_err_t state_handler(httpd_req_t *req)
{
    set_json_headers(req);
    int response_len = command_api_get_state_json(s_response_buffer,
                                                  sizeof(s_response_buffer));
    if (response_len < 0 || (size_t)response_len >= sizeof(s_response_buffer)) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"state unavailable\"}");
    }
    return httpd_resp_send(req, s_response_buffer, response_len);
}

static esp_err_t command_handler(httpd_req_t *req)
{
    size_t received = 0;
    set_json_headers(req);
    if (req->content_len == 0 || req->content_len >= sizeof(s_command_buffer)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"invalid command length\"}");
    }
    while (received < req->content_len) {
        int n = httpd_req_recv(req, s_command_buffer + received,
                               req->content_len - received);
        if (n <= 0) {
            httpd_resp_set_status(req, "408 Request Timeout");
            return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"request incomplete\"}");
        }
        received += (size_t)n;
    }
    s_command_buffer[received] = '\0';
    ESP_LOGI(TAG, "LAN command received (%u bytes)", (unsigned)received);
    int response_len = command_api_execute_json(s_command_buffer,
                                                s_response_buffer,
                                                sizeof(s_response_buffer));
    if (response_len < 0 || (size_t)response_len >= sizeof(s_response_buffer)) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"command response unavailable\"}");
    }
    if (strstr(s_response_buffer, "\"ok\":false") != NULL) {
        httpd_resp_set_status(req, "400 Bad Request");
    }
    return httpd_resp_send(req, s_response_buffer, response_len);
}

esp_err_t wifi_service_start(void)
{
    if (server) return ESP_OK;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) return err;
    const httpd_uri_t routes[] = {
        {.uri="/api/health", .method=HTTP_GET, .handler=health_handler},
        {.uri="/api/state", .method=HTTP_GET, .handler=state_handler},
        {.uri="/api/command", .method=HTTP_POST, .handler=command_handler},
        {.uri="/api/command", .method=HTTP_OPTIONS, .handler=options_handler},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) return err;
    }
    ESP_LOGI(TAG, "LAN command API listening on port %u", (unsigned)config.server_port);
    return ESP_OK;
}
