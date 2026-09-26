#pragma once

/**
 * 2026-09-22(설계 — 콘<->브릿지 CAN 프로토콜, "대화하자" 세션의 순수 설계 대화 기반)
 *
 * I2C 브릿지(bridge_link.h)를 CAN으로 완전 대체. 어제(I2C) 설계의 역할분담/철학은 그대로:
 * 브릿지는 "어쩔 수 없는 것"(reliable 재시도/ACK/타임아웃 루프, RF 측정, 수신 MAC pass-through)
 * 만 직접 하고, 나머지(생존판단/auto-connect 판단 등)는 전부 투명하게 콘으로 넘겨서 콘이 판단—
 * 통신량이 늘어나더라도 이 원칙 우선(사용자 지시).
 *
 * CAN(TWAI)은 프레임당 데이터 최대 8바이트(콘=ESP32-S3는 CAN-FD 미지원, HAL v1 확정) —
 * 어차피 못 피하는 한계이므로 자체 프로토콜을 새로 만들지 않고 표준 ISO-TP(ISO 15765-2)의
 * SF/FF/CF/FC 프레이밍을 그대로 채택(사용자 지시 — "표준을 따라야지").
 *
 * CAN ID 4개 — 방향(콘->브릿지 / 브릿지->콘) x 카테고리(DATA/CONTROL). 기기(MAC)별 ID는
 * 없음 — 이 링크는 물리적으로 콘-브릿지 2노드뿐이라 여러 기기 트래픽도 어차피 순차 처리됨
 * (사용자 지적: "캔 관점에서만 보면 노드는 그저 양단에 2개야"). CONTROL이 DATA보다 낮은
 * ID(=높은 우선순위) — DATA 멀티프레임 전송 도중에도 CONTROL이 끼어들 수 있어야 함.
 *
 * MAC은 CAN/ISO-TP 계층에는 의미 없는 페이로드 바이트일 뿐 — 해석은 콘/브릿지 앱 레벨에서만
 * (사용자 지시: "MAC은 캔 관점에서는 페이로드에 들어가는 몰라도 되는 값일 뿐이야"). MAC(6B)만
 * 해도 ISO-TP Single Frame 한계(7B)를 거의 다 채우므로, DATA/CONTROL 둘 다 예외 없이
 * 멀티프레임(FC/CF) 지원이 필요함 — CONTROL을 SF 전용으로 단순화할 수 없음(확인됨).
 *
 * 흐름 원칙: DATA는 한 번에 세션 1개만(다른 기기 것은 대기), CONTROL도 마찬가지로 큐잉.
 * 큐는 드롭 금지 — 꽉 차면 계속 키움(콘의 요약/설정 화면에 큐 깊이를 노출해서 개발 단계에
 * 비정상 적체를 바로 확인할 수 있게 함, 사용자 지시). 버퍼는 PSRAM 우선, 없으면 내부 RAM
 * 폴백(콘/브릿지 공통 원칙, feedback_prefer_psram_for_buffers 메모리 참고).
 */

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ---- CAN ID 배정: CONTROL < DATA (낮은 ID = 높은 버스 우선순위, 사용자 지시) ---- */
#define CAN_BRIDGE_ID_CNTL_TO_BRIDGE_CONTROL   0x100
#define CAN_BRIDGE_ID_BRIDGE_TO_CNTL_CONTROL   0x101
#define CAN_BRIDGE_ID_CNTL_TO_BRIDGE_DATA      0x200
#define CAN_BRIDGE_ID_BRIDGE_TO_CNTL_DATA      0x201

typedef enum {
    CAN_BRIDGE_ROLE_CNTL = 0,   /* 콘: CNTL_TO_BRIDGE로 송신, BRIDGE_TO_CNTL로 수신 */
    CAN_BRIDGE_ROLE_BRIDGE = 1, /* 브릿지: 반대 방향 */
} can_bridge_role_t;

typedef enum {
    CAN_BRIDGE_CAT_CONTROL = 0,
    CAN_BRIDGE_CAT_DATA = 1,
} can_bridge_category_t;

/* ---- ISO-TP PCI(Protocol Control Info) 인코딩 — 표준 그대로, extended addressing 없음 ----
 * byte0 상위니블: 0=Single Frame, 1=First Frame, 2=Consecutive Frame, 3=Flow Control
 * 이 링크는 콘-브릿지 전용 P2P라 BS(BlockSize)=0("남은 거 다 보내"), STmin=0(지연 없음)
 * 고정 — 별도 페이싱 협상 없이 가장 단순하게 구현(사용자 지시 없었던 세부는 최소 구현으로) */
#define ISO_TP_PCI_SF 0x0
#define ISO_TP_PCI_FF 0x1
#define ISO_TP_PCI_CF 0x2
#define ISO_TP_PCI_FC 0x3

#define ISO_TP_SF_MAX_LEN 7      /* Single Frame: byte0 하위니블=길이(1~7) + 데이터 7B */
#define ISO_TP_FF_FIRST_LEN 6    /* First Frame: byte0상위4b+byte1=12비트 전체길이 + 데이터 6B */
#define ISO_TP_CF_MAX_LEN 7      /* Consecutive Frame: byte0 하위니블=순번(0~15 순환) + 데이터 7B */
#define ISO_TP_MAX_MSG_LEN 4095  /* FF 길이필드 12비트 한도 */

#define ISO_TP_FC_STATUS_CTS 0x0      /* Clear To Send — 나머지 CF 계속 보내도 됨 */
#define ISO_TP_FC_STATUS_WAIT 0x1
#define ISO_TP_FC_STATUS_OVERFLOW 0x2

/* ---- 애플리케이션 레벨 메시지 헤더 — ISO-TP가 재조립한 바이트열의 맨 앞에 옴 ----
 * MAC은 페이로드 안의 평범한 값(CAN/ISO-TP는 이 구조체 내용을 전혀 모름). msg_type의
 * 의미는 카테고리(CONTROL/DATA)에 따라 다른 enum을 씀(아래 참고) */
typedef struct __attribute__((packed)) {
    uint8_t msg_type;
    uint8_t mac[6];
    uint8_t flags;      /* CONTROL: 상황별 의미(예: CONNECT_RESULT의 성공/실패, ADVERTISE_NOTIFY의 rssi 등은
                            현재 1바이트만 필요한 것들에 한해 이 필드 재사용) */
} can_bridge_app_header_t;

#define CAN_BRIDGE_APP_HEADER_LEN 8  /* sizeof(can_bridge_app_header_t), packed */

/* CONTROL 카테고리 메시지 타입 — 전부 "어쩔 수 없는 것"만 브릿지가 직접 판단, 나머지는
 * 원시 사실만 실어 나름(투명성 우선 원칙).
 * 2026-09-26(설계 Docs/설계_CAN링크_2026-09-26.md §2) — app msg_type은 두 경로가 한 번호 공간을
 * 공유(RELAY/RELIABLE_SEND/RELIABLE_RESULT는 분류에 따라 어느 경로로도 감) → CTRL 번호를 DATA
 * 번호(1~3)와 안 겹치게 0x11~로 옮김(그때까지 어디서도 안 쓰였음) */
typedef enum {
    CAN_CTRL_PING = 0x11,          /* 콘->브릿지: 생존확인 */
    CAN_CTRL_PONG = 0x12,           /* 브릿지->콘 */
    CAN_CTRL_ADVERTISE_NOTIFY = 0x13, /* 브릿지->콘: mac이 광고중, flags=rssi(부호있는 값이지만 1B로 절단) */
    CAN_CTRL_CONNECT_INSTRUCT = 0x14, /* 콘->브릿지: mac에 접속 시도해 */
    CAN_CTRL_CONNECT_RESULT = 0x15, /* 브릿지->콘: mac 접속 결과, flags=1(성공)/0(실패) */
    CAN_CTRL_LIVENESS_EVENT = 0x16, /* 브릿지->콘: mac에서 원시 수신 이벤트 발생(생존판단은 콘이) */
    CAN_CTRL_SET_CHANNEL = 0x17, /* 콘->브릿지: flags=채널번호(1~13) */
    CAN_CTRL_RESET = 0x18, /* 콘->브릿지: 브릿지 자체 재시작 요청 */
} can_bridge_ctrl_type_t;

/* DATA 카테고리 — 브릿지는 이 안의 CASK(esp_now_link.h) 내용을 해석하지 않는 투명 릴레이.
 * mac=상대 CAM/Sens MAC, payload=CASK 프레임 원본 바이트 그대로.
 *
 * 2026-09-23(1단계 — 광고~연결) — 콘은 ESP-NOW를 전혀 모름(esp_now_init조차 없음, 사용자
 * 지시). 콘의 esp_now_reliable_request() 같은 "요청 1개 -> 응답 1개, 안 오면 재시도"
 * 호출은, 콘 프로세스 안에서 직접 재시도하던 걸 CAN을 통해 브에 그대로 넘기는 IPC 호출로
 * 바뀜 — 재시도 자체는 브가 기존 esp_now_reliable 컴포넌트를 그대로 써서 수행(새로 안
 * 만듦, 사용자 지시: "콘의 소스를 가져다 써야지"). RELIABLE_SEND/RESULT가 그 IPC 왕복. */
typedef enum {
    CAN_DATA_RELAY = 1,            /* 양방향: mac 기준 CASK 프레임 원본을 그대로 실어나름
                                       (단순 fire-and-forget, esp_now_send 자리) */
    CAN_DATA_RELIABLE_SEND = 2,    /* 콘->브: mac에게 이 요청을 reliable로 보내고 결과를 달라
                                       (esp_now_reliable_request 자리) — can_bridge_reliable_send_hdr_t
                                       + 원본 요청 페이로드가 이어붙음 */
    CAN_DATA_RELIABLE_RESULT = 3,  /* 브->콘: 위 요청의 최종 결과 — can_bridge_reliable_result_hdr_t
                                       + (성공 시)응답 페이로드가 이어붙음. mac으로 상관관계 매칭
                                       (한 MAC당 미결 요청 1개 원칙, 예전 I2C 설계와 동일 근거 —
                                       seq 필드 불필요) */
    /* 2026-09-26(설계 §4 SR 수신, 4단계) — 브가 SR의 끝점이 되면 브→콘은 순서 맞춘 사진 스트림만 보냄.
     * 셋 다 Data 경로, mac=캠 MAC. 캠당 사진 1장씩(file_id로 확인). 브는 사진 전체를 들고 있지 않음 */
    CAN_DATA_SR_META  = 4,         /* 브->콘: 사진 시작 — can_bridge_sr_meta_t */
    CAN_DATA_SR_CHUNK = 5,         /* 브->콘: 다음 청크(항상 chunk_idx 순서, 빠짐·중복 없음) — can_bridge_sr_chunk_hdr_t
                                       + 데이터 len바이트 */
    CAN_DATA_SR_DONE  = 6,         /* 브->콘: 사진 끝 — can_bridge_sr_done_t. 콘은 누적 CRC를 META의 crc32와 비교 */
} can_bridge_data_type_t;

/* SR 스트림 페이로드(app_header 바로 뒤). CRC는 캠과 같은 esp_rom_crc32_le — 이전 값을 넘겨 청크마다
 * 누적 가능(crc = esp_rom_crc32_le(crc, data, len), 시작값 0) */
typedef struct __attribute__((packed)) {
    uint32_t file_id;
    uint32_t total_size;
    uint16_t total_chunks;
    uint32_t crc32;
    uint8_t  kind;          /* cam_capture_kind_t('M'/'T') — 콘 저장 파일명 접두사 */
} can_bridge_sr_meta_t;

typedef struct __attribute__((packed)) {
    uint32_t file_id;
    uint16_t chunk_idx;
    uint16_t len;
} can_bridge_sr_chunk_hdr_t;

typedef enum {
    CAN_BRIDGE_SR_DONE_COMPLETE = 0,  /* 모든 청크를 순서대로 보냄 */
    CAN_BRIDGE_SR_DONE_ABORTED  = 1,  /* 캠 전송이 중간에 끊김(캠 무응답·새 META 등) — 콘은 받은 부분을 버림 */
} can_bridge_sr_done_status_t;

typedef struct __attribute__((packed)) {
    uint32_t file_id;
    uint8_t  status;        /* can_bridge_sr_done_status_t */
} can_bridge_sr_done_t;

/* ---- 경로 분류(설계 §2) — 판단은 이 두 함수에만 둠(콘/브가 각자 판단하면 언젠가 어긋남) ----
 * SR(사진 전송) 7종만 DATA, 나머지 전부 CONTROL:
 *   PHOTO_META / META_ACK / PHOTO_CHUNK / WINDOW_STATUS_REQUEST / WINDOW_STATUS_ACK / PHOTO_DONE / DONE_ACK */
can_bridge_category_t can_bridge_path_for_esp_now_msg(uint8_t esp_now_msg_type);

/* CAN app 메시지(app_header + body) 전체 기준: RELAY는 안에 실린 ESP-NOW 프레임의 msg_type(body[1])으로,
 * SR_META/SR_CHUNK/SR_DONE은 DATA, RELIABLE_SEND/RELIABLE_RESULT·CTRL_*은 항상 CONTROL(신뢰 요청·결과·제어) */
can_bridge_category_t can_bridge_path_for_app_msg(const uint8_t *msg, size_t len);

#define CAN_BRIDGE_RELIABLE_MAX_ACCEPT_TYPES 4

/* CAN_DATA_RELIABLE_SEND 페이로드의 헤더 — esp_now_reliable_request()의 파라미터 그대로 실음.
 * app_header 바로 뒤, 원본 요청 페이로드(req) 바로 앞에 옴 */
typedef struct __attribute__((packed)) {
    uint32_t timeout_ms;
    uint8_t  max_attempts;
    uint8_t  accept_reply_types[CAN_BRIDGE_RELIABLE_MAX_ACCEPT_TYPES];
    uint8_t  accept_reply_types_count;
} can_bridge_reliable_send_hdr_t;

/* CAN_DATA_RELIABLE_RESULT 페이로드의 헤더 — app_header 바로 뒤, (ok=1이면)응답 페이로드
 * 바로 앞에 옴 */
typedef struct __attribute__((packed)) {
    uint8_t ok;  /* 1=성공(응답 페이로드 있음), 0=타임아웃/실패(페이로드 없음) */
} can_bridge_reliable_result_hdr_t;

/* ---- 재조립 버퍼 — DATA/CONTROL 각각 독립(동시에 진행 중인 세션이 섞이지 않게) ----
 * PSRAM에 할당(caller가 malloc, 이 구조체는 포인터만 들고 있음 — feedback_prefer_psram_for_buffers) */
typedef struct {
    uint8_t *buf;            /* CAN_BRIDGE_APP_HEADER_LEN + 최대 페이로드, PSRAM */
    size_t   buf_cap;
    size_t   total_len;      /* FF에서 통보받은 전체 길이 */
    size_t   received_len;   /* 지금까지 CF로 채운 길이(SF면 즉시 total_len과 같아짐) */
    uint8_t  next_seq;       /* 다음에 기대하는 CF 순번(0~15 순환) */
    uint8_t  in_progress;    /* 1이면 FF 받고 CF 기다리는 중 */
} can_bridge_reassembly_t;

/* ---- FIFO 큐 — 드롭 금지, 꽉 차면 재할당으로 키움(PSRAM). 완성된 메시지(app_header+payload)
 * 를 통째로 저장 */
typedef struct can_bridge_queue_entry {
    struct can_bridge_queue_entry *next;
    size_t len;               /* app_header + payload 전체 길이 */
    uint8_t data[];           /* PSRAM 할당, 가변 길이 */
} can_bridge_queue_entry_t;

/* 2026-09-25(사용자 지시 — 통신은 전부 이벤트 방식, 큐는 락/방어코드 보강) — push(CAN 수신
 * 태스크)와 pop(소비 태스크)이 서로 다른 태스크(듀얼코어면 서로 다른 코어일 수도)에서 불리므로
 * 목록 조작은 스핀락 크리티컬 섹션으로 보호. 할당/해제/로그/알림은 크리티컬 섹션 밖에서만 함.
 * notify_task가 등록돼 있으면 push(=완성된 메시지 1개 도착) 때만 그 태스크에 태스크 알림을
 * 보냄 — 소비 태스크는 폴링 없이 ulTaskNotifyTake()로 대기 */
typedef struct {
    can_bridge_queue_entry_t *head;
    can_bridge_queue_entry_t *tail;
    uint32_t count;           /* 현재 큐에 들어있는 메시지 수 — 콘 요약/설정 화면 노출용 */
    uint32_t high_water_mark; /* 역대 최대 큐 길이 — 비정상 적체 진단용 */
    portMUX_TYPE lock;        /* head/tail/count/high_water_mark/notify_task 보호 */
    TaskHandle_t notify_task; /* push 시 깨울 소비 태스크(NULL이면 알림 없음) */
    uint8_t inited;           /* can_bridge_queue_init() 호출 여부 — 미초기화 사용 방어 */
} can_bridge_queue_t;

void can_bridge_queue_init(can_bridge_queue_t *q);
/* push 때 태스크 알림을 받을 소비 태스크 등록. 소비 태스크는 "큐 비우기 -> ulTaskNotifyTake()"
 * 순서로 돌아야 함(등록 전에 들어온 메시지도 첫 비우기에서 처리됨) */
void can_bridge_queue_set_notify_task(can_bridge_queue_t *q, TaskHandle_t task);
/* data/len을 PSRAM에 복사해 큐 끝에 추가 — 절대 실패/드롭하지 않음(할당 실패 시 abort,
 * 이 링크에서 메시지 유실은 절대 허용 안 한다는 사용자 지시) */
void can_bridge_queue_push(can_bridge_queue_t *q, const uint8_t *data, size_t len);
/* 큐가 비어있으면 0, 아니면 1을 반환하고 out_data / out_len에 맨 앞 항목을 채움(호출자가
 * can_bridge_queue_pop_free로 해제) — peek 후 pop 분리하지 않고 바로 소유권 이전 */
int can_bridge_queue_pop(can_bridge_queue_t *q, uint8_t **out_data, size_t *out_len);
void can_bridge_queue_pop_free(uint8_t *popped_data);
/* 상태 로그용 — count/high_water_mark를 락 안에서 한 번에 읽음(둘 중 NULL은 건너뜀) */
void can_bridge_queue_get_stats(can_bridge_queue_t *q, uint32_t *out_count, uint32_t *out_high_water_mark);

/* ---- 재조립 상태머신 — CAN RX ISR/태스크에서 프레임 1개씩 넣어주면, 완성된 메시지가
 * 나올 때 완성된 바이트열(app_header+payload)을 큐에 push. fc_frame_out에 FC를 보내야
 * 하면 채워서 반환(호출자가 twai_node_transmit으로 즉시 전송) */
void can_bridge_reassembly_init(can_bridge_reassembly_t *r, uint8_t *psram_buf, size_t buf_cap);
/* 반환값: 1이면 fc_frame_out(8바이트)에 FC 프레임을 채웠으니 즉시 전송해야 함, 0이면 불필요.
 * 메시지가 완성되면 완성 콜백으로 큐에 넣어줌(complete_queue) */
int can_bridge_reassembly_feed(can_bridge_reassembly_t *r, const uint8_t *frame_data, uint8_t frame_len,
                                uint8_t fc_frame_out[8], can_bridge_queue_t *complete_queue);

/* ---- 송신 — msg(app_header+payload, len바이트)를 ISO-TP로 분할해서 twai_node_id로
 * 순차 전송. FC를 기다려야 하면 fc_sem으로 대기(타임아웃 있음). CONTROL/DATA 각각
 * 독립적으로 호출되어야 함(서로 다른 CAN ID, 서로 다른 세션) — 동시에 두 스레드가 같은
 * can_bridge_ctx_t로 이 함수를 부르면 안 됨(세션 1개 원칙, 호출자가 FIFO로 직렬화) */
typedef struct can_bridge_ctx can_bridge_ctx_t;

#define CAN_BRIDGE_DEFAULT_TIMEOUT_MS 500

/* ---- 송신 프레임 풀(PSRAM) ----
 * 2026-09-25(실기 크래시 — 콘 Core0 LoadProhibited, twai_hal_format_frame 안) — ESP-IDF v6
 * twai_node_transmit()은 프레임을 복사하지 않고 포인터만 송신 큐에 넣음(하드웨어가 바쁘면 즉시
 * 반환, 나중에 ISR이 그 포인터를 따라가 전송). 예전엔 스택 지역변수 프레임/데이터를 넘겨서, 반환
 * 뒤 덮인 스택을 ISR이 읽고 죽거나 엉뚱한 바이트를 보냄. 이제 모든 CAN 송신은 이 풀의 슬롯(프레임
 * +데이터 8B)에 복사해서 보내고, on_tx_done(송신 완료, 성공/실패 무관)에서 슬롯을 반납함 —
 * 빈 슬롯은 카운팅 세마포어로 기다림(이벤트 방식). 크기: 드라이버 송신 큐 깊이(8)+전송 중 1개 여유 */
#define CAN_BRIDGE_TX_POOL_SIZE 16

/* twai_node_enable() 전에 1회 호출(can_bridge_node_start_on_core()가 대신 불러줌) */
void can_bridge_tx_pool_init(void);
/* 2026-09-26(사용자 설계 — CAN 관련은 전부 코어 1) — TWAI 인터럽트는 twai_new_node_onchip()을 부른 코어에
 * 잡히므로, core에 고정된 임시 태스크에서 노드 생성 + 송신 풀 초기화 + 콜백 등록 + 시작을 하고 끝날 때까지
 * 기다림. 이후 모든 CAN 송신(can_bridge_tx_frame/can_bridge_send)은 같은 core에 고정된 태스크에서 해야 함
 * — 다른 코어에서 보내면 on_tx_done(송신 권한 반납) 직후~드라이버가 hw_busy를 내리기 전 사이에 끼어들어
 * 프레임이 드라이버 큐에 갇히고 송신이 영구 정지함 */
esp_err_t can_bridge_node_start_on_core(const twai_onchip_node_config_t *node_cfg, const twai_event_callbacks_t *cbs,
                                        int core, twai_node_handle_t *out_node);
/* data(len<=8)를 풀 슬롯에 복사해 id로 송신 큐에 넣음. timeout_ms는 빈 슬롯 대기+드라이버 큐 대기
 * 전체 상한(-1이면 무한). 실패 시 슬롯은 즉시 반납됨 */
esp_err_t can_bridge_tx_frame(twai_node_handle_t node, uint32_t id, const uint8_t *data, uint8_t len, int timeout_ms);
/* 앱의 twai on_tx_done 콜백(ISR)에서 호출 — 끝난 프레임의 슬롯을 반납. 반환값은 더 높은 우선순위
 * 태스크를 깨웠는지(콜백의 반환값으로 그대로 넘기면 됨) */
bool can_bridge_tx_pool_on_done_isr(const twai_tx_done_event_data_t *edata);
/* 2026-09-26 — 앱의 twai on_state_change 콜백(ISR)에서 new_sta==TWAI_ERROR_BUS_OFF일 때 호출. 버스 오프가
 * 되면 드라이버는 하드웨어에 있던 프레임을 on_tx_done 없이 버림 → 그 슬롯과 송신 권한을 여기서 반납(그
 * 프레임은 실패로 끝남). 안 하면 송신 권한이 영영 안 돌아와 이후 모든 송신이 타임아웃(실기: 브 bus_off 뒤
 * TX 고정, Relay fail ERR-TIMEOUT 연속). 반환값은 on_tx_done과 같음 */
bool can_bridge_tx_pool_on_bus_off_isr(void);

can_bridge_ctx_t *can_bridge_ctx_create(twai_node_handle_t node, uint32_t tx_id);
/* CAN RX 콜백에서 PCI==FC인 프레임을 받으면, 그 프레임을 기다리고 있는 송신측 ctx에 대해
 * 호출 — can_bridge_send()가 이걸로 깨어나서 CF 전송을 계속함 */
void can_bridge_ctx_notify_fc(can_bridge_ctx_t *ctx, const uint8_t *frame_data);
/* msg(app_header+payload, len바이트)를 ISO-TP로 분할해 ctx->tx_id로 순차 전송(동기 호출,
 * FC 대기 포함). 2026-09-25부터 ctx당 세션 1개 원칙을 함수 안의 뮤텍스가 보장 — 여러 태스크가
 * 같은 ctx로 동시에 불러도 한 메시지씩 차례로 나감(호출자가 따로 직렬화할 필요 없음) */
esp_err_t can_bridge_send(can_bridge_ctx_t *ctx, const uint8_t *msg, size_t len);
