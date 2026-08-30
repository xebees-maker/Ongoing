#include "esp_now_photo.h"
#include "esp_now_link.h"
#include "esp_now_hub.h"
#include "esp_now_tx.h"
#include "ui_log.h"
#include "device_config.h"

#include <string.h>
#include "esp_now.h"
#include "esp_rom_crc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "esp_now_photo";

/* 모듈 전체 상태를 하나의 뮤텍스로 보호 — ESP-NOW 태스크(recv_cb 경유)와 LVGL 워커
 * 태스크(UI) 양쪽에서 건드리는데, 호출 빈도가 낮아서(초당 몇 번 수준) 필드별로 락을
 * 쪼갤 실익이 없음 */
static SemaphoreHandle_t s_mutex;

/* ────────────────────────────────────────────────────────────
 * 1. 단일 사진 수신(capture_now/fetch_by_id 공용)
 * ──────────────────────────────────────────────────────────── */
/* 부팅 시 한 번만 할당하고 절대 free/realloc 안 함(2026-08-01, recv 버퍼/캐시 슬롯/판넬
 * 디코드 버퍼 모두 동일 원칙 — 반복 사용하는 버퍼는 처음에 한 번만 잡고 계속 재사용).
 * 720KB로는 실기에서 큰 사진(고엔트로피 장면)이 넘쳐서 3001 에러가 남 — 720KB->1024KB로
 * 확장(2026-08-02). 캐시 슬롯이 2->1로 줄면서(아래 참고) recv_buf+캐시슬롯 합이 이제 버퍼
 * 3개가 아니라 2개라, 실측 여유PSRAM(캐시슬롯[1] 할당 직후 161,560B, 구성 3버퍼 기준)으로
 * 역산한 전체 가용치(~2.42MB) 안에서 1024KB씩 잡아도 ~270KB 여유가 남음(과거 6.7MB 기준
 * 계산은 틀렸었으니 이 실측치를 기준으로 삼음) */
#define PHOTO_RECV_BUF_CAP (1024 * 1024)
static uint8_t  *s_recv_buf = NULL;   /* ESP-NOW 태스크만 건드림, 밖으로 포인터가 안 나감 */
static size_t     s_recv_cap = 0;
static uint32_t   s_file_id = 0;
static uint32_t   s_total_size = 0;
static uint16_t   s_total_chunks = 0;
static uint32_t   s_expected_crc = 0;
static uint16_t   s_chunks_received = 0;
static uint8_t    s_photo_cam_mac[6] = { 0 };  /* NACK을 돌려보낼 대상 — 요청 시점에 저장 */

/* 청크 신뢰성 재설계(2026-08-03) — "핸드셰이크처럼 청크마다 응답을 기다리는데 정작 그
 * 응답이 로컬 라디오 ACK일 뿐이라 진짜 확인이 아니었던" 예전 방식을 버리고, 신뢰도 높은
 * 브로드캐스팅에서 쓰는 스트리밍+선택적 재전송(NACK) 방식으로 교체(사용자 설계 지시,
 * esp_now_link.h의 esp_now_photo_chunk_nack_t 주석 참고). 어느 chunk_idx를 받았는지
 * 비트맵으로 추적해뒀다가 DONE 도착 시 빠진 것만 콕 집어 CAM에 재전송 요청 */
#define PHOTO_MAX_CHUNKS ((PHOTO_RECV_BUF_CAP + ESP_NOW_PHOTO_CHUNK_DATA_LEN - 1) / ESP_NOW_PHOTO_CHUNK_DATA_LEN)
static uint8_t    s_chunk_bitmap[(PHOTO_MAX_CHUNKS + 7) / 8];
static int        s_nack_rounds_used = 0;

static inline void chunk_bitmap_clear(void) { memset(s_chunk_bitmap, 0, sizeof(s_chunk_bitmap)); }
static inline void chunk_bitmap_set(uint16_t idx)
{
    if (idx >= PHOTO_MAX_CHUNKS) return;
    s_chunk_bitmap[idx / 8] |= (uint8_t)(1u << (idx % 8));
}
static inline bool chunk_bitmap_test(uint16_t idx)
{
    if (idx >= PHOTO_MAX_CHUNKS) return false;
    return (s_chunk_bitmap[idx / 8] >> (idx % 8)) & 1;
}

static volatile esp_now_photo_state_t s_state = ESP_NOW_PHOTO_STATE_IDLE;
static uint32_t s_ready_file_id = 0;  /* READY 상태일 때 방금 캐시에 들어간 file_id */

/* 압축 JPEG 원본 캐시 — 방금 수신 완료된 사진 1장을 판넬/웹이 재조회 없이 디코드해 쓸
 * 수 있게 담아두는 "완료본 보관소". 슬롯 버퍼는 recv 버퍼와 같은 이유로 부팅 시 고정
 * 할당해두고 매번 memcpy만 함, malloc/free 없음(2026-08-01).
 * 슬롯 수 1개로 축소(2026-08-02, 기존 2개) — 예전엔 "이미 선택했던 사진을 다시 선택하면
 * 재요청 안 함" 용도로 최근 2장을 들고 있었는데, 사용자가 그 설계를 뒤집음: "탭은
 * Select하기 위한 것일 뿐, 실제 action(가져오기)은 select가 바뀔 때만, 그리고 바뀌면
 * 무조건 새로 가져온다 — 이전 사진을 들고 있는 개념 자체가 없다"(ui_main.c의
 * reconcile_selection 참고). 즉 "여러 장을 기억해뒀다 재사용"할 일이 이제 없어서 슬롯은
 * "지금 막 도착한 사진 1장"만 있으면 충분 — 이 슬롯은 재요청 회피용이 아니라 순전히
 * display_photo/웹 다운로드가 읽어가는 데이터 소스 역할만 함. 여기서 아낀 만큼
 * PHOTO_RECV_BUF_CAP을 키우는 데 씀(위 참고) */
#define PHOTO_CACHE_SLOTS 1
#define PHOTO_CACHE_SLOT_CAP PHOTO_RECV_BUF_CAP
typedef struct {
    bool     used;
    uint32_t file_id;
    uint8_t *data;   /* 부팅 시 고정 할당, 절대 free 안 함 */
    size_t   len;
} photo_cache_slot_t;
static photo_cache_slot_t s_cache[PHOTO_CACHE_SLOTS];
static int s_cache_next = 0;  /* 다음에 (필요하면 덮어)쓸 슬롯 */

/* 이미 있으면 그 슬롯에, 없으면 s_cache_next 슬롯에 memcpy로 덮어쓰고 한 칸 전진 —
 * 호출부가 뮤텍스 잡고 불러야 함. 슬롯 버퍼가 고정 크기라 len이 넘치면 버림(실측
 * 크기 대비 여유를 크게 뒀으니 정상 상황에서는 안 일어나야 함) */
static void cache_insert_locked(uint32_t file_id, const uint8_t *data, size_t len)
{
    if (len > PHOTO_CACHE_SLOT_CAP) {
        ESP_LOGE(TAG, "cache_insert: 사진이 캐시 슬롯보다 큼(%u > %u) — 버림",
                 (unsigned)len, (unsigned)PHOTO_CACHE_SLOT_CAP);
        ui_log_add_err(UI_ERR_CACHE_TOO_BIG, "Photo save failed (too big) file_id=%u len=%u", (unsigned)file_id, (unsigned)len);
        return;
    }

    for (int i = 0; i < PHOTO_CACHE_SLOTS; i++) {
        if (s_cache[i].used && s_cache[i].file_id == file_id) {
            memcpy(s_cache[i].data, data, len);
            s_cache[i].len = len;
            ui_log_add("CACHE slot[%d] updated file_id=%u len=%u", i, (unsigned)file_id, (unsigned)len);
            return;
        }
    }
    photo_cache_slot_t *slot = &s_cache[s_cache_next];
    if (!slot->data) {
        ESP_LOGE(TAG, "cache_insert: 슬롯 버퍼 없음(초기 할당 실패?) — 버림");
        ui_log_add_err(UI_ERR_CACHE_NO_BUF, "Photo save failed (out of memory) file_id=%u", (unsigned)file_id);
        s_cache_next = (s_cache_next + 1) % PHOTO_CACHE_SLOTS;
        return;
    }
    ui_log_add("CACHE slot[%d] new (prev file_id=%u) -> file_id=%u len=%u",
               s_cache_next, (unsigned)slot->file_id, (unsigned)file_id, (unsigned)len);
    memcpy(slot->data, data, len);
    slot->used    = true;
    slot->file_id = file_id;
    slot->len     = len;
    s_cache_next = (s_cache_next + 1) % PHOTO_CACHE_SLOTS;
}

/* ────────────────────────────────────────────────────────────
 * 2. 지금촬영 진행 단계
 * ──────────────────────────────────────────────────────────── */
static volatile esp_now_capture_stage_t s_capture_stage = ESP_NOW_CAPTURE_STAGE_NONE;
/* 2026-08-26(사용자 지시) — 어느 mac을 대상으로 진행 중인지 기록(s_photo_cam_mac과 동일
 * 이유). 없으면 여러 기기가 붙어있을 때 캠1 지금촬영 중에 캠2까지 "통신 중"으로 오판해서
 * 불필요하게 안 재우는 버그가 됨(esp_now_photo_is_transacting_with 참고) */
static uint8_t s_capture_cam_mac[6] = { 0 };

/* ────────────────────────────────────────────────────────────
 * 3. 사진 목록
 * ──────────────────────────────────────────────────────────── */
/* 2026-08-21 — 내부(비-PSRAM) DRAM이 httpd_start 실패(5005)를 겪을 만큼 빠듯했던 걸 실기로
 * 확인 — 화면 표시용 목록이라 빠른 접근이 필수가 아니어서 PSRAM으로 옮김(s_recv_buf/캐시
 * 슬롯과 동일 원칙, esp_now_photo_init()에서 할당) */
static esp_now_photo_list_view_item_t *s_list_items = NULL;
static int                        s_list_count = 0;
static volatile esp_now_photo_list_state_t s_list_state = ESP_NOW_PHOTO_LIST_STATE_IDLE;
static uint32_t                   s_sd_total_kb = 0;  /* 최근 목록 응답에 실려온 CAM SD 용량 */
static uint32_t                   s_sd_used_kb  = 0;

/* 2026-08-11 재설계(사용자 지시) — 파일당 1메시지 unreliable 스트리밍 + 사후 SR 복구(2026-08-10)
 * 방식을 버리고, COUNT -> BATCH(reliable) x M -> DONE 순서로 교체(esp_now_link.h의
 * esp_now_photo_list_count_t 주석 참고). 배치 자체가 reliable이라 누락이 있을 수 없으므로
 * 비트맵/재전송 라운드가 통째로 불필요해짐 — "받은 개수 vs COUNT로 안 개수"만 비교하면 됨.
 * 에러 조건 2가지(사용자 지시): (1) DONE이 이미 아는 개수보다 적게 받은 상태에서 옴(조기 종료)
 * -> 즉시 PHOTO_LIST_ERROR로 CAM에 알림. (2) 다 받았는데 DONE이 안 옴 -> 별도 코드 불필요,
 * ui_main.c의 renew_list_tick_fn()이 이미 "마지막 진행 이후 경과시간" 정체감지로 잡아냄
 * (진행이 s_list_received_count로 노출되는데, 다 받은 뒤로 더 안 늘면 그게 곧 정체) */
static uint8_t  s_list_cam_mac[6];
static uint16_t s_list_expected_count = 0;  /* PHOTO_LIST_COUNT로 받은 진짜 총 개수(LIST_MAX
                                                초과분 포함) — 화면엔 min(이 값, LIST_MAX)만 */
static uint16_t s_list_received_count = 0;  /* 지금까지 BATCH로 실제 받은 개수(진행팝업 표시 겸
                                                DONE 조기도착 판정에 씀) */
static bool     s_list_count_received = false;  /* PHOTO_LIST_COUNT 수신 여부(2026-08-11) —
                                                    진행팝업 "명령 전송" 단계 완료 판정용 */

/* 2026-08-30(사용자 설계: "웹기생") — s_list_state는 온디바이스 컨슈머(sync_photo_list_tick)가
 * 매틱 소비(ack)해서 IDLE로 되돌리므로, 웹처럼 늦게 폴링하는 리더는 그 사이 놓칠 수 있음.
 * s_list_items/s_list_count 버퍼 자체는 ack와 무관하게 안 지워지지만, "이 버퍼가 지금 내
 * 요청에 대한 결과가 맞는지" 구분할 수단이 없었음 — 요청마다 세대번호를 매겨서, 그 결과가
 * 어느 세대에 대한 것인지를 ack와 별개로(소비되지 않게) 남겨둠 */
static uint32_t s_list_generation      = 0;  /* esp_now_photo_list_request() 호출마다 증가 */
static uint32_t s_list_done_generation = 0;  /* 마지막으로 완료(성공/실패)된 세대 */
static bool     s_list_done_ok         = false;

void esp_now_photo_init(void)
{
    s_mutex = xSemaphoreCreateMutex();

    ui_log_add("INIT free PSRAM(start)=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    s_list_items = heap_caps_malloc(sizeof(esp_now_photo_list_view_item_t) * ESP_NOW_PHOTO_LIST_MAX,
                                     MALLOC_CAP_SPIRAM);
    if (!s_list_items) {
        ESP_LOGE(TAG, "목록 버퍼 할당 실패 — 목록 표시 불가");
    }

    s_recv_buf = heap_caps_malloc(PHOTO_RECV_BUF_CAP, MALLOC_CAP_SPIRAM);
    if (s_recv_buf) {
        s_recv_cap = PHOTO_RECV_BUF_CAP;
        ui_log_add("INIT recv_buf=OK free PSRAM=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    } else {
        ESP_LOGE(TAG, "수신 버퍼 초기 할당 실패(%u bytes) — 사진 수신 불가", (unsigned)PHOTO_RECV_BUF_CAP);
        ui_log_add_err(UI_ERR_RECV_BUF_ALLOC, "Recv buffer alloc failed - cannot receive photos");
    }

    for (int i = 0; i < PHOTO_CACHE_SLOTS; i++) {
        s_cache[i].data = heap_caps_malloc(PHOTO_CACHE_SLOT_CAP, MALLOC_CAP_SPIRAM);
        if (s_cache[i].data) {
            ui_log_add("INIT cache slot[%d]=OK free PSRAM=%u", i, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        } else {
            ESP_LOGE(TAG, "캐시 슬롯[%d] 초기 할당 실패(%u bytes)", i, (unsigned)PHOTO_CACHE_SLOT_CAP);
            ui_log_add_err(UI_ERR_CACHE_SLOT_ALLOC, "Cache slot[%d] alloc failed - cannot store multiple photos", i);
        }
    }
}

/* ════════════════════════════════════════════════════════════
 * 단일 사진 수신 — 내부 공용 로직
 * ════════════════════════════════════════════════════════════ */
/* 2026-08-26(사용자 지시로 재설계) — 예전엔 통신 시도 전 여기서 "지금 이 노드가 붙어있나"를
 * 확인해서 WAITING이면 그냥 막았음(require_paired). 이제 그 판단 자체가 필요 없어짐 — 5개
 * 액션 함수 전부 esp_now_hub_queue_action()으로 노드별 대기 큐에 넣기만 하고, "지금 보낼 수
 * 있나"는 CASK 사이클(esp_now_hub.c의 WAKE_HELLO 핸들러) 한 곳에서만 판단함(중앙집중,
 * "req...을 여기저기서 확인할 필요가 없어지는" 게 이 재설계의 목적). 노드가 지금 자고 있어도
 * 액션이 유실되지 않고 다음 체크인 때 자동으로 나감.
 * 적응형 반응시간(2026-08-10)의 "마지막 사용자 조작" 시각은 여전히 5개 함수 전부에서
 * esp_now_hub_note_user_action()으로 갱신 — 큐잉 자체는 즉시 성공하지만, 사용자가 방금
 * 뭔가 했다는 사실 자체는 여전히 CNTL의 적응형 유예 판단에 필요함 */

static void start_single_receive(const uint8_t *cam_mac, uint8_t mode, uint32_t param)
{
    esp_now_hub_note_user_action();

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_state == ESP_NOW_PHOTO_STATE_RECEIVING) {
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "이미 수신 중 — 새 요청 무시");
        ui_log_add_err(UI_ERR_REQUEST_BUSY, "Request ignored (already receiving) param=%u", (unsigned)param);
        return;
    }
    s_state = ESP_NOW_PHOTO_STATE_RECEIVING;
    /* 이전 전송의 진행률 카운터가 새 META 도착 전까지 남아있으면 fetch 진행 팝업이
     * 그 낡은 값으로 퍼센트/ETA를 잘못 계산함(2026-08-01 실기에서 확인: 67%부터 시작,
     * 남은시간이 거꾸로 증가) — 새 요청 시작 시점에 바로 지움 */
    s_chunks_received = 0;
    s_total_chunks    = 0;
    chunk_bitmap_clear();
    s_nack_rounds_used = 0;
    /* file_id도 요청 시점에 바로 갱신(현재 유일한 호출부인 fetch_by_id는 mode=BY_ID,
     * param=file_id) — META 도착 전까지 s_file_id가 "이전" 요청 값 그대로 남아있으면
     * 그 사이에 이전 요청의 뒤늦은 CHUNK/DONE이 도착했을 때 file_id가 우연히 일치해서
     * 지금 받는 중인 걸로 잘못 받아들여지는 경우가 있었음(2026-08-01, 실기에서 다른
     * 사진을 선택해도 엉뚱한 사진이 뜨는 문제로 확인) */
    s_file_id = param;
    memcpy(s_photo_cam_mac, cam_mac, sizeof(s_photo_cam_mac));
    xSemaphoreGive(s_mutex);

    /* 2026-08-05 Layer 1 -> 2026-08-26 CASK 큐로 재설계 — 이제 esp_now_tx로 바로 안 나가고
     * 노드별 대기 큐에 들어갔다가 다음 CASK "할일" 단계에서 나감(PHOTO_META를 기다리는 건
     * 그때 esp_now_tx_enqueue가 그대로 함). CAM 쪽 recv_cb가 mode+param 동일 요청은 dedup
     * 처리하므로(esp_now_cam.c 참고) 재시도로 같은 요청이 여러 번 도착해도 안전 */
    esp_now_photo_request_t req = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_PHOTO_REQUEST,
        .mode     = mode,
        .param    = param,
    };
    static const uint8_t s_meta_types[] = { ESP_NOW_MSG_PHOTO_META };
    esp_now_hub_queue_action(cam_mac, &req, sizeof(req), s_meta_types, 1, 500, 3, "사진 요청");
    ESP_LOGI(TAG, "PHOTO_REQUEST(mode=%d, param=%u) 큐잉됨", mode, (unsigned)param);
    ui_log_add("REQUEST mode=%d param=%u queued", mode, (unsigned)param);
}

/* 촬영과 전송은 완전히 분리(2026-08-01) — CAM에 "지금 찍어라"만 보내고 CAPTURE_STATUS로
 * 결과만 확인함. 사진 자체는 여기서 안 받음(단일수신 상태머신을 아예 안 씀) — 실제로
 * 보려면 목록에서 선택해서 fetch_by_id로 따로 받아야 함 */
void esp_now_photo_capture_now(const uint8_t *cam_mac)
{
    esp_now_hub_note_user_action();

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_capture_stage = ESP_NOW_CAPTURE_STAGE_SENT;
    memcpy(s_capture_cam_mac, cam_mac, sizeof(s_capture_cam_mac));
    xSemaphoreGive(s_mutex);

    esp_now_photo_request_t req = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_PHOTO_REQUEST,
        .mode     = PHOTO_REQUEST_MODE_CAPTURE_NOW,
        .param    = 0,
    };
    /* 2026-08-05 Layer 1 -> 2026-08-26 CASK 큐 — CAPTURE_STATUS(RECEIVED)를 기다리는 건
     * 여전히 다음 CASK "할일" 단계에서 esp_now_tx_enqueue가 함. 촬영 자체의 최종 결과
     * (SUCCESS/FAILED)는 이후 별도 비동기 CAPTURE_STATUS로 옴 — 그건 기존처럼
     * recv_cb -> handle_capture_status()가 처리(여기서 안 기다림) */
    static const uint8_t s_capture_status_types[] = { ESP_NOW_MSG_CAPTURE_STATUS };
    esp_now_hub_queue_action(cam_mac, &req, sizeof(req), s_capture_status_types, 1, 500, 3, "지금촬영");
    ESP_LOGI(TAG, "PHOTO_REQUEST(mode=CAPTURE_NOW) 큐잉됨");
}

void esp_now_photo_fetch_by_id(const uint8_t *cam_mac, uint32_t file_id)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    /* 이전 요청이 끝까지(PHOTO_DONE) 못 가고 걸려있었어도, 목록에서 새로 선택한 이상
     * 사용자 의도는 "새로 시작"이므로 강제로 흘려보냄(capture_now에서 겪었던 것과 같은
     * 이유 — RECEIVING에 타임아웃이 없어서 한번 걸리면 이후 요청이 계속 씹힘) */
    s_state = ESP_NOW_PHOTO_STATE_IDLE;
    /* s_file_id를 여기서 바로 새 file_id로 맞춰둠(META 도착 전에 미리) — CAM은 취소
     * 프로토콜이 없어서 사용자가 빠르게 다른 사진을 다시 선택하면 CAM이 이전 요청을
     * 여전히 전송 중일 수 있음(2026-08-02, CAM 쪽엔 세대번호로 스스로 중단하게 고침).
     * 그 사이 이전 file_id의 뒤늦은 청크가 도착했을 때 s_file_id가 아직 옛 값 그대로면
     * handle_chunk()의 file_id 일치 검사를 통과해서 새 수신버퍼에 잘못 섞여 들어갈 수
     * 있음 — 미리 새 file_id로 바꿔두면 옛 청크는 자동으로 불일치 처리되어 버려짐 */
    s_file_id = file_id;
    xSemaphoreGive(s_mutex);
    start_single_receive(cam_mac, PHOTO_REQUEST_MODE_BY_ID, file_id);
}

static void handle_meta(const uint8_t *src_mac, const uint8_t *data, int len)
{
    if (len < (int)sizeof(esp_now_photo_meta_t)) return;
    const esp_now_photo_meta_t *meta = (const esp_now_photo_meta_t *)data;
    ESP_LOGI(TAG, "PHOTO_META 수신: file_id=%u, total_size=%u, chunks=%u",
             (unsigned)meta->file_id, (unsigned)meta->total_size, (unsigned)meta->total_chunks);
    ui_log_add("META file_id=%u size=%u chunks=%u",
               (unsigned)meta->file_id, (unsigned)meta->total_size, (unsigned)meta->total_chunks);

    /* s_state 하나만 뮤텍스로 짧게 감싸고 s_file_id/s_total_chunks 등 나머지 필드는 밖에서
     * 건드리던 게 진짜 경합이었음(2026-08-03, 사용자 지적: "CNTL의 수신단 구현이 이상한 것
     * 같아") — LVGL UI 태스크(esp_now_photo_fetch_by_id, 새 선택 시 s_file_id를 미리 바꿈)와
     * 이 함수(ESP-NOW 콜백 태스크)가 같은 필드들을 서로 다른 락 구간에서 만지고 있어서
     * "확인"과 "그 확인을 근거로 쓰기"가 원자적이지 않았음. 이제 관련 필드 전부를 하나의
     * 락 구간 안에서 같이 바꿈 */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (meta->total_size > s_recv_cap || !s_recv_buf) {
        s_state = ESP_NOW_PHOTO_STATE_ERROR;
        xSemaphoreGive(s_mutex);
        ESP_LOGE(TAG, "사진이 고정 수신 버퍼보다 큼(%u > %u bytes) — 버림",
                 (unsigned)meta->total_size, (unsigned)s_recv_cap);
        ui_log_add_err(UI_ERR_META_TOO_BIG, "Photo receive failed (too big) %u > %u", (unsigned)meta->total_size, (unsigned)s_recv_cap);
        return;
    }
    s_file_id         = meta->file_id;
    s_total_size       = meta->total_size;
    s_total_chunks     = meta->total_chunks;
    s_expected_crc     = meta->crc32;
    s_chunks_received  = 0;
    chunk_bitmap_clear();
    s_nack_rounds_used = 0;
    s_state            = ESP_NOW_PHOTO_STATE_RECEIVING;
    /* 응답(WINDOW_STATUS_ACK/DONE_ACK) 보낼 대상을 실제 발신자 MAC으로 갱신(2026-08-05,
     * Selective Repeat 벤치마크로 발견) — 원래는 start_single_receive()가 Cntl이 먼저
     * PHOTO_REQUEST를 보낼 때 미리 채워뒀는데, XFER_BENCH 모드는 CAM이 요청 없이 먼저
     * 밀어서(META부터 시작) s_photo_cam_mac이 한 번도 안 채워진 채로 남아있었음(초기값
     * 전부 0) — esp_now_send(0-MAC, ...)이 ESP_ERR_ESPNOW_NOT_FOUND로 항상 실패해서 응답이
     * CAM에 전혀 안 갔던 게 원인. META를 실제로 누가 보냈는지가 항상 진짜 정답이므로
     * 여기서 갱신하는 게 요청 경로 여부와 무관하게 맞음 */
    if (src_mac) memcpy(s_photo_cam_mac, src_mac, sizeof(s_photo_cam_mac));
    xSemaphoreGive(s_mutex);

    /* 2026-08-21 — META를 reliable로 전환(esp_now_link.h의 ESP_NOW_MSG_PHOTO_META_ACK 주석
     * 참고) — CAM은 이 ACK을 받을 때까지 청크 전송을 시작 안 하므로, 여기서 반드시 응답해야
     * CAM이 다음 단계로 진행함(용량초과로 위에서 이미 ERROR 처리하고 return한 경우는 응답
     * 안 함 — 어차피 못 받을 전송이라 CAM이 재시도 끝에 스스로 포기하게 두는 게 대역폭
     * 낭비가 적음) */
    esp_now_photo_done_t ack = { .version = ESP_NOW_LINK_VERSION, .msg_type = ESP_NOW_MSG_PHOTO_META_ACK };
    esp_now_send(src_mac, (const uint8_t *)&ack, sizeof(ack));
}

static void handle_chunk(const uint8_t *data, int len)
{
    if (len < (int)sizeof(esp_now_photo_chunk_t)) return;
    const esp_now_photo_chunk_t *chunk = (const esp_now_photo_chunk_t *)data;

    /* 확인(state/file_id 일치)과 실제 쓰기(memcpy)를 같은 락 구간 안에서 — 그 사이에
     * esp_now_photo_fetch_by_id()가 끼어들어 s_file_id/s_state를 새 요청으로 바꿔버리면,
     * 이 청크가 이미 낡은 요청 것인데도 그 사실을 놓치고 새 수신버퍼에 잘못 쓰일 수 있었음 */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_state != ESP_NOW_PHOTO_STATE_RECEIVING || chunk->file_id != s_file_id || !s_recv_buf) {
        xSemaphoreGive(s_mutex);
        return;
    }
    size_t offset = (size_t)chunk->chunk_idx * ESP_NOW_PHOTO_CHUNK_DATA_LEN;
    if (offset + chunk->chunk_len > s_recv_cap) {
        xSemaphoreGive(s_mutex);
        return;  /* 손상된/엉뚱한 청크 — 무시 */
    }
    memcpy(s_recv_buf + offset, chunk->data, chunk->chunk_len);
    /* NACK 재전송 라운드에서 같은 청크가 다시 올 수 있음(예: 재전송분과 뒤늦은 원본이
     * 둘 다 도착) — 비트맵으로 "새로 받은 것"만 카운트해서 중복 집계 방지 */
    if (!chunk_bitmap_test(chunk->chunk_idx)) {
        chunk_bitmap_set(chunk->chunk_idx);
        s_chunks_received++;
    }
    xSemaphoreGive(s_mutex);

    /* 2026-08-10 — 적응형 반응시간의 "마지막 사용자 조작" 시각을 청크마다 갱신. 예전엔
     * require_paired()가 요청 "시작" 시점에 한 번만 갱신해서, 전송이 몇 초 걸리면 그 시간이
     * 고스란히 조용한 시간으로 카운트돼버림 — 전송 도중에 이미 적응형 임계값을 넘겨 SLEEP_NOW가
     * CAM에 큐잉되고, 전송이 끝나 busy가 풀리자마자(사용자가 결과를 볼 틈도 없이) 바로 잠드는
     * 문제로 실사용 중 확인됨(사용자 분석: "통신 완료 후가 아니라 통신을 시작한 입력에 의해
     * 카운터가 진행됨"). 청크가 계속 들어오는 동안은 "활동 중"이 맞으므로 매 청크 갱신 */
    esp_now_hub_note_user_action();
}

/* missing_count==0이면 완료를 뜻하는 PHOTO_DONE_ACK를 항상 1번만 보냄(2026-08-05, Layer 1
 * 재설계) — 예전엔 "문제 있을 때만" NACK을 보내서 CAM이 정상종료인지 무응답인지 구분할
 * 방법이 없었음. 이제 CAM은 esp_now_reliable_request()로 이 응답을 기다리므로, 응답이 안
 * 오면 CAM 쪽에서 알아서 DONE을 재전송함 — Cntl은 3번씩 반복 전송할 필요가 없어짐(레이어가
 * 대신 재시도해줌) */
static void send_done_ack(uint16_t missing_count, const uint16_t *missing_idx)
{
    static esp_now_photo_chunk_nack_t ack;  /* static — 800B+ 구조체를 스택에 안 둠(2026-08-03
                                                스택 오버플로우 사고 이후 원칙) */
    ack.version       = ESP_NOW_LINK_VERSION;
    ack.msg_type      = ESP_NOW_MSG_PHOTO_DONE_ACK;
    ack.file_id       = s_file_id;
    ack.missing_count = missing_count;
    if (missing_count > 0) memcpy(ack.missing_idx, missing_idx, missing_count * sizeof(uint16_t));
    esp_err_t err = esp_now_send(s_photo_cam_mac, (const uint8_t *)&ack, sizeof(ack));
    ESP_LOGI(TAG, "PHOTO_DONE_ACK 전송(누락 %u개) file_id=%u: %s", missing_count, (unsigned)s_file_id, esp_err_to_name(err));
}

static void handle_done(const uint8_t *data, int len)
{
    (void)data; (void)len;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_now_photo_state_t st = s_state;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "PHOTO_DONE 수신: state=%d, chunks=%u/%u", st, s_chunks_received, s_total_chunks);
    ui_log_add("DONE state=%d chunks=%u/%u file_id=%u", st, s_chunks_received, s_total_chunks, (unsigned)s_file_id);
    if (st != ESP_NOW_PHOTO_STATE_RECEIVING) {
        /* RECEIVING이 아니면 이 거래는 CAM 쪽에서 이미 다른 시도로 대체됐거나(세대번호) 이
         * 요청 자체를 우리가 모름(META를 못 받은 상태) — 뭘 요청받았는지조차 몰라서 의미
         * 있는 ACK를 만들 방법이 없으므로 응답 안 함. CAM은 reliable_request 타임아웃으로
         * 알아서 포기함 */
        ui_log_add("DONE ignored (state!=RECEIVING)");
        return;
    }

    if (s_total_chunks == 0 && s_chunks_received == 0) {
        /* META가 아예 안 왔던 경우(해당 file_id가 없음 등) — 에러가 아니라 그냥
         * "보낼 게 없었다"로 조용히 종료. 위와 같은 이유로 응답 생략 */
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_state = ESP_NOW_PHOTO_STATE_IDLE;
        xSemaphoreGive(s_mutex);
        return;
    }

    if (s_chunks_received != s_total_chunks || s_total_size == 0 || !s_recv_buf) {
        /* 청크 누락(2026-08-03 재설계) — 빠진 chunk_idx를 정확히 짚어서 DONE_ACK에 실어
         * 재전송을 요청함(신뢰도 높은 브로드캐스팅 방식, 사용자 설계 지시). 라운드가
         * 남아있으면 RECEIVING 상태를 유지한 채 여기서 그냥 리턴 — CAM이 재전송 후 다시
         * 보내는 DONE이 이 함수를 다시 호출함. 라운드를 다 썼으면 그 사실도 DONE_ACK로
         * 알려주고(2026-08-05, 이전엔 응답 자체를 안 보내서 CAM이 자기 타임아웃까지
         * 기다려야 했음) 진짜 실패 처리 */
        static uint16_t missing_idx[ESP_NOW_PHOTO_NACK_MAX_INDICES];
        uint16_t n = 0;
        for (uint16_t idx = 0; idx < s_total_chunks && n < ESP_NOW_PHOTO_NACK_MAX_INDICES; idx++) {
            if (!chunk_bitmap_test(idx)) missing_idx[n++] = idx;
        }
        send_done_ack(n, missing_idx);

        /* 2026-08-21 off-by-one 수정 — CAM(esp_now_cam.c)의 재전송 루프는 정확히
         * nack_max_rounds번만 DONE을 보내고 그 후엔 조용히 포기함(그 다음 DONE은 절대 안 옴).
         * 그런데 여기 카운터는 증가 *전*에 검사해서, 마지막 DONE을 받고도 "아직 여유 있다"고
         * 오판하고 다음 DONE을 기다렸음 — CAM 쪽 마지막 라운드와 Cntl 쪽 "포기 조건"이 하나
         * 어긋나 있던 것. 실기에서 재현: CAM이 "재전송 라운드 소진"으로 조용히 끝냈는데 Cntl은
         * 계속 대기하다 stall 타임아웃(3006)으로만 빠짐 — 정작 원인은 청크 누락(3002)인데
         * 엉뚱한 무응답 에러로 보였음. 먼저 증가시키고 그 값으로 판단해야 CAM의 마지막
         * 라운드(=이 함수의 마지막 호출)에서 곧바로 실패 처리로 넘어감.
         *
         * 이 라운드 수는 CAM/Cntl 둘 다 "몇 라운드째인가"를 각자 판단 기준으로 쓰므로
         * 반드시 같은 숫자여야 함(둘 다 하드코딩했다가 이 버그로 처음 어긋났던 걸 발견,
         * feedback_cntl_owns_mutually_judged_values 메모리 참고) — 이제 CNTL이 유일한
         * 소유자(device_config_get_nack_max_rounds)이고 CAM_CONFIG_SET으로 CAM에도 같은 값을
         * 전달함(esp_now_hub.c push_cam_config_to 참고) */
        int nack_max_rounds = (int)device_config_get_nack_max_rounds();
        s_nack_rounds_used++;
        if (s_nack_rounds_used < nack_max_rounds) {
            ESP_LOGW(TAG, "청크 누락(%u/%u) — 재전송 요청(%u개, 라운드 %d/%d)",
                     s_chunks_received, s_total_chunks, n, s_nack_rounds_used, nack_max_rounds);
            ui_log_add("DONE_ACK reporting %u missing (round %d/%d) file_id=%u", n, s_nack_rounds_used,
                       nack_max_rounds, (unsigned)s_file_id);
            return;
        }
        ESP_LOGW(TAG, "청크 누락(%u/%u) — NACK 라운드 소진, 사진 버림", s_chunks_received, s_total_chunks);
        ui_log_add_err(UI_ERR_CHUNK_MISSING, "Photo receive failed (chunk missing %u/%u even after resend)", s_chunks_received, s_total_chunks);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_state = ESP_NOW_PHOTO_STATE_ERROR;
        xSemaphoreGive(s_mutex);
        return;
    }

    uint32_t crc = esp_rom_crc32_le(0, s_recv_buf, s_total_size);
    if (crc != s_expected_crc) {
        ESP_LOGW(TAG, "CRC 불일치 — 사진 버림(재조립 실패)");
        ui_log_add_err(UI_ERR_CRC_MISMATCH, "Photo receive failed (CRC mismatch) file_id=%u", (unsigned)s_file_id);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_state = ESP_NOW_PHOTO_STATE_ERROR;
        xSemaphoreGive(s_mutex);
        return;
    }

    /* 검증된 압축 JPEG 원본을 캐시 슬롯(고정 버퍼)으로 memcpy — 픽셀 디코드는 안 함
     * (판넬/뷰어/웹이 필요할 때마다 각자 해상도로 디코드). s_recv_buf는 다음 요청 때
     * 재사용되므로 캐시는 별도 고정 버퍼에 복사해둠(malloc 없음 — cache_insert_locked 참고) */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    cache_insert_locked(s_file_id, s_recv_buf, s_total_size);
    s_ready_file_id = s_file_id;
    s_state = ESP_NOW_PHOTO_STATE_READY;
    xSemaphoreGive(s_mutex);

    send_done_ack(0, NULL);  /* 완료 통보(2026-08-05, Layer 1) — missing_count=0 */
    ESP_LOGI(TAG, "사진 수신 완료: file_id=%u, %u bytes", (unsigned)s_file_id, (unsigned)s_total_size);
    ui_log_add("READY file_id=%u %u bytes", (unsigned)s_file_id, (unsigned)s_total_size);
}

/* Selective Repeat 실험(2026-08-05) — handle_done()과 같은 원칙(모르는 거래엔 무응답, CAM의
 * reliable_request 타임아웃/재시도에 맡김)이지만 훨씬 단순함: range 하나 안에서만 누락을
 * 찾으면 되고(파일 전체를 매번 다시 스캔하지 않음), range_count가 SR_WINDOW_SIZE(CAM 쪽)
 * 이하로 고정되니 missing_count가 400 상한을 넘을 일이 없음 — 새 구조체 대신
 * esp_now_photo_chunk_nack_t를 msg_type만 바꿔 그대로 재사용(DONE_ACK와 동일 이유) */
static void handle_window_status_request(const uint8_t *data, int len)
{
    if (len < (int)sizeof(esp_now_photo_window_status_req_t)) return;
    const esp_now_photo_window_status_req_t *req = (const esp_now_photo_window_status_req_t *)data;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = (s_state == ESP_NOW_PHOTO_STATE_RECEIVING && req->file_id == s_file_id);
    xSemaphoreGive(s_mutex);
    if (!ok) return;

    static esp_now_photo_chunk_nack_t ack;  /* static — 800B+ 구조체 스택 회피(기존 원칙) */
    uint16_t end = req->range_start + req->range_count;
    uint16_t n = 0;
    for (uint16_t idx = req->range_start; idx < end && n < ESP_NOW_PHOTO_NACK_MAX_INDICES; idx++) {
        if (!chunk_bitmap_test(idx)) ack.missing_idx[n++] = idx;
    }
    ack.version       = ESP_NOW_LINK_VERSION;
    ack.msg_type      = ESP_NOW_MSG_PHOTO_WINDOW_STATUS_ACK;
    ack.file_id       = req->file_id;
    ack.missing_count = n;
    esp_err_t err = esp_now_send(s_photo_cam_mac, (const uint8_t *)&ack, sizeof(ack));
    ESP_LOGI(TAG, "WINDOW_STATUS_ACK [%u,%u) 누락 %u개: %s",
             req->range_start, end, n, esp_err_to_name(err));
}

esp_now_photo_state_t esp_now_photo_get_state(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_now_photo_state_t st = s_state;
    xSemaphoreGive(s_mutex);
    return st;
}

/* RECEIVING 중일 때만 의미 있음 — fetch 진행 팝업의 퍼센트/ETA 계산용(락 없이 읽음,
 * ESP-NOW 태스크만 쓰고 여긴 표시용으로만 읽어서 uint16 tearing 정도는 무해) */
void esp_now_photo_get_chunk_progress(uint16_t *received, uint16_t *total)
{
    *received = s_chunks_received;
    *total    = s_total_chunks;
}

uint32_t esp_now_photo_get_ready_file_id(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint32_t id = s_ready_file_id;
    xSemaphoreGive(s_mutex);
    return id;
}

void esp_now_photo_ready_ack(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_state == ESP_NOW_PHOTO_STATE_READY) {
        s_state = ESP_NOW_PHOTO_STATE_IDLE;
    }
    xSemaphoreGive(s_mutex);
}

bool esp_now_photo_cache_get(uint32_t file_id, const uint8_t **out_data, size_t *out_len)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool found = false;
    for (int i = 0; i < PHOTO_CACHE_SLOTS; i++) {
        if (s_cache[i].used && s_cache[i].file_id == file_id) {
            *out_data = s_cache[i].data;
            *out_len  = s_cache[i].len;
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    return found;
}

void esp_now_photo_clear(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_state == ESP_NOW_PHOTO_STATE_ERROR) {
        s_state = ESP_NOW_PHOTO_STATE_IDLE;
    }
    xSemaphoreGive(s_mutex);
}

/* ════════════════════════════════════════════════════════════
 * 지금촬영 진행 단계
 * ════════════════════════════════════════════════════════════ */
static void handle_capture_status(const uint8_t *src_mac, const uint8_t *data, int len)
{
    if (len < (int)sizeof(esp_now_capture_status_t)) return;
    const esp_now_capture_status_t *msg = (const esp_now_capture_status_t *)data;
    ESP_LOGI(TAG, "CAPTURE_STATUS 수신: status=%d", msg->status);

    /* 2026-08-21 — RECEIVED만 recv_cb 컨텍스트의 fire-and-forget 예외, 나머지(INIT_NEEDED/
     * INIT_DONE/CAPTURING/SUCCESS/FAILED)는 전부 CAM이 esp_now_reliable_request()로 감싸서
     * 기다리는 reliable이라 다 ACK 필요(feedback_default_to_reliable_messaging 메모리 참고,
     * 예전엔 SUCCESS/FAILED만 ACK했음) */
    if (src_mac && msg->status != CAM_CAPTURE_STATUS_RECEIVED) {
        esp_now_capture_status_ack_t ack = {
            .version  = ESP_NOW_LINK_VERSION,
            .msg_type = ESP_NOW_MSG_CAPTURE_STATUS_ACK,
        };
        esp_now_send(src_mac, (const uint8_t *)&ack, sizeof(ack));
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    switch (msg->status) {
        case CAM_CAPTURE_STATUS_RECEIVED:
            s_capture_stage = ESP_NOW_CAPTURE_STAGE_ACKED;
            break;
        case CAM_CAPTURE_STATUS_INIT_NEEDED:
            s_capture_stage = ESP_NOW_CAPTURE_STAGE_INIT_NEEDED;
            break;
        case CAM_CAPTURE_STATUS_INIT_DONE:
            s_capture_stage = ESP_NOW_CAPTURE_STAGE_INIT_DONE;
            break;
        case CAM_CAPTURE_STATUS_CAPTURING:
            s_capture_stage = ESP_NOW_CAPTURE_STAGE_CAPTURING;
            break;
        case CAM_CAPTURE_STATUS_SUCCESS:
            s_capture_stage = ESP_NOW_CAPTURE_STAGE_CAPTURED;
            break;
        case CAM_CAPTURE_STATUS_FAILED:
            s_capture_stage = ESP_NOW_CAPTURE_STAGE_CAPTURE_FAILED;
            ui_log_add_err(UI_ERR_CAPTURE_FAILED, "Capture failed (CAM response)");
            break;
        default:
            break;
    }
    xSemaphoreGive(s_mutex);
}

esp_now_capture_stage_t esp_now_photo_get_capture_stage(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_now_capture_stage_t stage = s_capture_stage;
    xSemaphoreGive(s_mutex);
    return stage;
}

void esp_now_photo_capture_stage_clear(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_capture_stage = ESP_NOW_CAPTURE_STAGE_NONE;
    xSemaphoreGive(s_mutex);
}

/* ════════════════════════════════════════════════════════════
 * 사진 목록
 * ════════════════════════════════════════════════════════════ */
static void send_list_request_raw(const uint8_t *cam_mac)
{
    /* 2026-08-05 Layer 1 -> 2026-08-26 CASK 큐 — 다음 CASK "할일" 단계에서 esp_now_tx_enqueue가
     * 최종 신호인 PHOTO_LIST_DONE을 기다림(중간 COUNT/BATCH는 그 사이 recv_cb ->
     * handle_list_count()/handle_list_batch()로 정상 누적됨, 여기선 안 건드림). CAM 쪽
     * s_list_request_pending으로 중복 요청 처리는 걸러짐(esp_now_cam.c 참고). 타임아웃을
     * 3초로 넉넉히 잡음 — 최대 500장 스캔+배치 전송이라 즉답형 메시지보다 오래 걸림 */
    esp_now_photo_list_request_t req = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_PHOTO_LIST_REQUEST,
    };
    static const uint8_t s_list_done_types[] = { ESP_NOW_MSG_PHOTO_LIST_DONE };
    esp_now_hub_queue_action(cam_mac, &req, sizeof(req), s_list_done_types, 1, 3000, 3, "목록 요청");
    ESP_LOGI(TAG, "PHOTO_LIST_REQUEST 큐잉됨");
}

uint32_t esp_now_photo_list_request(const uint8_t *cam_mac)
{
    esp_now_hub_note_user_action();

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_list_state = ESP_NOW_PHOTO_LIST_STATE_REQUESTING;
    s_list_count = 0;
    s_list_expected_count = 0;
    s_list_received_count = 0;
    s_list_count_received = false;
    memcpy(s_list_cam_mac, cam_mac, sizeof(s_list_cam_mac));
    uint32_t generation = ++s_list_generation;
    xSemaphoreGive(s_mutex);

    send_list_request_raw(cam_mac);
    return generation;
}

/* 2026-08-30 — ack(esp_now_photo_list_ack)로 소비되지 않는 결과 확인 수단. generation이
 * esp_now_photo_list_request()가 돌려준 값과 같으면, 그 요청이 성공/실패 여부와 무관하게
 * "이미 결과가 나왔다"는 뜻(성공 여부는 *out_ok) — s_list_items/s_list_count는 그대로 두고
 * 읽으면 됨(esp_now_photo_list_get_items() 그대로 사용 가능) */
bool esp_now_photo_list_get_result(uint32_t generation, bool *out_ok)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool done = (s_list_done_generation == generation);
    if (done) *out_ok = s_list_done_ok;
    xSemaphoreGive(s_mutex);
    return done;
}

bool esp_now_photo_list_count_received(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool v = s_list_count_received;
    xSemaphoreGive(s_mutex);
    return v;
}

/* CAM -> Cntl 1단계: 스트리밍 시작 전에 총 개수를 먼저 알림. src_mac으로 바로 ACK — 요청측
 * (send_list_request_raw)이 기다리는 응답이 아니라 CAM이 자기 esp_now_reliable_request()로
 * 따로 기다리는 응답이라 여기서 즉시 esp_now_send()로 답함(handle_capture_status()와 동일
 * 패턴, Layer 1 재시도는 CAM 쪽이 알아서 함) */
static void handle_list_count(const uint8_t *src_mac, const uint8_t *data, int len)
{
    if (len < (int)sizeof(esp_now_photo_list_count_t)) return;
    const esp_now_photo_list_count_t *msg = (const esp_now_photo_list_count_t *)data;

    /* 2026-08-21 — 예전엔 s_list_state가 REQUESTING이 아니면(예: 이전 목록요청이 이미
     * 완료/에러로 끝난 뒤 도착한 요청의 재전송) 조용히 버리고 COUNT_ACK도 안 보냈음. CAM은
     * blocking으로 성실히 재시도할 뿐인데(지능 없음 원칙), Cntl이 자기 상태만 보고 거부하니
     * CAM 입장에선 영원히 무응답으로 보여서 재진입 루프에 빠졌던 게 실제 원인이었음
     * (project_cntl_cam_photo_list_first_pair_timeout 계열 버그). CAM이 보내는 COUNT는
     * 그 자체로 "지금 목록 전송을 시작한다"는 선언이니, Cntl의 예전 상태가 뭐든 그냥 새
     * 트랜잭션으로 받아들여서 매번 완주시킴 — "CAM은 지능 없이 blocking, Cntl이 상태관리"
     * 원칙에 따라 Cntl 쪽에서 흡수해야 하는 책임 */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_list_state = ESP_NOW_PHOTO_LIST_STATE_REQUESTING;
    memcpy(s_list_cam_mac, src_mac, sizeof(s_list_cam_mac));
    s_list_expected_count = msg->count;
    s_sd_total_kb          = msg->sd_total_kb;
    s_sd_used_kb           = msg->sd_used_kb;
    s_list_received_count  = 0;
    s_list_count           = 0;
    s_list_count_received  = true;
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "PHOTO_LIST_COUNT 수신: %u개, SD %u/%uKB", msg->count, (unsigned)msg->sd_used_kb, (unsigned)msg->sd_total_kb);
    ui_log_add("LIST_COUNT count=%u sd=%u/%uKB", msg->count, (unsigned)msg->sd_used_kb, (unsigned)msg->sd_total_kb);

    esp_now_photo_list_count_t ack = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_PHOTO_LIST_COUNT_ACK,
    };
    esp_err_t err = esp_now_send(src_mac, (const uint8_t *)&ack, sizeof(ack));
    ESP_LOGI(TAG, "PHOTO_LIST_COUNT_ACK 전송: %s", esp_err_to_name(err));
}

/* CAM -> Cntl 2단계: 항목 여러 개를 묶은 배치(reliable) — 배치 자체가 reliable이라 누락이
 * 있을 수 없으므로 index로 바로 씀(재전송/중복 걱정 없음). LIST_MAX를 넘는 index는 화면에
 * 어차피 안 보여줄 항목이라 조용히 버림(기존 "먼저 온 LIST_MAX개만" 동작과 동일, index는
 * 오름차순으로 오므로 앞쪽 LIST_MAX개가 곧 파일이 오래된 순 LIST_MAX개) */
static void handle_list_batch(const uint8_t *src_mac, const uint8_t *data, int len)
{
    if (!s_list_items) return;  /* PSRAM 할당 실패 시(극히 드묾) */
    if (len < 3) return;  /* version+msg_type+entry_count 최소 헤더 */
    const esp_now_photo_list_batch_t *batch = (const esp_now_photo_list_batch_t *)data;
    int entry_count = batch->entry_count;
    if (entry_count < 0 || entry_count > ESP_NOW_PHOTO_LIST_BATCH_MAX) return;
    if (len < (int)(3 + entry_count * sizeof(esp_now_photo_list_item_t))) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_list_state == ESP_NOW_PHOTO_LIST_STATE_REQUESTING) {
        for (int i = 0; i < entry_count; i++) {
            const esp_now_photo_list_item_t *item = &batch->entries[i];
            if (item->index < ESP_NOW_PHOTO_LIST_MAX) {
                s_list_items[item->index].file_id      = item->file_id;
                s_list_items[item->index].kind         = item->kind;
                s_list_items[item->index].capture_time = item->capture_time;
                s_list_items[item->index].file_size    = item->file_size;
            }
            s_list_received_count++;
        }
        uint16_t capped = (s_list_expected_count < ESP_NOW_PHOTO_LIST_MAX) ? s_list_expected_count : ESP_NOW_PHOTO_LIST_MAX;
        s_list_count = (s_list_received_count < capped) ? s_list_received_count : capped;
    }
    xSemaphoreGive(s_mutex);

    esp_now_hub_note_user_action();  /* 2026-08-10 — handle_chunk()와 동일 이유(목록도 최대
                                         500장이라 전송에 시간이 걸릴 수 있음) */

    esp_now_photo_list_batch_t ack = {
        .version     = ESP_NOW_LINK_VERSION,
        .msg_type    = ESP_NOW_MSG_PHOTO_LIST_BATCH_ACK,
        .entry_count = 0,
    };
    esp_err_t err = esp_now_send(src_mac, (const uint8_t *)&ack, 3);  /* entries는 안 봄 — 헤더만 */
    ESP_LOGI(TAG, "PHOTO_LIST_BATCH_ACK 전송(%d개 처리): %s", entry_count, esp_err_to_name(err));
}

/* Cntl -> CAM: 개수 불일치(조기 DONE) 통보 — reliable(사용자 지시 "6번도 reliable로 보낼 수
 * 있어?" -> 예). ERROR_ACK는 esp_now_hub.c의 esp_now_reliable_on_recv()가 범용으로 매칭해줘서
 * 여기선 별도 수신 핸들러가 필요 없음(다른 esp_now_tx_enqueue 호출들과 동일) */
static void send_list_error(const uint8_t *cam_mac)
{
    esp_now_photo_list_done_t msg = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_PHOTO_LIST_ERROR,
    };
    static const uint8_t s_list_error_ack_types[] = { ESP_NOW_MSG_PHOTO_LIST_ERROR_ACK };
    esp_now_tx_enqueue(cam_mac, &msg, sizeof(msg), s_list_error_ack_types, 1, 800, 3, "목록 에러 통보");
    ESP_LOGW(TAG, "PHOTO_LIST_ERROR 큐잉됨(CAM 상태정리 요청)");
}

/* CAM -> Cntl 3단계: 완료 신호. 에러 조건(사용자 지시) — DONE이 이미 아는 개수(COUNT)보다
 * 적게 받은 상태에서 옴(조기 종료) -> 즉시 에러. "다 받았는데 DONE이 안 옴" 조건은 여기서
 * 안 다룸 — ui_main.c의 renew_list_tick_fn()이 진행정체(s_list_received_count가 더 안 늘어남)
 * 로 이미 감지함(2026-08-10에 도입된 정체감지 그대로 재사용) */
static void handle_list_done(const uint8_t *src_mac, const uint8_t *data, int len)
{
    if (len < (int)sizeof(esp_now_photo_list_done_t)) return;

    /* 2026-08-21 — handle_list_count()와 동일한 이유로 상태 무관하게 항상 완주(ACK)시킴.
     * COUNT가 이제 항상 상태를 REQUESTING으로 리셋하므로, 같은 트랜잭션의 DONE이 도착할
     * 때는 정상적으로 REQUESTING일 것 — 혹시 아니어도(극히 드문 순서뒤바뀜) 응답 자체는
     * 항상 보내서 CAM이 무응답으로 오인해 재진입하는 일이 없게 함 */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint16_t received = s_list_received_count;
    uint16_t expected = s_list_expected_count;
    bool ok = (received >= expected);
    if (ok) {
        s_list_state = ESP_NOW_PHOTO_LIST_STATE_READY;
    }
    s_list_done_generation = s_list_generation;  /* ack 여부와 무관하게 남는 결과 표시 */
    s_list_done_ok = ok;
    xSemaphoreGive(s_mutex);

    if (!ok) {
        ESP_LOGW(TAG, "PHOTO_LIST_DONE 조기도착(%u/%u) — 에러 처리", received, expected);
        ui_log_add_err(UI_ERR_LIST_COUNT_MISMATCH, "List receive failed (stopped early %u/%u)", received, expected);
        send_list_error(src_mac);

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        /* 2026-08-11 — IDLE이 아니라 ERROR로 남김. received/expected는 그대로 둬서(초기화 안 함)
         * 진행팝업이 실패 사유(몇 개 중 몇 개)를 표시할 수 있게 함 — get_progress()가 이 값을
         * 그대로 돌려줌. 팝업이 확인하고 esp_now_photo_list_ack()를 부르면 그때 IDLE로 감 */
        s_list_state = ESP_NOW_PHOTO_LIST_STATE_ERROR;
        s_list_count = 0;
        xSemaphoreGive(s_mutex);
        return;
    }

    ESP_LOGI(TAG, "PHOTO_LIST_DONE 수신: %u개", received);
    ui_log_add("LIST_DONE %u items", received);

    esp_now_photo_list_done_t ack = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_PHOTO_LIST_DONE_ACK,
    };
    esp_err_t err = esp_now_send(src_mac, (const uint8_t *)&ack, sizeof(ack));
    ESP_LOGI(TAG, "PHOTO_LIST_DONE_ACK 전송: %s", esp_err_to_name(err));
}

esp_now_photo_list_state_t esp_now_photo_list_get_state(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_now_photo_list_state_t st = s_list_state;
    xSemaphoreGive(s_mutex);
    return st;
}

int esp_now_photo_list_get_items(esp_now_photo_list_view_item_t *out, int max)
{
    if (!s_list_items) return 0;  /* PSRAM 할당 실패 시(극히 드묾) */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    /* CAM은 오래된 것부터(오름차순) 보내는데, 화면엔 최신이 위로 오는 게 자연스러워서
     * 역순으로 훑음. 2026-08-11 — 배치 자체가 reliable이라 빈틈이 있을 수 없어서(항상
     * 촘촘함) 비트맵 확인 없이 바로 씀 */
    int n = 0;
    for (int idx = s_list_count - 1; idx >= 0 && n < max; idx--) {
        out[n++] = s_list_items[idx];
    }
    xSemaphoreGive(s_mutex);
    return n;
}

/* REQUESTING 중일 때만 의미 있음 — 락 없이 읽음(get_chunk_progress()와 동일 원칙, 표시용으로만
 * 쓰여서 uint16 tearing 정도는 무해) */
void esp_now_photo_list_get_progress(uint16_t *received, uint16_t *total)
{
    *received = s_list_received_count;
    *total    = s_list_expected_count;
}

void esp_now_photo_list_ack(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    /* ERROR도 여기서 같이 IDLE로 되돌림(2026-08-11) — capture_stage_clear()와 동일하게
     * "확인 후 IDLE" 패턴, READY와 다른 별도 clear 함수를 새로 안 만듦 */
    if (s_list_state == ESP_NOW_PHOTO_LIST_STATE_READY || s_list_state == ESP_NOW_PHOTO_LIST_STATE_ERROR) {
        s_list_state = ESP_NOW_PHOTO_LIST_STATE_IDLE;
    }
    xSemaphoreGive(s_mutex);
}

void esp_now_photo_list_get_sd_usage(uint32_t *out_total_kb, uint32_t *out_used_kb)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out_total_kb = s_sd_total_kb;
    *out_used_kb  = s_sd_used_kb;
    xSemaphoreGive(s_mutex);
}

void esp_now_photo_delete(const uint8_t *cam_mac, uint32_t file_id)
{
    esp_now_hub_note_user_action();

    esp_now_photo_delete_request_t req = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_PHOTO_DELETE_REQUEST,
        .file_id  = file_id,
    };
    /* 2026-08-05 Layer 1 -> 2026-08-26 CASK 큐 — PHOTO_DELETE_ACK를 기다리는 건 다음 CASK
     * "할일" 단계에서 esp_now_tx_enqueue가 함 */
    static const uint8_t s_delete_ack_types[] = { ESP_NOW_MSG_PHOTO_DELETE_ACK };
    esp_now_hub_queue_action(cam_mac, &req, sizeof(req), s_delete_ack_types, 1, 500, 3, "사진 삭제");
    ESP_LOGI(TAG, "PHOTO_DELETE_REQUEST(id=%u) 큐잉됨", (unsigned)file_id);
}

static void handle_delete_ack(const uint8_t *data, int len)
{
    if (len < (int)sizeof(esp_now_photo_delete_ack_t)) return;
    const esp_now_photo_delete_ack_t *ack = (const esp_now_photo_delete_ack_t *)data;
    ESP_LOGI(TAG, "PHOTO_DELETE_ACK id=%u: %s", (unsigned)ack->file_id, ack->success ? "성공" : "실패");
    if (!ack->success) ui_log_add_err(UI_ERR_DELETE_FAILED, "Photo delete failed file_id=%u", (unsigned)ack->file_id);
}

/* ────────────────────────────────────────────────────────────
 * 4. 전체 삭제
 * ──────────────────────────────────────────────────────────── */
static volatile esp_now_delete_all_state_t s_delete_all_state = ESP_NOW_DELETE_ALL_STATE_NONE;
static bool     s_delete_all_success = false;
static uint16_t s_delete_all_count = 0;
static uint16_t s_delete_all_received_count = 0;  /* 2026-08-21 — CAM이 삭제 시작 전 보고한
                                                       "지울 개수"(완료 대기 예산용) */
/* 2026-08-26(사용자 지시) — s_capture_cam_mac과 동일 이유(여러 기기 붙었을 때 혼동 방지) */
static uint8_t s_delete_all_cam_mac[6] = { 0 };

void esp_now_photo_delete_all(const uint8_t *cam_mac)
{
    esp_now_hub_note_user_action();

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_delete_all_state = ESP_NOW_DELETE_ALL_STATE_REQUESTED;
    memcpy(s_delete_all_cam_mac, cam_mac, sizeof(s_delete_all_cam_mac));
    xSemaphoreGive(s_mutex);

    esp_now_photo_delete_all_request_t req = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_PHOTO_DELETE_ALL_REQUEST,
    };
    /* 2026-08-21 — 예전엔 여기서 최종 DELETE_ALL_ACK(삭제 완료)까지 기다렸는데, 그러면
     * "접수됐는지"와 "다 지웠는지"가 하나의 고정 타임아웃으로 뭉뚱그려져서, 파일 개수가
     * 많을 때 CAM이 정상 작업 중인데도 먼저 포기하는 오탐이 있었음. 이제 이 reliable
     * 요청은 빠른 RECEIVED(접수+개수 통보)만 기다리고, 진짜 완료(ACK)는 그 개수 기준
     * 예산으로 UI(delete_all_tick_fn)가 별도로 폴링함 — 목록 COUNT/BATCH/DONE과 동일하게
     * recv_cb의 일반 dispatch로 비동기 처리(아래 handle_delete_all_ack).
     * 2026-08-26 — RECEIVED를 기다리는 esp_now_tx_enqueue 호출 자체는 다음 CASK "할일"
     * 단계에서 일어남(esp_now_hub_queue_action으로 큐잉) */
    static const uint8_t s_delete_all_received_types[] = { ESP_NOW_MSG_PHOTO_DELETE_ALL_RECEIVED };
    esp_now_hub_queue_action(cam_mac, &req, sizeof(req), s_delete_all_received_types, 1, 800, 3, "전체삭제");
    ESP_LOGI(TAG, "PHOTO_DELETE_ALL_REQUEST 큐잉됨");
}

static void handle_delete_all_received(const uint8_t *data, int len)
{
    if (len < (int)sizeof(esp_now_photo_delete_all_received_t)) return;
    const esp_now_photo_delete_all_received_t *msg = (const esp_now_photo_delete_all_received_t *)data;
    ESP_LOGI(TAG, "PHOTO_DELETE_ALL_RECEIVED 수신: %u개 삭제 예정", (unsigned)msg->count);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_delete_all_state == ESP_NOW_DELETE_ALL_STATE_REQUESTED) {
        s_delete_all_received_count = msg->count;
        s_delete_all_state          = ESP_NOW_DELETE_ALL_STATE_RECEIVED;
    }
    xSemaphoreGive(s_mutex);
}

uint16_t esp_now_photo_delete_all_get_received_count(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint16_t count = s_delete_all_received_count;
    xSemaphoreGive(s_mutex);
    return count;
}

static void handle_delete_all_ack(const uint8_t *data, int len)
{
    if (len < (int)sizeof(esp_now_photo_delete_all_ack_t)) return;
    const esp_now_photo_delete_all_ack_t *ack = (const esp_now_photo_delete_all_ack_t *)data;
    ESP_LOGI(TAG, "PHOTO_DELETE_ALL_ACK 수신: 성공=%d, %u개", ack->success, (unsigned)ack->deleted_count);
    if (!ack->success) ui_log_add_err(UI_ERR_DELETE_ALL_FAILED, "Delete-all failed");

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    /* REQUESTED에서도 받아줌(RECEIVED가 유실됐어도 최종 ACK가 그 자체로 완료 확인이라 정상
     * 처리) — RECEIVED/REQUESTED 둘 다 "아직 최종 완료 아님" 상태라 동일하게 취급 */
    if (s_delete_all_state == ESP_NOW_DELETE_ALL_STATE_REQUESTED ||
        s_delete_all_state == ESP_NOW_DELETE_ALL_STATE_RECEIVED) {
        s_delete_all_success = ack->success;
        s_delete_all_count   = ack->deleted_count;
        s_delete_all_state   = ESP_NOW_DELETE_ALL_STATE_ACKED;
    }
    xSemaphoreGive(s_mutex);
}

esp_now_delete_all_state_t esp_now_photo_delete_all_get_state(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_now_delete_all_state_t st = s_delete_all_state;
    xSemaphoreGive(s_mutex);
    return st;
}

bool esp_now_photo_delete_all_get_success(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = s_delete_all_success;
    xSemaphoreGive(s_mutex);
    return ok;
}

uint16_t esp_now_photo_delete_all_get_count(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint16_t count = s_delete_all_count;
    xSemaphoreGive(s_mutex);
    return count;
}

void esp_now_photo_delete_all_clear(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_delete_all_state          = ESP_NOW_DELETE_ALL_STATE_NONE;
    s_delete_all_received_count = 0;
    xSemaphoreGive(s_mutex);
}

/* 2026-08-26(사용자 지시) — esp_now_photo.h 주석 참고. "통신 중엔 안 재운다" 판단의 근거.
 * 4개 트랜잭션 전부 대상 mac을 저장해두므로(s_photo_cam_mac/s_list_cam_mac/
 * s_capture_cam_mac/s_delete_all_cam_mac) 여러 기기가 동시에 붙어있어도 각자 자기 것만
 * "통신 중"으로 정확히 매칭됨 — 다른 기기 것 때문에 잘못 안 재우는 일이 없음 */
bool esp_now_photo_is_transacting_with(const uint8_t *mac)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool busy = (s_state == ESP_NOW_PHOTO_STATE_RECEIVING && memcmp(s_photo_cam_mac, mac, 6) == 0)
             || (s_list_state == ESP_NOW_PHOTO_LIST_STATE_REQUESTING && memcmp(s_list_cam_mac, mac, 6) == 0)
             || (s_capture_stage != ESP_NOW_CAPTURE_STAGE_NONE
                 && s_capture_stage != ESP_NOW_CAPTURE_STAGE_CAPTURED
                 && s_capture_stage != ESP_NOW_CAPTURE_STAGE_CAPTURE_FAILED
                 && memcmp(s_capture_cam_mac, mac, 6) == 0)
             || ((s_delete_all_state == ESP_NOW_DELETE_ALL_STATE_REQUESTED
                  || s_delete_all_state == ESP_NOW_DELETE_ALL_STATE_RECEIVED)
                 && memcmp(s_delete_all_cam_mac, mac, 6) == 0);
    xSemaphoreGive(s_mutex);
    return busy;
}

/* ════════════════════════════════════════════════════════════
 * 디스패치
 * ════════════════════════════════════════════════════════════ */
void esp_now_photo_on_recv(uint8_t msg_type, const uint8_t *src_mac, const uint8_t *data, int len)
{
    switch (msg_type) {
        case ESP_NOW_MSG_PHOTO_META:      handle_meta(src_mac, data, len);  break;
        case ESP_NOW_MSG_PHOTO_CHUNK:      handle_chunk(data, len);          break;
        case ESP_NOW_MSG_PHOTO_DONE:       handle_done(data, len);           break;
        case ESP_NOW_MSG_CAPTURE_STATUS:   handle_capture_status(src_mac, data, len); break;
        case ESP_NOW_MSG_PHOTO_LIST_COUNT: handle_list_count(src_mac, data, len); break;
        case ESP_NOW_MSG_PHOTO_LIST_BATCH: handle_list_batch(src_mac, data, len); break;
        case ESP_NOW_MSG_PHOTO_LIST_DONE:  handle_list_done(src_mac, data, len);  break;
        case ESP_NOW_MSG_PHOTO_DELETE_ACK: handle_delete_ack(data, len);     break;
        case ESP_NOW_MSG_PHOTO_DELETE_ALL_ACK: handle_delete_all_ack(data, len); break;
        case ESP_NOW_MSG_PHOTO_DELETE_ALL_RECEIVED: handle_delete_all_received(data, len); break;
        case ESP_NOW_MSG_PHOTO_WINDOW_STATUS_REQUEST: handle_window_status_request(data, len); break;
        default: break;
    }
}
