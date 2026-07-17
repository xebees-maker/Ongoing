#include "web_dashboard.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_now_hub.h"
#include <stdio.h>

static const char *TAG = "web_dash";

static esp_err_t root_get_handler(httpd_req_t *req)
{
    static const char page[] =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<title>Cntl</title>"
        "<style>"
        "body{font-family:sans-serif;background:#1a1a2e;color:#fff;padding:16px}"
        "h2,h3{margin-top:0}"
        "#net{color:#9aa;font-size:0.85em;margin:-8px 0 12px}"
        ".card{border-radius:6px;background:#16213e;padding:10px 12px;margin-bottom:10px}"
        ".row{display:flex;justify-content:space-between;font-size:1em;padding:4px 0}"
        ".row .label{color:#9aa}"
        "</style></head><body>"
        "<h2>Cntl</h2>"
        "<div id='net'></div>"
        "<h3>센서 요약</h3>"
        "<div class='card' id='summary'></div>"
        "<h3>노드</h3>"
        "<div id='nodes'></div>"
        "<script>"
        /* sensor_channel_type_t(esp_now_link.h)와 값이 일치해야 함 */
        "const CHAN_LABEL={"
        "1:{name:'Temp',unit:'C',  fmt:v=>v.toFixed(1)},"
        "2:{name:'Humi',unit:'%',  fmt:v=>v.toFixed(0)},"
        "3:{name:'CO2', unit:'ppm',fmt:v=>String(Math.round(v))},"
        "};"
        "function summaryRow(t){"
        "  const lbl=CHAN_LABEL[t.type];"
        "  const val=t.has_data?(lbl.fmt(t.min)+'~'+lbl.fmt(t.max)+' '+lbl.unit+' (평균 '+lbl.fmt(t.avg)+')'):'--';"
        "  return '<div class=\"row\"><span class=\"label\">'+lbl.name+'</span><span>'+val+'</span></div>';"
        "}"
        "async function tick(){"
        "  const r=await fetch('/api/data'); const d=await r.json();"
        "  document.getElementById('net').textContent=d.net;"
        "  const s=d.summary;"
        "  document.getElementById('summary').innerHTML = s.count===0 ? '페어링된 노드 없음' :"
        "    '<div class=\"row\"><span class=\"label\">노드 수</span><span>'+s.count+'</span></div>'+"
        "    s.by_type.map(summaryRow).join('')+"
        "    '<div class=\"row\"><span class=\"label\">BATT</span><span>'+s.batt_pct_min+'~'+s.batt_pct_max+'% (평균 '+s.batt_pct_avg.toFixed(0)+'%)</span></div>';"
        "  const nc=document.getElementById('nodes'); nc.innerHTML='';"
        "  d.nodes.forEach(n=>{"
        "    const c=document.createElement('div'); c.className='card';"
        "    const chanRows=n.channels.map(ch=>{"
        "      const lbl=CHAN_LABEL[ch.type];"
        "      const val=ch.ok?(lbl.fmt(ch.val)+' '+lbl.unit):'--';"
        "      return '<div class=\"row\"><span class=\"label\">'+lbl.name+'</span><span>'+val+'</span></div>';"
        "    }).join('');"
        "    c.innerHTML='<h4 style=\"margin:0 0 6px 0\">'+n.name+'</h4>'+chanRows+"
        "      '<div class=\"row\"><span class=\"label\">BATT</span><span>'+(n.batt_ok?(n.batt_pct+'%  '+(n.powered?'USB':'BAT')):'--')+'</span></div>';"
        "    nc.appendChild(c);"
        "  });"
        "}"
        "tick(); setInterval(tick,1000);"
        "</script></body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t data_get_handler(httpd_req_t *req)
{
    char net_status[40];
    esp_now_hub_get_net_status(net_status, sizeof(net_status));

    esp_now_hub_summary_t sum;
    esp_now_hub_get_summary(&sum);

    esp_now_hub_node_t nodes[ESP_NOW_HUB_MAX_NODES];
    int count = esp_now_hub_get_nodes(nodes, ESP_NOW_HUB_MAX_NODES);

    char buf[3072];  /* 최대 ESP_NOW_HUB_MAX_NODES(8) * 채널 5개 + 요약까지 여유 있게 */
    int off = snprintf(buf, sizeof(buf),
        "{\"net\":\"%s\",\"summary\":{\"count\":%d,\"by_type\":[",
        net_status, sum.count);

    bool first_type = true;
    for (uint8_t type = SENSOR_CHAN_TEMP_C; type < SENSOR_CHAN_TYPE_COUNT; type++) {
        const esp_now_hub_channel_agg_t *agg = &sum.by_type[type];
        off += snprintf(buf + off, sizeof(buf) - off,
            "%s{\"type\":%d,\"has_data\":%s,\"min\":%.1f,\"max\":%.1f,\"avg\":%.1f}",
            first_type ? "" : ",", type, agg->has_data ? "true" : "false",
            agg->val_min, agg->val_max, agg->val_avg);
        first_type = false;
    }

    off += snprintf(buf + off, sizeof(buf) - off,
        "],\"batt_pct_min\":%d,\"batt_pct_max\":%d,\"batt_pct_avg\":%.1f},\"nodes\":[",
        sum.batt_pct_min, sum.batt_pct_max, sum.batt_pct_avg);

    bool first = true;
    for (int i = 0; i < count; i++) {
        if (!nodes[i].paired) continue;
        off += snprintf(buf + off, sizeof(buf) - off,
            "%s{\"name\":\"%s\",\"channels\":[",
            first ? "" : ",", nodes[i].name);
        for (int c = 0; c < nodes[i].chan_count; c++) {
            off += snprintf(buf + off, sizeof(buf) - off,
                "%s{\"type\":%d,\"ok\":%s,\"val\":%.1f}",
                c == 0 ? "" : ",", nodes[i].chan_type[c],
                nodes[i].chan_ok[c] ? "true" : "false", nodes[i].chan_val[c]);
        }
        off += snprintf(buf + off, sizeof(buf) - off,
            "],\"batt_ok\":%s,\"batt_pct\":%d,\"powered\":%s}",
            nodes[i].batt_ok ? "true" : "false", nodes[i].batt_pct,
            nodes[i].powered ? "true" : "false");
        first = false;
    }
    off += snprintf(buf + off, sizeof(buf) - off, "]}");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, off);
}

void web_dashboard_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start 실패");
        return;
    }

    static const httpd_uri_t root_uri = { .uri = "/",         .method = HTTP_GET, .handler = root_get_handler };
    static const httpd_uri_t data_uri = { .uri = "/api/data", .method = HTTP_GET, .handler = data_get_handler };
    httpd_register_uri_handler(server, &root_uri);
    httpd_register_uri_handler(server, &data_uri);
    ESP_LOGI(TAG, "웹 대시보드 시작됨");
}
