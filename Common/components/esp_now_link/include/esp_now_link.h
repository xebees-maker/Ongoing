#pragma once

#include <stdint.h>

#define ESP_NOW_LINK_VERSION 2
#define ESP_NOW_LINK_NAME_LEN 16
#define ESP_NOW_MAX_CHANNELS 5   /* 레거시 LCD 콤보 앱(DHT22+SCD41 동시 장착)의 5채널까지 수용 */

/* Cntl이 독자 SoftAP만 호스팅하던 시절엔 모든 피어가 고정으로 공유하는 채널이었음.
 * 지금은 Cntl이 외부망(WiFi 공유기 STA/Cellular)에도 붙을 수 있어서 Cntl의 실제 채널이
 * 빌드 시점에 정해지지 않음 — 이 값은 Cntl이 독자 SoftAP로만 동작하는 경우의 기본값으로만
 * 쓰이고, Sens/CAM 등 리프 노드는 이 값에 의존하지 않고 채널 스캔(esp_now_node.c의
 * 광고/스캔 상태머신)으로 Cntl의 실제 채널을 찾는다. */
#define ESP_NOW_LINK_CHANNEL 1

typedef enum {
    ESP_NOW_MSG_ADVERTISE = 1,
    ESP_NOW_MSG_PAIR_REQUEST = 2,
    ESP_NOW_MSG_PAIR_ACK = 3,
    ESP_NOW_MSG_SENSOR_DATA = 4,
    ESP_NOW_MSG_ADVERTISE_ACK = 5,
    ESP_NOW_MSG_PHOTO_REQUEST = 6,
    ESP_NOW_MSG_PHOTO_META = 7,
    ESP_NOW_MSG_PHOTO_CHUNK = 8,
    ESP_NOW_MSG_PHOTO_DONE = 9,
} esp_now_msg_type_t;

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t msg_type;
    char name[ESP_NOW_LINK_NAME_LEN];
    uint8_t mac[6];
} esp_now_advertise_t;

/* Cntl이 ADVERTISE를 받으면 사람이 페어링 버튼을 누르기 전이라도 즉시 이걸 유니캐스트로
 * 돌려보낸다 — 채널 스캔 중인 노드가 "Cntl을 찾았다"를 알고 그 채널에 고정(더 이상 채널을
 * 옮기지 않음)하기 위한 용도. 정식 페어링(PAIR_REQUEST/PAIR_ACK)과는 별개. */
typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t msg_type;
    uint8_t hub_mac[6];
} esp_now_advertise_ack_t;

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t msg_type;
    uint8_t hub_mac[6];
} esp_now_pair_request_t;

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t msg_type;
    uint8_t node_mac[6];
} esp_now_pair_ack_t;

/* 채널 종류 — 노드가 실제로 붙인 센서가 무엇을 재는지에 대응.
 * 새 센서가 새로운 물리량을 재면 여기에 하나 추가하면 됨(프로토콜 구조 자체는 안 바뀜). */
typedef enum {
    SENSOR_CHAN_NONE = 0,
    SENSOR_CHAN_TEMP_C,
    SENSOR_CHAN_HUMI_PCT,
    SENSOR_CHAN_CO2_PPM,
    SENSOR_CHAN_TYPE_COUNT,   /* 배열 크기용 — 새 채널 종류 추가 시 항상 마지막에 유지 */
} sensor_channel_type_t;

/* 노드에 붙은 센서 모델 — Cntl UI에서 라벨 표시용, 채널 해석에는 안 씀 */
typedef enum {
    SENSOR_KIND_UNKNOWN = 0,
    SENSOR_KIND_SCD41,
    SENSOR_KIND_DHT22,
    SENSOR_KIND_SHT45,
    SENSOR_KIND_SHT40,
    SENSOR_KIND_DHT22_SCD41_COMBO,   /* 레거시 Waveshare LCD 콤보 앱 전용 */
} sensor_kind_t;

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t msg_type;
    uint8_t mac[6];
    uint8_t sensor_kind;                      /* sensor_kind_t */
    uint8_t chan_count;
    uint8_t chan_type[ESP_NOW_MAX_CHANNELS];  /* sensor_channel_type_t, [0..chan_count) 유효 */
    uint8_t chan_ok[ESP_NOW_MAX_CHANNELS];
    float   chan_val[ESP_NOW_MAX_CHANNELS];
    uint8_t batt_ok;
    int32_t batt_pct;
    uint8_t powered;
} esp_now_sensor_data_t;

/* CAM(카메라 노드) ↔ Cntl 사진 전송 — ESP-NOW는 패킷당 250바이트(v1.0)라 사진 1장을
 * 여러 청크로 쪼개서 보내야 함. TCP 같은 재전송/순서보장이 없어서 최소한의 자체
 * 프로토콜: META로 이번 파일 정보 먼저 알리고, CHUNK를 순서대로 보내고, 요청 전체가
 * 끝나면 DONE. 청크 재전송은 일단 안 만듦 — esp_now_send()의 send_cb 성공/실패로
 * CAM이 그 청크만 로컬 재시도하는 정도로 시작(과설계 방지, 실기 테스트 후 부족하면
 * Cntl 쪽 NACK/누락감지를 추가). */
typedef enum {
    PHOTO_REQUEST_MODE_ALL = 0,           /* SD에 있는 사진 전부 */
    PHOTO_REQUEST_MODE_RECENT_HOURS = 1,  /* 최근 N시간 이내 것 전부 (param=시간 수) */
    PHOTO_REQUEST_MODE_LATEST = 2,        /* 가장 최근 1장만 */
    PHOTO_REQUEST_MODE_CAPTURE_NOW = 3,   /* 기존 파일 무시하고 즉시 새로 촬영한 뒤 그 1장만 전송 */
} photo_request_mode_t;

typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  msg_type;
    uint8_t  mode;    /* photo_request_mode_t */
    uint32_t param;   /* RECENT_HOURS일 때만 유효, 나머지 모드는 무시 */
} esp_now_photo_request_t;

typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  msg_type;
    uint32_t file_id;     /* CAM이 부여하는 파일 식별자(타임스탬프 등, 유일하기만 하면 됨) */
    uint32_t total_size;  /* 파일 전체 바이트 수 */
    uint16_t total_chunks;
    uint32_t crc32;
} esp_now_photo_meta_t;

#define ESP_NOW_PHOTO_CHUNK_DATA_LEN 200  /* 헤더 포함 210바이트 — ESP-NOW v1.0 250바이트 한도 여유 */

typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  msg_type;
    uint32_t file_id;
    uint16_t chunk_idx;
    uint16_t chunk_len;   /* 마지막 청크는 이보다 작을 수 있음(파일 크기가 배수가 아닐 때) */
    uint8_t  data[ESP_NOW_PHOTO_CHUNK_DATA_LEN];
} esp_now_photo_chunk_t;

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t msg_type;
} esp_now_photo_done_t;
