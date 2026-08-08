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
    ESP_NOW_MSG_HUB_RESET = 21,             /* Cntl -> 전체 브로드캐스트: Cntl이 방금 부팅함
                                              * (2026-08-02 추가). 노드가 이미 페어링된 줄 알고
                                              * ADVERTISE를 멈춘 채 keepalive만 보내는 중이면(=
                                              * 원래 상대인 Cntl이 소프트리셋 등으로 재부팅해서
                                              * 노드 테이블이 비어버린 상태) UNPAIR와 마찬가지로
                                              * 강제로 재광고 모드로 돌아가게 함 — ESP-NOW의
                                              * send_cb 성공/실패는 물리 계층 ACK 기준이라
                                              * 애플리케이션이 그 keepalive를 무시하고 있어도
                                              * 노드 쪽에서는 "성공"으로만 보여서 스스로는 절대
                                              * 이 상태를 못 벗어남(실기로 확인) */
    ESP_NOW_MSG_PHOTO_CHUNK_NACK = 22,      /* Cntl -> CAM: 청크 일련번호(chunk_idx) 기준으로
                                              * 못 받은 것만 콕 집어 재전송 요청(2026-08-03 —
                                              * 아래 esp_now_photo_chunk_nack_t 주석 참고).
                                              * 이름은 PHOTO_로 남겨뒀지만 구조(일련번호 기반
                                              * 스트리밍+선택적 재전송)는 사진 전용이 아니라
                                              * 나중에 다른 대용량 전송(Sens 등)에도 그대로
                                              * 재사용 가능한 일반적인 패턴 */
    ESP_NOW_MSG_BENCH_BLAST = 23,           /* CAM -> Cntl: 처리량 벤치마크용 더미 바이트(내용
                                              * 무의미) — Cntl은 받는 즉시 버리고 바이트 수만
                                              * 카운트. 프로토콜 오버헤드 없는 순수 채널 처리량
                                              * 실측용(2026-08-04, esp_now_reliable 설계 착수
                                              * 전 기준치 측정) */
    ESP_NOW_MSG_BENCH_START = 24,           /* Cntl -> CAM: 벤치마크 트리거(N초간 BLAST 최대
                                              * 속도 전송 시작). Cntl 설정화면의 임시 버튼에서
                                              * 보냄 */
    ESP_NOW_MSG_CHANNEL_PING = 25,           /* 노드 -> 허브: 순수 생존/채널동기 확인 전용
                                               * (2026-08-04, esp_now_channelsync 설계 — 페어링
                                               * 여부와 무관하게 "지금 이 채널에서 서로 닿는지"만
                                               * 확인. PAIR_ACK 재사용 keepalive를 대체함 —
                                               * 그건 "살아있음"과 "페어링됨"의 의미가 섞여
                                               * 있었음). */
    ESP_NOW_MSG_CHANNEL_PONG = 26,           /* 허브 -> 노드: CHANNEL_PING 응답. 허브는 페어링
                                               * 여부와 무관하게 자기가 지금 있는 채널에서 받은
                                               * PING엔 항상 응답함 — 생존확인은 페어링 승인과
                                               * 별개 개념이므로. */
    ESP_NOW_MSG_PHOTO_DONE_ACK = 27,        /* Cntl -> CAM: PHOTO_DONE에 대한 응답, "항상" 보냄
                                              * (2026-08-05, esp_now_reliable Layer 1 설계 —
                                              * 예전 NACK은 "문제 있을 때만" 보내서 정상 종료를
                                              * CAM이 구분할 방법이 없었음). missing_count=0이면
                                              * 완료, 아니면 기존 esp_now_photo_chunk_nack_t
                                              * 구조체 그대로 재사용해 누락분을 실어보냄 */
    ESP_NOW_MSG_PHOTO_LIST_DONE_ACK = 28,   /* Cntl -> CAM: PHOTO_LIST_DONE에 대한 응답, 개수
                                              * 일치 여부와 무관하게 "항상" 보냄(위와 동일 원칙) */
    ESP_NOW_MSG_CAPTURE_STATUS_ACK = 29,    /* Cntl -> CAM: CAPTURE_STATUS(최종 SUCCESS/FAILED)
                                              * 에 대한 최소 확인 응답 — CAM이
                                              * esp_now_reliable_request()로 감쌀 수 있게 함 */
    ESP_NOW_MSG_PHOTO_WINDOW_STATUS_REQUEST = 30,  /* CAM -> Cntl: Selective Repeat 실험(2026-08-05)
                                              * — 전송 도중 윈도우 하나(range_start부터
                                              * range_count개) 다 보낼 때마다 그 범위 안에서
                                              * 뭘 못 받았는지 물어봄. 기존 DONE/NACK는 파일
                                              * 전체를 다 보낸 "끝에" 한 번만 확인하는데, 이건
                                              * 그걸 전송 도중 여러 번 하는 버전 — 어느 쪽이
                                              * 실제로 더 빠른지 BMT로 실측 비교하는 게 목적
                                              * (project_cntl_cam_esp_now_reliability_layers
                                              * 메모리 참고, 기존 방식은 손대지 않고 나란히
                                              * 추가). */
    ESP_NOW_MSG_PHOTO_WINDOW_STATUS_ACK = 31,      /* Cntl -> CAM: 위 요청에 대한 응답 — 새 구조체
                                              * 안 만들고 esp_now_photo_chunk_nack_t를 msg_type만
                                              * 바꿔 그대로 재사용(PHOTO_DONE_ACK와 동일 이유:
                                              * "누락 목록을 알려준다"는 의미가 이미 똑같음,
                                              * range_count가 SR_WINDOW_SIZE 이하로 고정이라
                                              * missing_count가 400 상한을 넘을 일도 없음). */
    ESP_NOW_MSG_CAM_CONFIG_ACK = 32,        /* CAM -> Cntl: CAM_CONFIG_SET 적용 결과(2026-08-08,
                                              * 절전/응답성 설정 추가하면서 Cntl이 esp_now_reliable_
                                              * request()로 감쌀 수 있게 응답 메시지 신설 — 예전엔
                                              * CAM_CONFIG_SET을 보내기만 하고 확인이 없었음) */
} esp_now_msg_type_t;

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t msg_type;
} esp_now_hub_reset_t;

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

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t msg_type;
} esp_now_channel_ping_t;

typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t msg_type;
} esp_now_channel_pong_t;

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

/* 청크 전송 신뢰성 재설계(2026-08-03) — "핸드셰이크처럼 매 청크마다 응답을 기다리면서도,
 * 정작 그 응답이 로컬 라디오의 물리계층 ACK일 뿐 상대 애플리케이션이 실제로 받았다는
 * 진짜 확인이 아니었던" 이전 설계를 버림(사용자 지적: "그거 handshake도 아니고 streaming도
 * 아니고 최악의 조합이야"). 신뢰도 높은 브로드캐스팅에서 흔히 쓰는 방식으로 교체:
 * 1) 송신측은 청크마다 응답을 기다리지 않고 그냥 순서대로 쭉 보냄(스트리밍) — chunk_idx가
 *    이미 일련번호 역할을 함(esp_now_photo_chunk_t 참고).
 * 2) DONE(한 바퀴 다 보냈다는 신호)까지 받으면, 수신측은 어느 chunk_idx가 비었는지
 *    확인해서 그 목록만 콕 집어 NACK으로 재전송 요청.
 * 3) 송신측은 요청받은 것만 다시 보내고 다시 DONE — 이걸 정해진 횟수만큼 반복.
 * 이러면 매 청크 왕복 대기가 없어서 빠르고, 신뢰성은 "진짜 못 받은 것"을 정확히 짚어서
 * 다시 받는 방식이라 로컬 라디오 ACK의 애매함에 의존하지 않음. */
#define ESP_NOW_PHOTO_NACK_MAX_INDICES 400  /* ESP-NOW v2 1470B 한도 안에서 여유있게 잡은 값 —
                                              * 헤더(10B) + 400*2B = 810B < 1470B */
typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  msg_type;
    uint32_t file_id;
    uint16_t missing_count;  /* missing_idx[0..missing_count) 유효 */
    uint16_t missing_idx[ESP_NOW_PHOTO_NACK_MAX_INDICES];
} esp_now_photo_chunk_nack_t;

/* ESP_NOW_MSG_PHOTO_DONE_ACK(2026-08-05, Layer 1 Reliable 모드)는 이 구조체를 msg_type만
 * 바꿔서 그대로 재사용함 — missing_count=0이면 완료, 아니면 지금의 NACK과 똑같이 누락
 * chunk_idx 목록을 실음. 굳이 새 구조체를 안 만든 이유: "누락분을 알려준다"는 의미 자체가
 * 이미 이 구조체와 동일하고, PHOTO_DONE_ACK는 그냥 "이걸 항상 보낸다"로 바뀐 것뿐이라서
 * (예전엔 문제 있을 때만 NACK을 보냈는데, 이제 missing_count=0인 경우도 포함해 매번 보냄) */

/* ESP_NOW_MSG_PHOTO_WINDOW_STATUS_ACK(Selective Repeat 실험, 2026-08-05)도 이 구조체를
 * msg_type만 바꿔 그대로 재사용 — 위 DONE_ACK와 동일 이유, range_count가 SR_WINDOW_SIZE
 * 이하로 고정되니 missing_count가 400 상한을 넘을 일이 없음 */

typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  msg_type;
    uint32_t file_id;
    uint16_t range_start;   /* 이 chunk_idx부터 */
    uint16_t range_count;   /* range_count개 범위 안에서 뭘 못 받았는지 물어봄 — 응답은
                                esp_now_photo_chunk_nack_t(msg_type=WINDOW_STATUS_ACK) */
} esp_now_photo_window_status_req_t;

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

/* CAPTURE_STATUS(SUCCESS/FAILED, 최종 결과)에 대한 Cntl의 최소 확인 응답(2026-08-05,
 * Layer 1) — CAM이 esp_now_reliable_request()로 감싸서 "진짜 도달했는지" 확인할 수 있게 함.
 * 페이로드 없음(응답이 왔다는 사실 자체가 의미의 전부) */
typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t msg_type;
} esp_now_capture_status_ack_t;

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
/* ESP_NOW_MSG_PHOTO_LIST_DONE_ACK(2026-08-05, Layer 1)도 이 구조체를 msg_type만 바꿔서
 * 그대로 재사용 — 필드 의미가 이미 동일함(count 등), "항상 보낸다"만 새로운 규칙 */

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
    uint32_t capture_interval_sec;  /* 0 = 자동 촬영 끔 — 배터리/SD 용량 트레이드오프
                                        (project_cntl_cam_power_responsiveness 메모리 참고) */
    uint32_t response_interval_sec; /* 2026-08-08 추가 — on-demand 요청에 CAM이 반응하기까지
                                        허용 가능한 최대 지연(연결성/절전 트레이드오프, 1~10초
                                        권장 범위). CAM 쪽 채널동기 PING 주기 및 Cntl 쪽 노드
                                        무응답 타임아웃 산정에 그대로 쓰임 — 두 값 다 이 하나의
                                        설정에서 파생(사용자가 직접 지정, 0=레거시/기본 500ms
                                        틱 그대로 유지) */
} esp_now_cam_config_t;

/* CAM -> Cntl: CAM_CONFIG_SET 적용 결과 확인(2026-08-08) */
typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t msg_type;
    uint8_t success;
} esp_now_cam_config_ack_t;

/* PHOTO_CHUNK와 동일 크기로 맞춰야 실사용 전송과 같은 조건에서 처리량을 잴 수 있음.
 * seq는 순서 확인용이 아니라 Cntl 로그에서 유실 유무를 눈으로 보기 위한 참고값(벤치마크는
 * 유실을 감지/보정하지 않음 — 순수 최대 처리량 측정이 목적). */
#define ESP_NOW_BENCH_BLAST_DATA_LEN ESP_NOW_PHOTO_CHUNK_DATA_LEN

typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  msg_type;
    uint32_t seq;
    uint8_t  data[ESP_NOW_BENCH_BLAST_DATA_LEN];
} esp_now_bench_blast_t;

/* mode(2026-08-05 추가, Selective Repeat 실험용) — 0: 기존 BENCH_BLAST 순수 채널 처리량 측정
 * (프로토콜 오버헤드 없음, 원래 이 메시지의 유일한 용도였음). 1: 현재 사진전송 방식(블라스트+
 * 끝에 NACK라운드)을 CAM의 최근 촬영 사진으로 duration_sec 동안 반복 전송. 2: 같은 걸
 * Selective Repeat(윈도우+주기적 상태확인)으로 반복 전송. 1/2는 실제 프로토콜 오버헤드까지
 * 포함해서 두 방식의 실제 소요시간/왕복횟수를 비교하는 게 목적 — 순수 채널 속도(mode 0)와는
 * 잴 대상 자체가 다름 */
typedef enum {
    ESP_NOW_BENCH_MODE_RAW_BLAST   = 0,
    ESP_NOW_BENCH_MODE_XFER_CURRENT = 1,
    ESP_NOW_BENCH_MODE_XFER_SR      = 2,
} esp_now_bench_mode_t;

typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  msg_type;
    uint16_t duration_sec;
    uint8_t  mode;  /* esp_now_bench_mode_t */
} esp_now_bench_start_t;
