/**
 * @file    wifi_dashboard.c
 * @brief   C6 센서값을 폰 브라우저로 보기 위한 임시 WiFi 대시보드 (개발/테스트용)
 *
 * 테스트 단계: STA로 기존 AP에 접속, 같은 네트워크의 폰에서 접속.
 * 양산 단계: SoftAP로 전환(SENS_WIFI_MODE_AP) — Kconfig 선택지로 전환.
 */

#include "wifi_dashboard.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "history_log.h"
#include "esp_now_node.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "wifi_dash";

typedef struct {
    float dht_temp, dht_humi;
    bool  dht_ok;
    int   co2;
    float scd_temp, scd_humi;
    bool  scd_ok;
    int   batt_pct;
    bool  batt_ok;
    bool  powered;
} readings_t;

static readings_t s_readings = { 0 };

void wifi_dashboard_set_readings(float dht_temp, float dht_humi, bool dht_ok,
                                  int co2, float scd_temp, float scd_humi, bool scd_ok,
                                  int batt_pct, bool batt_ok, bool powered)
{
    s_readings.dht_temp = dht_temp;
    s_readings.dht_humi = dht_humi;
    s_readings.dht_ok   = dht_ok;
    s_readings.co2      = co2;
    s_readings.scd_temp = scd_temp;
    s_readings.scd_humi = scd_humi;
    s_readings.scd_ok   = scd_ok;
    s_readings.batt_pct = batt_pct;
    s_readings.batt_ok  = batt_ok;
    s_readings.powered  = powered;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    static const char page[] =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>C6 Sensor Node</title>"
        "<style>"
        "body{font-family:sans-serif;background:#1a1a2e;color:#fff;padding:16px}"
        "h2{margin-top:0}"
        "#rows{border-radius:6px;overflow:hidden;background:#16213e}"
        ".row{display:flex;align-items:center;gap:12px;font-size:1.2em;padding:12px 10px;"
        "cursor:pointer;border-bottom:1px solid #2a2a4a}"
        ".row:last-child{border-bottom:none}"
        ".row:active{background:#0f1830}"
        ".row .icon{width:1.3em;text-align:center}"
        ".row .label{flex:0 0 auto;color:#9aa}"
        ".row .value{flex:1;text-align:right;white-space:pre}"
        ".row .chev{color:#778;margin-left:4px}"
        "#modal{display:none;position:fixed;top:0;left:0;width:100%;height:100%;"
        "background:rgba(0,0,0,0.85);padding:16px;overflow:auto;box-sizing:border-box}"
        ".card{background:#16213e;border-radius:6px;padding:8px 12px;margin:8px 0;white-space:pre-line}"
        ".card h4{margin:0 0 4px 0}"
        "#close{display:inline-block;margin-bottom:12px;padding:6px 14px;"
        "background:#fff;color:#1a1a2e;border-radius:4px;cursor:pointer}"
        "</style></head><body>"
        "<h2 id='node-name'>C6 Sensor Node</h2>"
        "<div id='rows'></div>"
        "<div id='modal'>"
        "<div id='close' onclick=\"closeModal()\">&lt; Back</div>"
        "<h3 id='modal-title'></h3><div id='modal-body'></div>"
        "</div>"
        "<script>"
        "const METRICS=["
        "{name:'DH Temp', icon:'\\u{1F321}\\uFE0F', unit:'C',   ok:d=>d.dht_ok, val:d=>d.dht_temp.toFixed(1),"
        " row:d=>d.dht_temp.toFixed(1)+' C'},"
        "{name:'DH Humi', icon:'\\u{1F4A7}', unit:'%',   ok:d=>d.dht_ok, val:d=>d.dht_humi.toFixed(0),"
        " row:d=>d.dht_humi.toFixed(0)+' %'},"
        "{name:'SCD CO2', icon:'\\u{1F32B}\\uFE0F', unit:'ppm', ok:d=>d.scd_ok, val:d=>String(d.co2),"
        " row:d=>d.co2+' ppm'},"
        "{name:'SCD TEMP',icon:'\\u{1F321}\\uFE0F', unit:'C',   ok:d=>d.scd_ok, val:d=>d.scd_temp.toFixed(1),"
        " row:d=>d.scd_temp.toFixed(1)+' C'},"
        "{name:'SCD-Humi',icon:'\\u{1F4A7}', unit:'%',   ok:d=>d.scd_ok, val:d=>d.scd_humi.toFixed(0),"
        " row:d=>d.scd_humi.toFixed(0)+' %'},"
        "{name:'BATT',    icon:'\\u{1F50B}', unit:'%',   ok:d=>d.batt_ok,val:d=>d.batt_pct.toFixed(0),"
        " row:d=>d.batt_pct+'% '+(d.powered?'USB':'BAT')},"
        "];"
        "let rowsBuilt=false;"
        "function buildRows(){"
        "  const c=document.getElementById('rows'); c.innerHTML='';"
        "  METRICS.forEach((m,i)=>{"
        "    const r=document.createElement('div');"
        "    r.className='row'; r.id='row'+i; r.onclick=()=>openDetail(i);"
        "    r.innerHTML="
        "      '<span class=\"icon\">'+m.icon+'</span>'+"
        "      '<span class=\"label\">'+m.name+'</span>'+"
        "      '<span class=\"value\" id=\"val'+i+'\">--</span>'+"
        "      '<span class=\"chev\">&gt;</span>';"
        "    c.appendChild(r);"
        "  });"
        "  rowsBuilt=true;"
        "}"
        "async function tick(){"
        "  if(!rowsBuilt) buildRows();"
        "  const r=await fetch('/api/data'); const d=await r.json();"
        "  document.getElementById('node-name').textContent=d.name;"
        "  METRICS.forEach((m,i)=>{"
        "    document.getElementById('val'+i).textContent=m.ok(d)?m.row(d):'--';"
        "  });"
        "}"
        "async function openDetail(i){"
        "  const m=METRICS[i];"
        "  const r=await fetch('/api/history?metric='+i); const wins=await r.json();"
        "  document.getElementById('modal-title').textContent=m.name;"
        "  const body=document.getElementById('modal-body'); body.innerHTML='';"
        "  wins.forEach(w=>{"
        "    const card=document.createElement('div'); card.className='card';"
        "    if(!w.valid){"
        "      card.innerHTML='<h4>'+w.label+'</h4>no data yet';"
        "    } else {"
        "      card.innerHTML='<h4>'+w.label+'</h4>'+"
        "        'A '+w.avg.toFixed(1)+' '+m.unit+'\\n'+"
        "        'N '+w.min.toFixed(1)+' '+m.unit+' @ '+w.min_time+'\\n'+"
        "        'X '+w.max.toFixed(1)+' '+m.unit+' @ '+w.max_time;"
        "    }"
        "    body.appendChild(card);"
        "  });"
        "  document.getElementById('modal').style.display='block';"
        "}"
        "function closeModal(){ document.getElementById('modal').style.display='none'; }"
        "let touchX=0, touchY=0;"
        "const modalEl=document.getElementById('modal');"
        "modalEl.addEventListener('touchstart',e=>{"
        "  touchX=e.touches[0].clientX; touchY=e.touches[0].clientY;"
        "});"
        "modalEl.addEventListener('touchend',e=>{"
        "  const dx=e.changedTouches[0].clientX-touchX;"
        "  const dy=e.changedTouches[0].clientY-touchY;"
        "  if(Math.abs(dx)>60 && Math.abs(dx)>Math.abs(dy)*2) closeModal();"
        "});"
        "tick(); setInterval(tick,1000);"
        "</script></body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static void format_ts(time_t t, char *buf, size_t buflen)
{
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(buf, buflen, "%m/%d %H:%M", &tmv);
}

static esp_err_t history_get_handler(httpd_req_t *req)
{
    char query[32];
    int metric = -1;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[8];
        if (httpd_query_key_value(query, "metric", val, sizeof(val)) == ESP_OK) {
            metric = atoi(val);
        }
    }
    if (metric < 0 || metric >= HISTORY_METRIC_COUNT) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad metric");
        return ESP_FAIL;
    }

    static const struct { history_window_t w; const char *label; } windows[HISTORY_WINDOW_COUNT] = {
        { HISTORY_WINDOW_8H,    "Last 8 hours" },
        { HISTORY_WINDOW_DAY,   "Last 1 day"   },
        { HISTORY_WINDOW_WEEK,  "Last 1 week"  },
        { HISTORY_WINDOW_MONTH, "Last 1 month" },
    };

    char buf[1024];
    int off = snprintf(buf, sizeof(buf), "[");
    for (int i = 0; i < HISTORY_WINDOW_COUNT; i++) {
        history_stats_t st;
        bool ok = history_log_get_stats((history_metric_t)metric, windows[i].w, &st) && st.valid;
        char min_ts[24] = "", max_ts[24] = "";
        if (ok) {
            format_ts(st.min_time, min_ts, sizeof(min_ts));
            format_ts(st.max_time, max_ts, sizeof(max_ts));
        }
        off += snprintf(buf + off, sizeof(buf) - off,
            "%s{\"label\":\"%s\",\"valid\":%s,\"avg\":%.1f,\"min\":%.1f,\"min_time\":\"%s\",\"max\":%.1f,\"max_time\":\"%s\"}",
            i == 0 ? "" : ",", windows[i].label, ok ? "true" : "false",
            st.avg_val, st.min_val, min_ts, st.max_val, max_ts);
    }
    off += snprintf(buf + off, sizeof(buf) - off, "]");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, off);
}

static esp_err_t data_get_handler(httpd_req_t *req)
{
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
        "{\"name\":\"%s\","
        "\"dht_ok\":%s,\"dht_temp\":%.1f,\"dht_humi\":%.1f,"
        "\"scd_ok\":%s,\"co2\":%d,\"scd_temp\":%.1f,\"scd_humi\":%.1f,"
        "\"batt_ok\":%s,\"batt_pct\":%d,\"powered\":%s}",
        esp_now_node_get_name(),
        s_readings.dht_ok ? "true" : "false", s_readings.dht_temp, s_readings.dht_humi,
        s_readings.scd_ok ? "true" : "false", s_readings.co2, s_readings.scd_temp, s_readings.scd_humi,
        s_readings.batt_ok ? "true" : "false", s_readings.batt_pct,
        s_readings.powered ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

static void start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start 실패");
        return;
    }

    static const httpd_uri_t root_uri    = { .uri = "/",            .method = HTTP_GET, .handler = root_get_handler };
    static const httpd_uri_t data_uri    = { .uri = "/api/data",    .method = HTTP_GET, .handler = data_get_handler };
    static const httpd_uri_t history_uri = { .uri = "/api/history", .method = HTTP_GET, .handler = history_get_handler };
    httpd_register_uri_handler(server, &root_uri);
    httpd_register_uri_handler(server, &data_uri);
    httpd_register_uri_handler(server, &history_uri);
    ESP_LOGI(TAG, "웹 대시보드 시작됨");
}

/* dst는 호출 전에 0으로 초기화돼 있어야 함(wifi_config_t = {0}) — 남는 바이트는 그대로 0 유지 */
static void copy_str(uint8_t *dst, size_t dst_size, const char *src)
{
    size_t len = strlen(src);
    if (len > dst_size - 1) len = dst_size - 1;
    memcpy(dst, src, len);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
#if CONFIG_SENS_WIFI_MODE_STA
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi 연결 끊김 — 재시도");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "IP 받음: " IPSTR, IP2STR(&evt->ip_info.ip));
        start_http_server();
    }
#else
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "클라이언트 연결됨");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGI(TAG, "클라이언트 연결 해제");
    }
#endif
}

void wifi_dashboard_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

#if CONFIG_SENS_WIFI_MODE_STA
    esp_netif_create_default_wifi_sta();
#else
    esp_netif_create_default_wifi_ap();
#endif

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

#if CONFIG_SENS_WIFI_MODE_STA
    wifi_config_t wifi_cfg = { 0 };
    copy_str(wifi_cfg.sta.ssid, sizeof(wifi_cfg.sta.ssid), CONFIG_SENS_WIFI_SSID);
    copy_str(wifi_cfg.sta.password, sizeof(wifi_cfg.sta.password), CONFIG_SENS_WIFI_PASSWORD);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
#else
    wifi_config_t wifi_cfg = { 0 };
    copy_str(wifi_cfg.ap.ssid, sizeof(wifi_cfg.ap.ssid), CONFIG_SENS_WIFI_SSID);
    wifi_cfg.ap.ssid_len = strlen(CONFIG_SENS_WIFI_SSID);
    copy_str(wifi_cfg.ap.password, sizeof(wifi_cfg.ap.password), CONFIG_SENS_WIFI_PASSWORD);
    wifi_cfg.ap.max_connection = 4;
    wifi_cfg.ap.authmode = (strlen(CONFIG_SENS_WIFI_PASSWORD) == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg));
#endif

    ESP_ERROR_CHECK(esp_wifi_start());

#if CONFIG_SENS_WIFI_MODE_AP
    start_http_server();  /* AP는 GOT_IP 이벤트 없이 시작 시점에 이미 자체 IP가 있음 */
#endif
}
