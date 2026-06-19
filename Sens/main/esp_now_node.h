#pragma once

/**
 * ESP-NOW 노드 신원 + 광고(advertise) 브로드캐스트.
 * Cntl(S3) 허브 쪽 탐색/페어링 구현 전까지는 단방향 광고만 수행한다.
 */

void esp_now_node_init(void);
const char *esp_now_node_get_name(void);
