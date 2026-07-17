#pragma once

/**
 * ESP-NOW 허브 — Sens(C6) 노드들의 advertise 브로드캐스트 수신, 페어링(PAIR_REQUEST/ACK),
 * 페어링된 노드의 SENSOR_DATA 수신/저장 + 종합(최대/최소/평균) 집계.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_now_link.h"

#define ESP_NOW_HUB_MAX_NODES 8
#define ESP_NOW_HUB_NODE_TIMEOUT_MS 3000U  /* advertise/SENSOR_DATA 주기(1s) 3회 누락 시 목록에서 제외 */

typedef struct {
    char    name[ESP_NOW_LINK_NAME_LEN];
    uint8_t mac[6];
    uint32_t last_seen_ms;   /* 마지막 advertise 수신 시각 (미페어링 노드 발견용) */

    bool    paired;
    uint32_t last_data_ms;   /* 마지막 SENSOR_DATA 수신 시각 (페어링 후 생존 판단용) */

    uint8_t sensor_kind;                      /* sensor_kind_t */
    uint8_t chan_count;
    uint8_t chan_type[ESP_NOW_MAX_CHANNELS];  /* sensor_channel_type_t */
    uint8_t chan_ok[ESP_NOW_MAX_CHANNELS];
    float   chan_val[ESP_NOW_MAX_CHANNELS];

    bool    batt_ok;
    int     batt_pct;
    bool    powered;
} esp_now_hub_node_t;

/* 채널 타입(sensor_channel_type_t)별 집계 — 노드가 몇 개든, 무슨 센서를 붙였든
 * 같은 타입(TEMP_C/HUMI_PCT/CO2_PPM)의 채널끼리만 묶어서 min/max/avg를 낸다 */
typedef struct {
    bool  has_data;
    float val_min, val_max, val_avg;
} esp_now_hub_channel_agg_t;

typedef struct {
    int   count;        /* 집계에 포함된 페어링 노드 수 */
    esp_now_hub_channel_agg_t by_type[SENSOR_CHAN_TYPE_COUNT];  /* index = sensor_channel_type_t */
    int   batt_pct_min, batt_pct_max;
    float batt_pct_avg;
} esp_now_hub_summary_t;

void esp_now_hub_init(void);

/* 미페어링 노드는 last_seen, 페어링 노드는 last_data 기준으로 ESP_NOW_HUB_NODE_TIMEOUT_MS
 * 이내인 것만 out에 채워서 개수를 반환 */
int esp_now_hub_get_nodes(esp_now_hub_node_t *out, int max);

/* 노드 탭에서 사용자가 특정 노드를 선택했을 때 호출 — PAIR_REQUEST 유니캐스트 전송.
 * mac은 esp_now_hub_get_nodes()로 얻은 노드의 mac[6] */
void esp_now_hub_pair(const uint8_t *mac);

/* 페어링되어 살아있는(timeout 이내) 노드들의 metric별 max/min/avg */
void esp_now_hub_get_summary(esp_now_hub_summary_t *out);

/* 화면 표기용 — "STA CH1 192.168.0.42" / "AP CH1 192.168.4.1" / "STA 연결 중..." 형태로 채움 */
void esp_now_hub_get_net_status(char *buf, size_t buflen);
