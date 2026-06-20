#pragma once

/**
 * ESP-NOW 허브 — Sens(C6) 노드들의 advertise 브로드캐스트 수신 + 미발견(미페어링) 노드 목록 관리.
 * 페어링 로직은 아직 없음 — 1초 주기 advertise를 받아 살아있는 노드 목록만 추적한다.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_now_link.h"

#define ESP_NOW_HUB_MAX_NODES 8
#define ESP_NOW_HUB_NODE_TIMEOUT_MS 3000U  /* advertise 주기(1s) 3회 누락 시 목록에서 제외 */

typedef struct {
    char    name[ESP_NOW_LINK_NAME_LEN];
    uint8_t mac[6];
    uint32_t last_seen_ms;
} esp_now_hub_node_t;

void esp_now_hub_init(void);

/* last_seen이 ESP_NOW_HUB_NODE_TIMEOUT_MS 이내인 노드만 out에 채워서 개수를 반환 */
int esp_now_hub_get_nodes(esp_now_hub_node_t *out, int max);

/* 화면 표기용 — "STA CH1 192.168.0.42" / "AP CH1 192.168.4.1" / "STA 연결 중..." 형태로 채움 */
void esp_now_hub_get_net_status(char *buf, size_t buflen);
