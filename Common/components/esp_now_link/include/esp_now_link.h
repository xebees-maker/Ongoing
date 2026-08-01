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
    ESP_NOW_MSG_CAM_CONFIG_SET = 10,  /* Cntl -> CAM: 화이트밸런스/자동촬영 주기 설정 푸시 */
    ESP_NOW_MSG_UNPAIR = 11,          /* Cntl -> 노드: Cntl이 연결 해제했음을 알림(2026-07-31
                                        * 추가 — 이게 없으면 노드는 자기가 여전히 페어링된 줄
                                        * 알고 keepalive를 계속 보냄, 사용자가 실기로 확인) */
    ESP_NOW_MSG_CAPTURE_STATUS = 12,       /* CAM -> Cntl: 지금촬영 진행상태(접수/성공/실패) —
                                             * Cntl UI가 진행 팝업에 단계별로 표시하려고 추가 */
    ESP_NOW_MSG_PHOTO_LIST_REQUEST = 13,   /* Cntl -> CAM: 저장된 사진 "목록"만 요청(내용 전송 없음) */
    ESP_NOW_MSG_PHOTO_LIST_ENTRY = 14,     /* CAM -> Cntl: 목록 항목 1개(파일당 1개씩 반복 전송) */
    ESP_NOW_MSG_PHOTO_LIST_DONE = 15,      /* CAM -> Cntl: 목록 전송 끝 */
    ESP_NOW_MSG_PHOTO_DELETE_REQUEST = 16, /* Cntl -> CAM: 특정 사진 삭제 요청 */
    ESP_NOW_MSG_PHOTO_DELETE_ACK = 17,     /* CAM -> Cntl: 삭제 결과 */
    ESP_NOW_MSG_SET_TIME = 18,             /* Cntl -> 노드: 페어링 완료 시각 유닉스 타임스탬프
                                             * 전달(2026-07-31 추가 — Cntl은 보드 실장 PCF85063A
                                             * RTC가 있지만 CAM/Sens는 없어서, 페어링될 때마다
                                             * Cntl이 자기 시각을 상대에게 알려줌) */
    ESP_NOW_MSG_PHOTO_DELETE_ALL_REQUEST = 19,  /* Cntl -> CAM: 저장된 사진 전체 삭제 요청 */
    ESP_NOW_MSG_PHOTO_DELETE_ALL_ACK = 20,      /* CAM -> Cntl: 삭제 결과(삭제된 개수) */
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

/* Cntl -> 노드(유니캐스트): 사용자가 Cntl에서 연결 해제함. 받은 노드는 다시 페어링 전
 * 상태(광고/채널스캔)로 돌아가고 keepalive를 멈춰야 함 — 그 전엔 노드가 자기 상태를 알 방법이
 * 없어서 계속 페어링된 걸로 믿고 keepalive를 보냈음(실기로 확인된 문제, 2026-07-31). */
typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t msg_type;
} esp_now_unpair_t;

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
    PHOTO_REQUEST_MODE_BY_ID = 4,         /* param=file_id — 목록에서 고른 특정 사진 1장만 전송 */
} photo_request_mode_t;

typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  msg_type;
    uint8_t  mode;    /* photo_request_mode_t */
    uint32_t param;   /* RECENT_HOURS=시간 수, BY_ID=file_id, 나머지 모드는 무시 */
} esp_now_photo_request_t;

typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  msg_type;
    uint32_t file_id;     /* CAM이 부여하는 파일 식별자(타임스탬프 등, 유일하기만 하면 됨) */
    uint32_t total_size;  /* 파일 전체 바이트 수 */
    uint16_t total_chunks;
    uint32_t crc32;
} esp_now_photo_meta_t;

/* 200(v1.0 250바이트 한도 기준)에서 1200으로 증설(2026-08-01) — 양쪽 다 이미 ESP-NOW
 * v2.0으로 붙어있어서(부팅로그 "espnow [version: 2.0] init") 실제 한도는
 * ESP_NOW_MAX_DATA_LEN_V2=1470바이트. 헤더 10바이트 포함 1210 < 1470로 여유 있게 설정 —
 * 청크 개수가 1/6로 줄어서 전송 시간도 그만큼 단축(340KB 사진 기준 1704개→284개) */
#define ESP_NOW_PHOTO_CHUNK_DATA_LEN 1200

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

/* 지금촬영(CAPTURE_NOW) 진행 상태 — Cntl UI가 진행 팝업 단계 표시에 씀.
 * RECEIVED: CAM이 요청을 접수(아직 촬영 전). SUCCESS/FAILED: 촬영 자체의 성공/실패
 * (전송은 SUCCESS 뒤에 기존 META/CHUNK/DONE으로 이어짐, FAILED면 곧바로 DONE만 옴). */
typedef enum {
    CAM_CAPTURE_STATUS_RECEIVED = 0,
    CAM_CAPTURE_STATUS_SUCCESS  = 1,
    CAM_CAPTURE_STATUS_FAILED   = 2,
} cam_capture_status_t;

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t msg_type;
    uint8_t status;  /* cam_capture_status_t */
} esp_now_capture_status_t;

/* 사진 "목록"만 요청 — META/CHUNK로 실제 JPEG 내용을 보내는 것과 무관하게, 저장된 파일들의
 * file_id/kind/촬영시각/크기만 가볍게 나열해서 알려줌. 목록에서 하나를 고르면 그때
 * PHOTO_REQUEST(mode=BY_ID, param=file_id)로 실제 내용을 따로 요청.
 *
 * file_id는 더 이상 타임스탬프가 아님(2026-08-01 재설계 — 사용자 지적: "정석대로 가자,
 * 파일명에서 날짜/시간을 뽑지 말고 파일정보(파일명/날짜시간/크기)를 따로 받아야지"). CAM
 * 파일명은 <kind><4자리 base36 순번>.jpg(예: "M002A.jpg") — 순번은 수동(M)/자동(T)
 * 촬영이 공유하는 전역 카운터(0~36^4-1=1,679,615, 별도 카운터 파일 없이 SD에서 가장
 * 최근(mtime) 파일의 seq+1부터 이어감, CAM_STORAGE_MAX_FILES=500개 순환삭제 한도 대비
 * 충분히 여유). file_id는 이 순번 그 자체이고, kind는 이 순번이 M/T 파일 어느 쪽인지
 * 별도 필드로 알려줌(같은 순번이 두 kind에 동시에 존재할 수 없어서 file_id+kind면 항상
 * 유일하게 식별됨). 촬영시각은 파일의 FAT 수정시각을 그대로 읽어서 capture_time으로
 * 별도 전달함 — file_id를 파싱해서 시각을 뽑아내지 않음. */
typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t msg_type;
} esp_now_photo_list_request_t;

typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  msg_type;
    uint32_t file_id;        /* CAM의 M/T 공용 순번 — 위 설명 참고 */
    uint8_t  kind;            /* cam_capture_kind_t: 'M' 또는 'T' */
    uint32_t capture_time;   /* 파일의 FAT 수정시각(유닉스 타임스탬프) — 촬영시각 표시용 */
    uint32_t file_size;
} esp_now_photo_list_entry_t;

typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  msg_type;
    uint16_t count;
    uint32_t sd_total_kb;  /* CAM SD카드 전체 용량(KB) — 목록 옆에 사용량 %로 보여주려고
                             * 추가(2026-08-01). 0이면 CAM이 조회 실패했다는 뜻(구버전 CAM과도
                             * 호환 — 안 채워진 필드는 그냥 0으로 옴) */
    uint32_t sd_used_kb;   /* 사용 중인 용량(KB) */
} esp_now_photo_list_done_t;

typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  msg_type;
    uint32_t file_id;
} esp_now_photo_delete_request_t;

typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  msg_type;
    uint32_t file_id;
    uint8_t  success;
} esp_now_photo_delete_ack_t;

typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  msg_type;
    uint32_t unix_time;
} esp_now_set_time_t;

typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  msg_type;
} esp_now_photo_delete_all_request_t;

typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  msg_type;
    uint8_t  success;        /* SD/디렉터리 접근 자체가 실패했으면 0(이 경우 deleted_count 무의미) */
    uint16_t deleted_count;
} esp_now_photo_delete_all_ack_t;

/* CAM 원격 설정 — 화이트밸런스는 esp32-camera sensor_t::set_wb_mode()의 모드값(0~4)과
 * 그대로 일치시킴(0=Auto,1=Sunny,2=Cloudy,3=Office,4=Home). 촬영 주기는 초 단위,
 * 0=자동촬영 끔 — CAM의 기존 Kconfig 프리셋(10초/30분/1시간/3시간/10시간)과 동일한 값을
 * Cntl UI에서 골라 보냄(펌웨어 재빌드 없이 런타임으로 바뀌도록 하는 게 목적이지 임의의
 * 자유 입력 주기를 지원하려는 게 아님). */
typedef enum {
    CAM_WB_AUTO = 0,
    CAM_WB_SUNNY = 1,
    CAM_WB_CLOUDY = 2,
    CAM_WB_OFFICE = 3,
    CAM_WB_HOME = 4,
    CAM_WB_MODE_COUNT,
} cam_wb_mode_t;

typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  msg_type;
    uint8_t  wb_mode;               /* cam_wb_mode_t */
    uint32_t capture_interval_sec;  /* 0 = 자동 촬영 끔 */
} esp_now_cam_config_t;
