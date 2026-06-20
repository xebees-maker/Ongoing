#pragma once

#include <stdint.h>

#define ESP_NOW_LINK_VERSION 1
#define ESP_NOW_LINK_NAME_LEN 16

/* 모든 ESP-NOW 피어(Cntl 허브 + Sens 노드들)가 고정으로 공유하는 WiFi 채널.
 * 각자 독립 SoftAP를 호스팅하므로 누군가의 AP 채널을 동적으로 따라갈 수 없음 —
 * 하나의 라디오가 한 번에 한 채널만 들을 수 있어 N개 노드를 동시에 지원하려면
 * 채널이 빌드 시점에 고정·공유되어야 함. 바꾸려면 여기 한 곳만 바꾸면 됨. */
#define ESP_NOW_LINK_CHANNEL 1

typedef enum {
    ESP_NOW_MSG_ADVERTISE = 1,
    ESP_NOW_MSG_PAIR_REQUEST = 2,
    ESP_NOW_MSG_PAIR_ACK = 3,
    ESP_NOW_MSG_SENSOR_DATA = 4,
} esp_now_msg_type_t;

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t msg_type;
    char name[ESP_NOW_LINK_NAME_LEN];
    uint8_t mac[6];
} esp_now_advertise_t;
