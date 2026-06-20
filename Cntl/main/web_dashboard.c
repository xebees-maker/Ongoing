#include "web_dashboard.h"

#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "web_dash";

static esp_err_t root_get_handler(httpd_req_t *req)
{
    static const char page[] =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>CNTL</title>"
        "<style>body{font-family:sans-serif;background:#1a1a2e;color:#fff;"
        "display:flex;align-items:center;justify-content:center;height:100vh;margin:0}"
        "h1{font-size:3em}</style></head>"
        "<body><h1>CNTL</h1></body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

void web_dashboard_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start 실패");
        return;
    }

    static const httpd_uri_t root_uri = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler };
    httpd_register_uri_handler(server, &root_uri);
    ESP_LOGI(TAG, "웹 대시보드 시작됨 (자리표시자: CNTL)");
}
