#include "can_bridge.h"
#include "memdiag.h"
#include "can_bridge_link.h"
#include "photo_rx.h"
#include "ui_log.h"
#include "esp_now_link.h"   /* ESP_NOW_MSG_ADVERTISE — 광고만 기기 종류로 브릿지를 가림 */

#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

/**
 * 2026-09-23 — 콘 쪽 CAN 링크. 브의 can_link.c와 방향만 반대(콘: CNTL_TO_BRIDGE로 송신,
 * BRIDGE_TO_CNTL로 수신).
 * 2026-09-26(설계 Docs/설계_CAN링크_2026-09-26.md §2~3, 2-②) — CONTROL/DATA 두 경로 수신.
 * can_rx가 ID로 나눠 각자의 조립기·완성 큐에 넣고, 경로마다 소비 태스크가 따로 돔(Control 17 /
 * Data 15) — 사진 청크 처리 중에도 WAKE_HELLO/결과가 기다리지 않게.
 */

static const char *TAG = "CAN";
/* 2026-09-27(로그 정리) — 반복될 수 있는 경고는 1초에 1번만(그 사이 건너뛴 횟수를 붙임) */
#define LOG_W_RL(tag, fmt, ...) do { \
    static TickType_t _rl_last; static uint32_t _rl_skip; TickType_t _rl_now = xTaskGetTickCount(); \
    if (_rl_last == 0 || (_rl_now - _rl_last) >= pdMS_TO_TICKS(1000)) { \
        ESP_LOGW(tag, fmt " (+%u skipped)", ##__VA_ARGS__, (unsigned)_rl_skip); _rl_last = _rl_now; _rl_skip = 0; \
    } else { _rl_skip++; } } while (0)

#define CAN_BRIDGE_TX_GPIO GPIO_NUM_15
#define CAN_BRIDGE_RX_GPIO GPIO_NUM_16
#define CAN_BRIDGE_BITRATE 1000000  /* 2026-09-26(사진 전송 CAN 개선 2단계) — 500k -> 1M, 브와 반드시 같은 값 */

static twai_node_handle_t s_node = NULL;
/* 2026-10-18(Docs/설계_보브_보릴_2026-10-18.md §4.2) — 브릿지 여러 대(재브 0, 보브 1). 브릿지마다 송신 문맥·재조립·완성 큐를
 * 따로 둠(브릿지별 ID로 들어오므로 섞이지 않음). 소비 태스크는 경로(Control/Data)마다 하나 — 두 브릿지 큐를 같이 비움 */
#define NB CAN_BRIDGE_MAX_BRIDGES
typedef struct {
    can_bridge_ctx_t *ctx[2];               /* [P_CTRL], [P_DATA] — 콘→브 송신 */
    can_bridge_reassembly_t reasm[2];
    can_bridge_queue_t complete_q[2];
    /* 생존(설계 6단계 — 브릿지마다) */
    volatile int64_t last_pong_us;
    uint32_t boot_id;                        /* 0 = 아직 모름 */
    volatile bool silent;
    volatile bool seen;                      /* 콘이 켜진 뒤 한 번이라도 응답함 — 0번은 늘 있다고 보고, 나머지는 한 번 본 뒤부터 감시 */
    volatile uint8_t channel;                /* 브가 PONG으로 알려 준 지금 ESP-NOW 채널(0 = 모름, 옛 펌웨어) */
    volatile bool channel_set;               /* 브가 이번 부팅에 콘의 채널을 받았음(PONG) — 안 받았으면 광고를 안 넘기므로 꼭 보내야 함 */
} bridge_t;
static bridge_t s_br[NB];
#define P_CTRL 0
#define P_DATA 1
static can_bridge_recv_cb_t s_recv_cb;

/* 노드 → 브릿지 기록 — 그 노드의 프레임이 마지막으로 들어온 브릿지로 보냄(모르면 0번). 노드 수가 적어 작은 표 */
#define NODE_BR_SLOTS 16
typedef struct {
    uint8_t used;
    uint8_t bridge;      /* 마지막으로 프레임이 들어온 브릿지 — 콘→노드 송신 경로 */
    uint8_t want;        /* 기기 종류가 정하는 브릿지(광고 이름으로 앎) — 다른 브릿지로 온 노드 프레임은 무시 */
    uint8_t mac[6];
    int64_t t;
} node_br_t;
static node_br_t s_node_br[NODE_BR_SLOTS];
static portMUX_TYPE s_node_br_lock = portMUX_INITIALIZER_UNLOCKED;

static void note_node_bridge(const uint8_t *mac, uint8_t b)
{
    static const uint8_t zero[6] = { 0 };
    if (memcmp(mac, zero, 6) == 0) return;
    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_node_br_lock);
    int idx = -1, oldest = 0;
    for (int i = 0; i < NODE_BR_SLOTS; i++) {
        if (s_node_br[i].used && memcmp(s_node_br[i].mac, mac, 6) == 0) { idx = i; break; }
        if (!s_node_br[i].used || s_node_br[i].t < s_node_br[oldest].t) oldest = i;
    }
    bool changed = false;
    if (idx < 0) {
        idx = oldest;
        s_node_br[idx].used = 1;
        memcpy(s_node_br[idx].mac, mac, 6);
        s_node_br[idx].bridge = b;
    } else if (s_node_br[idx].bridge != b) {
        s_node_br[idx].bridge = b;
        changed = true;
    }
    s_node_br[idx].t = now;
    taskEXIT_CRITICAL(&s_node_br_lock);
    if (changed) ESP_LOGI(TAG, "Node %02X%02X%02X now via bridge %u", mac[3], mac[4], mac[5], (unsigned)b + 1);
}

/* 2026-10-18(설계_보브_보릴 §4.3) — 기기 종류 → 브릿지. 광고 이름 첫 글자: "B…" 보릴 → 1(보브), 그 밖(캠 "C…", 센스 "S…") → 0(재브) */
static uint8_t kind_bridge_for_name(char first) { return first == 'B' ? 1 : 0; }

static void note_node_want(const uint8_t *mac, uint8_t want)
{
    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_node_br_lock);
    int idx = -1, oldest = 0;
    for (int i = 0; i < NODE_BR_SLOTS; i++) {
        if (s_node_br[i].used && memcmp(s_node_br[i].mac, mac, 6) == 0) { idx = i; break; }
        if (!s_node_br[i].used || s_node_br[i].t < s_node_br[oldest].t) oldest = i;
    }
    if (idx < 0) {
        idx = oldest;
        s_node_br[idx].used = 1;
        memcpy(s_node_br[idx].mac, mac, 6);
        s_node_br[idx].bridge = want;
        s_node_br[idx].t = now;
    }
    s_node_br[idx].want = want;
    taskEXIT_CRITICAL(&s_node_br_lock);
}

/* 모르는 노드(콘이 켜진 뒤 광고를 아직 못 봄)는 0번 — 보릴은 응답을 못 받으면 다시 광고하므로 그때 알게 됨 */
static uint8_t node_want(const uint8_t *mac)
{
    uint8_t w = 0;
    taskENTER_CRITICAL(&s_node_br_lock);
    for (int i = 0; i < NODE_BR_SLOTS; i++) {
        if (s_node_br[i].used && memcmp(s_node_br[i].mac, mac, 6) == 0) { w = s_node_br[i].want; break; }
    }
    taskEXIT_CRITICAL(&s_node_br_lock);
    return w;
}

uint8_t can_bridge_bridge_of(const uint8_t *mac)
{
    uint8_t b = 0;
    taskENTER_CRITICAL(&s_node_br_lock);
    for (int i = 0; i < NODE_BR_SLOTS; i++) {
        if (s_node_br[i].used && memcmp(s_node_br[i].mac, mac, 6) == 0) { b = s_node_br[i].bridge; break; }
    }
    taskEXIT_CRITICAL(&s_node_br_lock);
    return b;
}


/* ---- 미결 reliable 요청 테이블 — MAC당 1개 원칙(esp_now_reliable과 동일 전제), 여러
 * 기기(다른 MAC)는 동시에 대기 가능 ---- */
#define CAN_BRIDGE_MAX_PENDING 8
typedef struct {
    uint8_t  in_use;
    uint8_t  mac[6];
    SemaphoreHandle_t done_sem;
    uint8_t  ok;
    uint8_t *reply_out;
    size_t   reply_out_cap;
    size_t   reply_len;
} pending_request_t;

static pending_request_t s_pending[CAN_BRIDGE_MAX_PENDING];
static SemaphoreHandle_t s_pending_mutex;

typedef struct {
    uint32_t id;
    uint8_t  len;
    uint8_t  data[8];
} raw_frame_t;
static QueueHandle_t s_raw_frame_q;
static void note_bridge_alive(uint8_t b, const uint8_t *body, size_t body_len);  /* 2026-09-26(설계 6단계) — 아래 정의 */
static esp_err_t send_app_msg(uint8_t b, const uint8_t *msg, size_t len);
static volatile uint32_t s_raw_drop = 0;  /* ISR 원시 프레임 큐가 차서 버린 수 */
/* 2026-09-26(버스 에러 조사) — 에러를 트래픽 양으로 나눠 비교하려고 셈 */
static volatile uint32_t s_rx_frames = 0, s_tx_frames = 0, s_bus_errs = 0, s_state_changes = 0;

static TaskHandle_t s_status_task = NULL;  /* 버스 오프 시 on_state_change가 깨움 */

static twai_frame_t s_rx_frame;
static uint8_t       s_rx_buf[8];

static bool on_rx_done(twai_node_handle_t handle, const twai_rx_done_event_data_t *edata, void *ctx)
{
    BaseType_t woken = pdFALSE;
    if (twai_node_receive_from_isr(handle, &s_rx_frame) == ESP_OK) {
        s_rx_frames++;
        raw_frame_t rf;
        rf.id = s_rx_frame.header.id;
        rf.len = (uint8_t)s_rx_frame.buffer_len;
        if (rf.len > 8) rf.len = 8;
        memcpy(rf.data, s_rx_frame.buffer, rf.len);
        /* 2026-09-26 — 큐(16칸)가 차면 프레임이 조용히 버려져 CF 순번 어긋남으로 나타날 수 있음 → 셈 */
        if (xQueueSendFromISR(s_raw_frame_q, &rf, &woken) != pdTRUE) s_raw_drop++;
    }
    return woken == pdTRUE;
}

/* 송신 완료(성공/실패 무관) — 송신 풀 슬롯 반납 */
static bool on_tx_done(twai_node_handle_t handle, const twai_tx_done_event_data_t *edata, void *ctx)
{
    (void)handle;
    (void)ctx;
    s_tx_frames++;
    return can_bridge_tx_pool_on_done_isr(edata);
}

static bool on_error(twai_node_handle_t handle, const twai_error_event_data_t *edata, void *ctx)
{
    /* 0x1(arb_lost, 중재 패배)는 CAN 정상 동작(동시 송신 시 ID 높은 쪽이 양보) — 에러 수에서 제외 */
    if (edata->err_flags.val & ~0x1u) s_bus_errs++;
    return false;
}

static bool on_state_change(twai_node_handle_t handle, const twai_state_change_event_data_t *edata, void *ctx)
{
    /* 2026-09-26 — ISR 안에서 로그 안 찍음(콘솔 출력 대기로 ISR이 붙잡힘). 현재 상태는 can_status_task가 찍음 */
    s_state_changes++;
    BaseType_t woken = pdFALSE;
    if (edata->new_sta == TWAI_ERROR_BUS_OFF) {
        /* 드라이버가 버린 송신 중 프레임 정리(can_bridge_tx_pool_on_bus_off_isr 주석) + 복구를 5초 주기 확인에
         * 맡기지 않고 상태 태스크를 바로 깨워 twai_node_recover() */
        if (can_bridge_tx_pool_on_bus_off_isr()) woken = pdTRUE;
        if (s_status_task) vTaskNotifyGiveFromISR(s_status_task, &woken);
    }
    return woken == pdTRUE;
}

static void can_rx_task(void *arg)
{
    (void)arg;
    raw_frame_t rf;
    for (;;) {
        if (xQueueReceive(s_raw_frame_q, &rf, portMAX_DELAY) != pdTRUE) continue;

        /* 2026-09-26 — ID로 경로 선택. 받는 쪽 FC는 자기 경로의 송신 ID로(현행 원칙).
         * 2026-10-18 — ID로 브릿지도 가림(브릿지별 ID) */
        int b = -1, path = 0;
        for (int i = 0; i < NB && b < 0; i++) {
            if (rf.id == CAN_BRIDGE_ID_BRIDGE_TO_CNTL_CONTROL(i)) { b = i; path = P_CTRL; }
            else if (rf.id == CAN_BRIDGE_ID_BRIDGE_TO_CNTL_DATA(i)) { b = i; path = P_DATA; }
        }
        if (b < 0) continue;  /* 콘→브 방향 ID(자기 송신 에코 없음)나 모르는 ID */
        can_bridge_ctx_t *ctx = s_br[b].ctx[path];
        can_bridge_reassembly_t *reasm = &s_br[b].reasm[path];
        can_bridge_queue_t *complete_q = &s_br[b].complete_q[path];
        uint32_t fc_id = path == P_CTRL ? CAN_BRIDGE_ID_CNTL_TO_BRIDGE_CONTROL(b) : CAN_BRIDGE_ID_CNTL_TO_BRIDGE_DATA(b);

        uint8_t pci = (rf.len >= 1) ? ((rf.data[0] >> 4) & 0x0F) : 0xFF;
        if (pci == ISO_TP_PCI_FC) { can_bridge_ctx_notify_fc(ctx, rf.data); continue; }

        uint8_t fc[8];
        if (can_bridge_reassembly_feed(reasm, rf.data, rf.len, fc, complete_q)) {
            /* 2026-09-25 — 지역 프레임을 드라이버에 넘기면 반환 뒤 ISR이 덮인 스택을 읽음(실기 크래시).
             * PSRAM 송신 풀 슬롯에 복사해서 보냄 */
            esp_err_t fc_err = can_bridge_tx_frame(s_node, fc_id, fc, 8, CAN_BRIDGE_DEFAULT_TIMEOUT_MS);
            if (fc_err != ESP_OK) {
                LOG_W_RL(TAG, "FC send failed (id=0x%x): %s", (unsigned)fc_id, esp_err_to_name(fc_err));
            }
        }
    }
}

static void deliver_relay(const can_bridge_app_header_t *hdr, const uint8_t *payload, size_t payload_len)
{
    if (!s_recv_cb) return;
    /* 예전 I2C 브릿지 설계에서 이미 확인된 사실 그대로: hub.c의 recv_cb는 info->src_addr와
     * info->rx_ctrl->rssi 두 필드만 읽음. 나머지는 0으로 채워도 무방.
     * 2026-09-26 — 소비 태스크가 경로별 2개라 동시에 불릴 수 있음 → static 대신 지역 변수(작음) */
    wifi_pkt_rx_ctrl_t rx_ctrl;
    memset(&rx_ctrl, 0, sizeof(rx_ctrl));
    rx_ctrl.rssi = (int8_t)hdr->flags;

    esp_now_recv_info_t info;
    memset(&info, 0, sizeof(info));
    uint8_t src_addr[6];
    memcpy(src_addr, hdr->mac, 6);
    info.src_addr = src_addr;
    info.rx_ctrl = &rx_ctrl;

    s_recv_cb(&info, payload, (int)payload_len);
}

static void deliver_reliable_result(const can_bridge_app_header_t *hdr, const uint8_t *body, size_t body_len)
{
    if (body_len < sizeof(can_bridge_reliable_result_hdr_t)) return;
    can_bridge_reliable_result_hdr_t result_hdr;
    memcpy(&result_hdr, body, sizeof(result_hdr));
    const uint8_t *reply = body + sizeof(result_hdr);
    size_t reply_len = body_len - sizeof(result_hdr);

    xSemaphoreTake(s_pending_mutex, portMAX_DELAY);
    for (int i = 0; i < CAN_BRIDGE_MAX_PENDING; i++) {
        if (s_pending[i].in_use && memcmp(s_pending[i].mac, hdr->mac, 6) == 0) {
            s_pending[i].ok = result_hdr.ok;
            if (result_hdr.ok && s_pending[i].reply_out && reply_len > 0) {
                size_t copy_len = reply_len < s_pending[i].reply_out_cap ? reply_len : s_pending[i].reply_out_cap;
                memcpy(s_pending[i].reply_out, reply, copy_len);
                s_pending[i].reply_len = copy_len;
            }
            xSemaphoreGive(s_pending[i].done_sem);
            break;
        }
    }
    xSemaphoreGive(s_pending_mutex);
}

/* 2026-09-25(사용자 지시 — 이벤트 방식) — 예전엔 큐가 비면 vTaskDelay(20ms) 후 다시 확인하는
 * 폴링이었음(지시는 이벤트였는데 잘못 구현). 이제 큐를 비운 뒤 태스크 알림을 기다림 — 알림은
 * can_bridge_queue_push()가 완성된 메시지를 넣을 때만 보냄(can_bridge_queue_set_notify_task) */
/* 2026-09-26 — 경로마다 하나씩. 처리 내용은 같음(RELAY/RESULT).
 * 2026-10-18 — arg = 경로(P_CTRL/P_DATA), 그 경로의 브릿지별 큐를 모두 비움(큐마다 이 태스크에 알림) */
static void can_consume_task(void *arg)
{
    int path = (int)(intptr_t)arg;
    for (;;) {
        uint8_t *data; size_t len;
        for (uint8_t b = 0; b < NB; b++)
        while (can_bridge_queue_pop(&s_br[b].complete_q[path], &data, &len)) {
            if (len > CAN_BRIDGE_APP_HEADER_LEN) {
                can_bridge_app_header_t hdr;
                memcpy(&hdr, data, sizeof(hdr));
                const uint8_t *body = data + CAN_BRIDGE_APP_HEADER_LEN;
                size_t body_len = len - CAN_BRIDGE_APP_HEADER_LEN;

                MEMDIAG_BEGIN();  /* 2026-09-28(진단) — 메시지 처리 1건의 내부 RAM 변화 */
                /* 2026-10-18(설계_보브_보릴 §4.3) — 광고는 기기 종류에 맞는 브릿지가 전달한 것만 받음. 노드는 응답(ADVERTISE_ACK)을
                 * 보낸 브릿지에 붙으므로, 다른 브릿지가 들은 광고에 응답하면 실내 센스·캠이 보브에 붙을 수 있음(실기 — 두 브릿지가
                 * 같은 채널일 때 노드가 브릿지 1·2를 오감). 기기 종류는 광고 이름으로(kind_bridge_for_name) — 센스·캠은 0번(재브),
                 * 보릴은 1번(보브). 광고만이 아니라 노드가 보낸 프레임(전달·사진) 전부: 이미 다른 브릿지에 붙은 노드는 응답을 못 받아
                 * 다시 채널을 찾고, 그때는 맞는 브릿지만 응답하므로 거기 붙음(실기 — 규칙 전에 보브에 붙은 노드가 CH11에서 보브와 통신) */
                bool from_node = hdr.msg_type == CAN_DATA_RELAY || hdr.msg_type == CAN_DATA_SR_META ||
                                 hdr.msg_type == CAN_DATA_SR_CHUNK || hdr.msg_type == CAN_DATA_SR_DONE;
                bool adv = hdr.msg_type == CAN_DATA_RELAY && body_len >= sizeof(esp_now_advertise_t) && body[1] == ESP_NOW_MSG_ADVERTISE;
                uint8_t want;
                if (adv) {
                    want = kind_bridge_for_name((char)body[offsetof(esp_now_advertise_t, name)]);
                    note_node_want(hdr.mac, want);
                } else {
                    want = node_want(hdr.mac);
                }
                if (from_node && b != want) {
                    /* 광고는 두 브릿지가 다 들을 수 있어 흔함 → D. 그 밖(이미 다른 브릿지에 붙은 노드의 프레임)은 W */
                    if (adv) {
                        ESP_LOGD(TAG, "Advertise %02X%02X%02X via bridge %u ignored (its kind uses bridge %u)", hdr.mac[3], hdr.mac[4],
                                 hdr.mac[5], (unsigned)b + 1, (unsigned)want + 1);
                    } else {
                        LOG_W_RL(TAG, "Node %02X%02X%02X frame via bridge %u ignored (its kind uses bridge %u)", hdr.mac[3], hdr.mac[4],
                                 hdr.mac[5], (unsigned)b + 1, (unsigned)want + 1);
                    }
                    can_bridge_queue_pop_free(data);
                    continue;
                }
                note_node_bridge(hdr.mac, b);   /* 2026-10-18 — 이 노드는 이 브릿지로(PONG 등 mac 0은 건너뜀) */
                if (hdr.msg_type == CAN_DATA_RELAY) {
                    deliver_relay(&hdr, body, body_len);
                } else if (hdr.msg_type == CAN_DATA_RELIABLE_RESULT) {
                    deliver_reliable_result(&hdr, body, body_len);
                } else if (hdr.msg_type == CAN_CTRL_PONG || hdr.msg_type == CAN_CTRL_BRIDGE_BOOT) {
                    note_bridge_alive(b, body, body_len);
                } else if (hdr.msg_type == CAN_DATA_SR_META || hdr.msg_type == CAN_DATA_SR_CHUNK ||
                           hdr.msg_type == CAN_DATA_SR_DONE) {
                    /* 2026-09-26(설계 §4, 4단계) — 브가 순서를 맞춘 사진 스트림(Data 경로 → data_consume) */
                    photo_rx_on_sr_stream(hdr.msg_type, hdr.mac, body, body_len);
                }
                MEMDIAG_END(TAG, "can msg app=%u espnow=%u mac=%02X%02X%02X", hdr.msg_type,
                            (hdr.msg_type == CAN_DATA_RELAY && body_len >= 2) ? body[1] : 0,
                            hdr.mac[3], hdr.mac[4], hdr.mac[5]);
            }
            can_bridge_queue_pop_free(data);
        }
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}

/* ---- 2026-09-26(설계 §6, 6단계) — 브 장애 처리 ---- */
#define BRIDGE_PING_INTERVAL_MS  2000
#define BRIDGE_SILENT_US         (10LL * 1000 * 1000)
#define BRIDGE_RESET_REPEAT_US   (20LL * 1000 * 1000)
static can_bridge_bridge_reset_cb_t s_bridge_reset_cb = NULL;

void can_bridge_set_bridge_reset_cb(can_bridge_bridge_reset_cb_t cb) { s_bridge_reset_cb = cb; }
bool can_bridge_bridge_is_silent(uint8_t b) { return b < NB && s_br[b].silent; }
bool can_bridge_bridge_watched(uint8_t b) { return b == 0 || (b < NB && s_br[b].seen); }

static volatile bool s_bus_err_now;   /* can_status_task가 갱신 — 버스 오프 또는 송신 ACK 없음 */
bool can_bridge_bus_error(void) { return s_bus_err_now; }

can_bridge_state_t can_bridge_bridge_state(uint8_t b, uint8_t *channel_out)
{
    if (channel_out) *channel_out = 0;
    if (b >= NB) return CAN_BRIDGE_ST_ABSENT;
    const bridge_t *br = &s_br[b];
    if (!br->seen) return b == 0 ? CAN_BRIDGE_ST_NORESP : CAN_BRIDGE_ST_ABSENT;
    if (channel_out) *channel_out = br->channel;
    if (br->silent) return CAN_BRIDGE_ST_NORESP;
    return br->channel_set ? CAN_BRIDGE_ST_OK : CAN_BRIDGE_ST_SETTING;
}

/* 브가 재부팅하면 브에 맡긴 reliable 요청은 영영 결과가 안 옴 — 기다리는 호출자를 실패로 깨움(그 브릿지 몫만) */
static void fail_all_pending(uint8_t b)
{
    xSemaphoreTake(s_pending_mutex, portMAX_DELAY);
    for (int i = 0; i < CAN_BRIDGE_MAX_PENDING; i++) {
        if (s_pending[i].in_use && s_pending[i].done_sem && can_bridge_bridge_of(s_pending[i].mac) == b) {
            s_pending[i].ok = 0;
            xSemaphoreGive(s_pending[i].done_sem);
        }
    }
    xSemaphoreGive(s_pending_mutex);
}

/* PONG·BRIDGE_BOOT 수신(ctrl_consume 태스크) */
static void note_bridge_alive(uint8_t bi, const uint8_t *body, size_t body_len)
{
    if (body_len < sizeof(can_bridge_ctrl_boot_t)) return;
    bridge_t *br = &s_br[bi];
    can_bridge_ctrl_boot_t b;
    memcpy(&b, body, sizeof(b));
    br->last_pong_us = esp_timer_get_time();
    if (body_len >= sizeof(can_bridge_ctrl_pong_t)) {
        br->channel = body[offsetof(can_bridge_ctrl_pong_t, channel)];
        br->channel_set = body[offsetof(can_bridge_ctrl_pong_t, channel_set)] != 0;
    }
    if (!br->seen) {
        br->seen = true;
        ESP_LOGI(TAG, "Bridge %u present (boot ID %08x)", (unsigned)bi + 1, (unsigned)b.boot_id);
    }
    if (br->silent) {
        br->silent = false;
        ESP_LOGI(TAG, "Bridge %u responding again", (unsigned)bi + 1);
    }
    if (br->boot_id != 0 && b.boot_id != br->boot_id) {
        ESP_LOGW(TAG, "Bridge %u reboot detected (boot ID %08x -> %08x) - resetting its node state", (unsigned)bi + 1,
                 (unsigned)br->boot_id, (unsigned)b.boot_id);
        br->boot_id = b.boot_id;
        fail_all_pending(bi);
        if (s_bridge_reset_cb) s_bridge_reset_cb(bi);
        return;
    }
    br->boot_id = b.boot_id;
}

static void send_ctrl_to_bridge(uint8_t bi, uint8_t type, const void *body, size_t len)
{
    uint8_t msg[CAN_BRIDGE_APP_HEADER_LEN + 8];
    if (len > 8) return;
    can_bridge_app_header_t hdr = { .msg_type = type, .flags = 0 };
    memset(hdr.mac, 0, sizeof(hdr.mac));
    memcpy(msg, &hdr, CAN_BRIDGE_APP_HEADER_LEN);
    memcpy(msg + CAN_BRIDGE_APP_HEADER_LEN, body, len);
    send_app_msg(bi, msg, CAN_BRIDGE_APP_HEADER_LEN + len);  /* 실패해도 다음 주기에 다시 — 생존 판단은 PONG 수신으로만 */
}

/* ---- 2026-10-18(Docs/설계_보브_보릴_2026-10-18.md §4.3~4.4) — 브릿지 채널을 콘이 정함 ----
 * 보브(1번, 안테나가 AP·콘과 같은 실외)는 AP가 차지한 대역에서 가장 먼 채널(우선). 재브(0번, 안테나 실내)는 AP 대역과 보브 둘 다에서
 * 최대한 먼 채널(둘까지 거리 중 작은 쪽이 가장 큰 채널 — 사용자 10-18: 다른 방이라도 다른 채널이 나음). 1~13(브의 국가 설정은 부팅
 * 로그로 확인, 채널 변경이 실패하면 브가 로그로 알림). AP가 40MHz면 보조 채널 쪽으로 4칸 더 차지한 것으로 봄. AP가 채널을 바꿔도 바로
 * 따라가지 않음: 새 계획이 지금보다 나은 상태가 10분 계속될 때만 옮김(옮기면 그 브릿지의 노드가 채널을 잃고 다시 찾아야 해서).
 * 브가 알려 준 채널(PONG)이 정한 값과 다르면 SET_CHANNEL을 다시 보냄(브는 재부팅하면 기본 채널로 돌아감) */
#define CH_LO 1
#define CH_HI 13
#define CH_CLEAR 5                                /* 이만큼 떨어지면 안 겹침(20MHz 폭) */
#define CH_MOVE_DELAY_US (10LL * 60 * 1000 * 1000)
static uint8_t s_target_ch[NB];                   /* 0 = 아직 안 정함 */
static int64_t s_overlap_since_us;

static int ch_dist(int c, int lo, int hi) { return c < lo ? lo - c : (c > hi ? c - hi : 0); }

/* AP가 차지한 대역(채널 lo~hi). 모르면 false — 공유기에 붙어 있으면 그 AP, 단독 AP 모드면 콘 자신의 채널 */
static bool ap_band(int *lo, int *hi, uint8_t *primary_out)
{
    wifi_ap_record_t ap;
    uint8_t primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        primary = ap.primary;
        second = ap.second;
    } else {
        wifi_mode_t mode;
        if (esp_wifi_get_mode(&mode) != ESP_OK || mode != WIFI_MODE_AP) return false;
        if (esp_wifi_get_channel(&primary, &second) != ESP_OK) return false;
    }
    if (primary < 1 || primary > 14) return false;
    *lo = *hi = primary;
    if (second == WIFI_SECOND_CHAN_ABOVE) *hi = primary + 4;
    else if (second == WIFI_SECOND_CHAN_BELOW) *lo = primary - 4;
    *primary_out = primary;
    return true;
}

static int imin(int a, int b) { return a < b ? a : b; }
static int iabs(int a) { return a < 0 ? -a : a; }

/* 재브 채널 j의 떨어짐 = AP 대역까지와 보브까지 중 작은 쪽 */
static int jaebu_clear(int j, int bobu, int lo, int hi) { return imin(ch_dist(j, lo, hi), iabs(j - bobu)); }

static void plan_channels(int lo, int hi, uint8_t out[NB])
{
    int bobu = CH_HI, best_d = -1;
    for (int c = CH_HI; c >= CH_LO; c--) {
        int d = ch_dist(c, lo, hi);
        if (d > best_d) { best_d = d; bobu = c; }
    }
    int jaebu = CH_LO, best_j = -1;
    for (int c = CH_LO; c <= CH_HI; c++) {
        int d = jaebu_clear(c, bobu, lo, hi);
        if (d > best_j) { best_j = d; jaebu = c; }
    }
    out[1] = (uint8_t)bobu;
    out[0] = (uint8_t)jaebu;
}

/* 계획의 점수(보브 떨어짐 우선, 다음 재브 떨어짐 — 둘 다 CH_CLEAR 넘으면 같은 값). 지금 계획보다 새 계획이 클 때만 옮길 이유가 있음 */
static int plan_score(const uint8_t ch[NB], int lo, int hi)
{
    return imin(ch_dist(ch[1], lo, hi), CH_CLEAR) * 100 + imin(jaebu_clear(ch[0], ch[1], lo, hi), CH_CLEAR);
}

/* AP를 모를 때(공유기에 아직 못 붙음 등) 기본 배정까지 기다리는 시간 — 브는 채널을 받기 전엔 광고를 안 넘기므로 무한정 기다리면 안 됨 */
#define CH_NO_AP_FALLBACK_US (20LL * 1000 * 1000)

static void update_channels(void)
{
    int lo, hi;
    uint8_t primary;
    int64_t now = esp_timer_get_time();
    if (s_target_ch[0] == 0 && now > CH_NO_AP_FALLBACK_US && !ap_band(&lo, &hi, &primary)) {
        plan_channels(-100, -100, s_target_ch);   /* AP 없음으로 보고 계산(보브·재브를 양 끝으로) — AP를 알게 되면 점수로 다시 판단 */
        ESP_LOGW(TAG, "Channel plan without AP info: Bridge 1 CH%u, Bridge 2 CH%u", (unsigned)s_target_ch[0], (unsigned)s_target_ch[1]);
    }
    if (ap_band(&lo, &hi, &primary)) {
        uint8_t ideal[NB];
        plan_channels(lo, hi, ideal);
        if (s_target_ch[0] == 0) {
            memcpy(s_target_ch, ideal, sizeof(s_target_ch));
            ESP_LOGI(TAG, "Channel plan: AP CH%u (band %d-%d) -> Bridge 1 CH%u, Bridge 2 CH%u", (unsigned)primary, lo, hi,
                     (unsigned)s_target_ch[0], (unsigned)s_target_ch[1]);
        } else if (plan_score(ideal, lo, hi) > plan_score(s_target_ch, lo, hi)) {
            if (s_overlap_since_us == 0) {
                s_overlap_since_us = now;
                ESP_LOGW(TAG, "AP now CH%u (band %d-%d) - Bridge 1 CH%u / Bridge 2 CH%u could be better (CH%u / CH%u), moving in 10 min if it stays",
                         (unsigned)primary, lo, hi, (unsigned)s_target_ch[0], (unsigned)s_target_ch[1], (unsigned)ideal[0], (unsigned)ideal[1]);
            } else if (now - s_overlap_since_us >= CH_MOVE_DELAY_US) {
                memcpy(s_target_ch, ideal, sizeof(s_target_ch));
                s_overlap_since_us = 0;
                ESP_LOGW(TAG, "Channel plan changed: AP CH%u (band %d-%d) -> Bridge 1 CH%u, Bridge 2 CH%u - their nodes will rescan",
                         (unsigned)primary, lo, hi, (unsigned)s_target_ch[0], (unsigned)s_target_ch[1]);
            }
        } else {
            s_overlap_since_us = 0;
        }
    }
    for (uint8_t b = 0; b < NB; b++) {
        bridge_t *br = &s_br[b];
        if (s_target_ch[b] == 0 || !br->seen || br->silent || br->channel == 0) continue;
        if (br->channel == s_target_ch[b] && br->channel_set) continue;
        ESP_LOGI(TAG, "Bridge %u on CH%u%s - setting CH%u", (unsigned)b + 1, (unsigned)br->channel,
                 br->channel_set ? "" : " (not set by Cntl yet)", (unsigned)s_target_ch[b]);
        /* 본문 1바이트(채널)를 붙임 — 브의 소비 태스크는 헤더보다 긴 메시지만 처리함(채널 값은 header.flags에서 읽음) */
        uint8_t msg[CAN_BRIDGE_APP_HEADER_LEN + 1];
        can_bridge_app_header_t hdr = { .msg_type = CAN_CTRL_SET_CHANNEL, .flags = s_target_ch[b] };
        memset(hdr.mac, 0, sizeof(hdr.mac));
        memcpy(msg, &hdr, CAN_BRIDGE_APP_HEADER_LEN);
        msg[CAN_BRIDGE_APP_HEADER_LEN] = s_target_ch[b];
        send_app_msg(b, msg, sizeof(msg));   /* 결과는 다음 PONG의 채널로 확인(같지 않으면 다음 주기에 다시) */
    }
}

/* 2026-10-18 — 모든 브릿지에 PING. 무응답 감시는 0번(재브)은 늘, 나머지는 한 번 응답한 뒤부터(없는 브릿지로 경고하지 않게) */
static void bridge_ping_task(void *arg)
{
    (void)arg;
    uint32_t seq = 0;
    int64_t last_reset_us[NB] = { 0 };
    for (int b = 0; b < NB; b++) s_br[b].last_pong_us = esp_timer_get_time();   /* 부팅 직후 유예 */
    for (;;) {
        seq++;
        for (uint8_t b = 0; b < NB; b++) send_ctrl_to_bridge(b, CAN_CTRL_PING, &seq, sizeof(seq));
        vTaskDelay(pdMS_TO_TICKS(BRIDGE_PING_INTERVAL_MS));
        update_channels();
        int64_t now = esp_timer_get_time();
        for (uint8_t b = 0; b < NB; b++) {
            bridge_t *br = &s_br[b];
            if (!can_bridge_bridge_watched(b) || now - br->last_pong_us <= BRIDGE_SILENT_US) continue;
            if (!br->silent) {
                br->silent = true;
                last_reset_us[b] = 0;
                ESP_LOGE(TAG, "Bridge %u not responding (no PONG for %llds) - sending RESET", (unsigned)b + 1,
                         (long long)((now - br->last_pong_us) / 1000000));
            }
            if (last_reset_us[b] == 0 || now - last_reset_us[b] > BRIDGE_RESET_REPEAT_US) {
                send_ctrl_to_bridge(b, CAN_CTRL_RESET, &seq, sizeof(seq));
                last_reset_us[b] = now;
            }
        }
    }
}

static void can_status_task(void *arg)
{
    (void)arg;
    for (;;) {
        twai_node_status_t status;
        if (twai_node_get_info(s_node, &status, NULL) == ESP_OK) {
            static const char *names[] = {"error_active", "error_warning", "error_passive", "bus_off"};
            uint32_t q_count = 0, q_hwm = 0, cq_count = 0, cq_hwm = 0;
            for (int b = 0; b < NB; b++) {   /* 브릿지 합(최대는 큰 쪽) */
                uint32_t c, h;
                can_bridge_queue_get_stats(&s_br[b].complete_q[P_DATA], &c, &h);
                q_count += c; if (h > q_hwm) q_hwm = h;
                can_bridge_queue_get_stats(&s_br[b].complete_q[P_CTRL], &c, &h);
                cq_count += c; if (h > cq_hwm) cq_hwm = h;
            }
            /* 2026-09-27(로그 정리) — 주기 상태 줄은 D, 버스 상태가 바뀐 때만 W로 */
            static int s_last_state = -1;
            if (s_last_state >= 0 && s_last_state != (int)status.state) {
                ESP_LOGW(TAG, "CAN state %s -> %s (TEC=%u REC=%u)", names[s_last_state], names[status.state],
                         (unsigned)status.tx_error_count, (unsigned)status.rx_error_count);
            }
            s_last_state = (int)status.state;
            ESP_LOGD(TAG, "state=%s TEC=%u REC=%u ctrlQ=%u(max%u) dataQ=%u(max%u) rxDrop=%u RX=%u TX=%u busErr=%u stateChg=%u",
                     names[status.state], (unsigned)status.tx_error_count, (unsigned)status.rx_error_count,
                     (unsigned)cq_count, (unsigned)cq_hwm, (unsigned)q_count, (unsigned)q_hwm, (unsigned)s_raw_drop,
                     (unsigned)s_rx_frames, (unsigned)s_tx_frames, (unsigned)s_bus_errs, (unsigned)s_state_changes);
            if (status.state == TWAI_ERROR_BUS_OFF) twai_node_recover(s_node);
            /* 2026-10-20(링크 확인·재전송) — 브릿지별: 콘이 보낸 쪽 재전송/최종 실패, 받은 쪽 재전송 요청/중복 */
            for (int b = 0; b < NB; b++) {
                uint32_t rs_c = 0, f_c = 0, rs_d = 0, f_d = 0;
                can_bridge_ctx_get_link_stats(s_br[b].ctx[P_CTRL], &rs_c, &f_c);
                can_bridge_ctx_get_link_stats(s_br[b].ctx[P_DATA], &rs_d, &f_d);
                if (rs_c || f_c || rs_d || f_d || s_br[b].reasm[P_CTRL].nack_count || s_br[b].reasm[P_DATA].nack_count ||
                    s_br[b].reasm[P_CTRL].dup_count || s_br[b].reasm[P_DATA].dup_count) {
                    ESP_LOGD(TAG, "LINK bridge%d tx resend ctrl=%u data=%u fail ctrl=%u data=%u | rx nack ctrl=%u data=%u dup ctrl=%u data=%u",
                             b + 1, (unsigned)rs_c, (unsigned)rs_d, (unsigned)f_c, (unsigned)f_d,
                             (unsigned)s_br[b].reasm[P_CTRL].nack_count, (unsigned)s_br[b].reasm[P_DATA].nack_count,
                             (unsigned)s_br[b].reasm[P_CTRL].dup_count, (unsigned)s_br[b].reasm[P_DATA].dup_count);
                }
            }
            /* 2026-10-20(실측 — 교훈) — 이 태스크(코어 1)에서 heap_caps_get_largest_free_block()·heap_caps_get_info()처럼 힙을 훑는 호출을
             * 하지 말 것: 힙 잠금(크리티컬 섹션) 안에서 PSRAM 블록을 전부 훑어 인터럽트를 약 0.5ms 막고, 그 사이 들어오던 CAN 프레임이
             * 수신 FIFO에서 넘쳐 CF가 빠졌음(10분에 16번). 여유·최저치(heap_caps_get_free_size 등)는 카운터만 읽어 괜찮음 */
            /* 2026-10-02(할 일 Q — 09-26 18번 앱 알림) — 송신 실패 자체는 호출부가 이미 정해진 시간 안에 받음
             * (버스 오프 중 즉시, 막힘 중 300ms 중단). 여기선 사용자에게 한 번씩만 알림: 들어갈 때 에러, 풀리면 정보 로그 */
            static bool s_bus_off_warned = false;
            if (status.state == TWAI_ERROR_BUS_OFF && !s_bus_off_warned) {
                ui_log_add_err(UI_ERR_CAN_BUS_OFF, "CAN bus off - recovering, sends fail until then");
                s_bus_off_warned = true;
            } else if (status.state == TWAI_ERROR_ACTIVE && s_bus_off_warned) {
                ui_log_add("CAN bus recovered");
                s_bus_off_warned = false;
            }
            /* 막힘 — 중단 수가 늘면 에러. 중단 없이 송신이 끝난 프레임이 생기면 풀린 것으로 봄 */
            static bool s_stuck_warned = false;
            static uint32_t s_last_aborts = 0, s_last_tx = 0;
            uint32_t aborts = can_bridge_tx_pool_abort_count(), tx = s_tx_frames;
            if (aborts != s_last_aborts) {
                if (!s_stuck_warned) {
                    ui_log_add_err(UI_ERR_CAN_TX_STUCK, "CAN send not acknowledged (bridge off?) - sends fail");
                    s_stuck_warned = true;
                }
            } else if (s_stuck_warned && tx != s_last_tx) {
                ui_log_add("CAN sends acknowledged again");
                s_stuck_warned = false;
            }
            s_last_aborts = aborts;
            s_last_tx = tx;
            s_bus_err_now = s_bus_off_warned || s_stuck_warned;   /* 2026-10-18 — 요약의 브릿지 줄 */
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));  /* 5초 주기, 버스 오프·막힘이면 즉시 */
    }
}

/* 2026-09-26(설계 §2, 2-④) — 콘→브 송신: 경로 분류(can_bridge_path_for_app_msg)로 Control/Data ctx
 * 선택. ISO-TP 세션 1개 원칙은 ctx마다의 송신 뮤텍스(can_bridge_send 내부)가 지킴 — 예전의 별도
 * s_send_mutex는 그 중복이라 제거(두 경로가 서로를 기다리지 않게) */
/* 2026-09-26(사용자 설계 — CAN 관련은 전부 코어 1) — CAN 송신은 TWAI 인터럽트와 같은 코어 1에 고정된 태스크에서만
 * 해야 함(can_bridge_node_start_on_core 주석). 콘은 node_request 작업 태스크(코어 미고정), UI 등 여러 곳에서
 * 보내므로, 코어 1에 고정되지 않은 호출은 경로별 코어 1 송신 태스크(Control 17 / Data 15)에 넘기고 결과를 기다림.
 * 요청은 호출자 스택에 있고 완료 신호까지 호출자가 기다리므로 복사 없음 */
typedef struct {
    can_bridge_ctx_t *ctx;
    const uint8_t *msg;
    size_t len;
    esp_err_t err;
    SemaphoreHandle_t done;
    StaticSemaphore_t done_buf;
} tx_req_t;
static QueueHandle_t s_ctrl_tx_q, s_data_tx_q;

static void can_tx_task(void *arg)
{
    QueueHandle_t q = (QueueHandle_t)arg;
    for (;;) {
        tx_req_t *r;
        if (xQueueReceive(q, &r, portMAX_DELAY) == pdTRUE) {
            r->err = can_bridge_send(r->ctx, r->msg, r->len);
            xSemaphoreGive(r->done);
        }
    }
}

static esp_err_t send_app_msg(uint8_t b, const uint8_t *msg, size_t len)
{
    if (b >= NB) return ESP_ERR_INVALID_ARG;
    bool control = (can_bridge_path_for_app_msg(msg, len) == CAN_BRIDGE_CAT_CONTROL);
    can_bridge_ctx_t *ctx = s_br[b].ctx[control ? P_CTRL : P_DATA];
    if (!ctx) return ESP_ERR_INVALID_STATE;
    if (xTaskGetCoreID(xTaskGetCurrentTaskHandle()) == 1) return can_bridge_send(ctx, msg, len);

    tx_req_t r = { .ctx = ctx, .msg = msg, .len = len, .err = ESP_FAIL };
    r.done = xSemaphoreCreateBinaryStatic(&r.done_buf);
    tx_req_t *rp = &r;
    xQueueSend(control ? s_ctrl_tx_q : s_data_tx_q, &rp, portMAX_DELAY);
    xSemaphoreTake(r.done, portMAX_DELAY);
    vSemaphoreDelete(r.done);
    return r.err;
}

esp_err_t can_bridge_relay_send(const uint8_t *mac, const void *data, size_t len)
{
    uint8_t *msg = (uint8_t *)heap_caps_malloc(CAN_BRIDGE_APP_HEADER_LEN + len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!msg) msg = (uint8_t *)heap_caps_malloc(CAN_BRIDGE_APP_HEADER_LEN + len, MALLOC_CAP_8BIT);
    if (!msg) return ESP_ERR_NO_MEM;

    can_bridge_app_header_t hdr = { .msg_type = CAN_DATA_RELAY, .flags = 0 };
    memcpy(hdr.mac, mac, 6);
    memcpy(msg, &hdr, CAN_BRIDGE_APP_HEADER_LEN);
    memcpy(msg + CAN_BRIDGE_APP_HEADER_LEN, data, len);

    esp_err_t err = send_app_msg(can_bridge_bridge_of(mac), msg, CAN_BRIDGE_APP_HEADER_LEN + len);

    free(msg);
    return err;
}

esp_err_t can_bridge_reliable_request(const uint8_t *peer_mac,
                                       const void *req, size_t req_len,
                                       const uint8_t *accept_reply_types, size_t accept_reply_types_count,
                                       uint32_t timeout_ms, int max_attempts,
                                       void *reply_out, size_t reply_out_cap, size_t *reply_out_len)
{
    if (accept_reply_types_count > CAN_BRIDGE_RELIABLE_MAX_ACCEPT_TYPES) return ESP_ERR_INVALID_ARG;

    /* 미결 테이블에 슬롯 등록 */
    int slot = -1;
    xSemaphoreTake(s_pending_mutex, portMAX_DELAY);
    for (int i = 0; i < CAN_BRIDGE_MAX_PENDING; i++) {
        if (!s_pending[i].in_use) {
            slot = i;
            s_pending[i].in_use = 1;
            memcpy(s_pending[i].mac, peer_mac, 6);
            s_pending[i].ok = 0;
            s_pending[i].reply_out = (uint8_t *)reply_out;
            s_pending[i].reply_out_cap = reply_out_cap;
            s_pending[i].reply_len = 0;
            if (!s_pending[i].done_sem) s_pending[i].done_sem = xSemaphoreCreateBinary();
            xSemaphoreTake(s_pending[i].done_sem, 0);  /* 혹시 남아있던 신호 비움 */
            break;
        }
    }
    xSemaphoreGive(s_pending_mutex);
    if (slot < 0) {
        ESP_LOGE(TAG, "Pending request table full (%d) - mac=%02X%02X%02X%02X%02X%02X",
                 CAN_BRIDGE_MAX_PENDING, peer_mac[0], peer_mac[1], peer_mac[2], peer_mac[3], peer_mac[4], peer_mac[5]);
        return ESP_ERR_NO_MEM;
    }

    size_t msg_len = CAN_BRIDGE_APP_HEADER_LEN + sizeof(can_bridge_reliable_send_hdr_t) + req_len;
    uint8_t *msg = (uint8_t *)heap_caps_malloc(msg_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!msg) msg = (uint8_t *)heap_caps_malloc(msg_len, MALLOC_CAP_8BIT);
    if (!msg) {
        s_pending[slot].in_use = 0;
        return ESP_ERR_NO_MEM;
    }

    can_bridge_app_header_t app_hdr = { .msg_type = CAN_DATA_RELIABLE_SEND, .flags = 0 };
    memcpy(app_hdr.mac, peer_mac, 6);
    can_bridge_reliable_send_hdr_t send_hdr = {
        .timeout_ms = timeout_ms,
        .max_attempts = (uint8_t)max_attempts,
        .accept_reply_types_count = (uint8_t)accept_reply_types_count,
    };
    memcpy(send_hdr.accept_reply_types, accept_reply_types, accept_reply_types_count);

    size_t off = 0;
    memcpy(msg + off, &app_hdr, CAN_BRIDGE_APP_HEADER_LEN); off += CAN_BRIDGE_APP_HEADER_LEN;
    memcpy(msg + off, &send_hdr, sizeof(send_hdr)); off += sizeof(send_hdr);
    memcpy(msg + off, req, req_len); off += req_len;

    esp_err_t send_err = send_app_msg(can_bridge_bridge_of(peer_mac), msg, off);
    free(msg);

    if (send_err != ESP_OK) {
        s_pending[slot].in_use = 0;
        return send_err;
    }

    /* 브가 내부에서 timeout_ms*max_attempts까지 걸릴 수 있으니, CAN 쪽 대기는 여유를 둠 */
    TickType_t wait_ticks = pdMS_TO_TICKS((uint32_t)timeout_ms * (uint32_t)(max_attempts > 0 ? max_attempts : 1) + 2000);
    esp_err_t ret;
    if (xSemaphoreTake(s_pending[slot].done_sem, wait_ticks) == pdTRUE) {
        if (s_pending[slot].ok) {
            if (reply_out_len) *reply_out_len = s_pending[slot].reply_len;
            ret = ESP_OK;
        } else {
            ret = ESP_ERR_TIMEOUT;
        }
    } else {
        ESP_LOGW(TAG, "CAN-level timeout (no Bridge reply) - mac=%02X%02X%02X%02X%02X%02X",
                 peer_mac[0], peer_mac[1], peer_mac[2], peer_mac[3], peer_mac[4], peer_mac[5]);
        ret = ESP_ERR_TIMEOUT;
    }
    s_pending[slot].in_use = 0;
    return ret;
}

void can_bridge_init(can_bridge_recv_cb_t recv_cb)
{
    s_recv_cb = recv_cb;
    s_pending_mutex = xSemaphoreCreateMutex();
    memset(s_pending, 0, sizeof(s_pending));

    static StaticQueue_t s_raw_frame_q_struct;
    /* 2026-09-26 — 16칸은 두 경로가 동시에 흐를 때 모자라서 대량으로 버려짐(실기: 콘 2901개/90초 → CF 순번
         * 어긋남). 256칸으로 늘림.
         * 2026-09-26(사용자 설계 — 가급적 PSRAM) — PSRAM에 둠. TWAI 인터럽트는 IRAM 플래그 없이 할당돼서
         * 플래시 쓰기(캐시 꺼짐) 동안엔 IDF가 아예 막아두므로, 그 사이 PSRAM에 접근할 일이 없음 */
        #define RAW_FRAME_Q_LEN 256
        uint8_t *raw_frame_q_storage = (uint8_t *)heap_caps_malloc(RAW_FRAME_Q_LEN * sizeof(raw_frame_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!raw_frame_q_storage) raw_frame_q_storage = (uint8_t *)heap_caps_malloc(RAW_FRAME_Q_LEN * sizeof(raw_frame_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_raw_frame_q = xQueueCreateStatic(RAW_FRAME_Q_LEN, sizeof(raw_frame_t), raw_frame_q_storage, &s_raw_frame_q_struct);

    s_rx_frame.buffer = s_rx_buf;
    s_rx_frame.buffer_len = sizeof(s_rx_buf);

    #define CAN_BRIDGE_DATA_BUF_LEN 1536
    for (int b = 0; b < NB; b++) {
        for (int path = 0; path < 2; path++) {
            can_bridge_queue_init(&s_br[b].complete_q[path]);
            uint8_t *buf = (uint8_t *)heap_caps_malloc(CAN_BRIDGE_DATA_BUF_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!buf) buf = (uint8_t *)heap_caps_malloc(CAN_BRIDGE_DATA_BUF_LEN, MALLOC_CAP_8BIT);
            can_bridge_reassembly_init(&s_br[b].reasm[path], buf, CAN_BRIDGE_DATA_BUF_LEN);
        }
    }

    /* 2026-09-26(사용자 설계 — CAN 관련은 전부 코어 1) — TWAI 인터럽트를 코어 1에 잡음(노드를 코어 1 태스크에서
     * 생성). 이 함수는 node_hub_init()→app_main(코어 0)에서 불림 */
    twai_onchip_node_config_t node_cfg = {
        .io_cfg = {
            .tx = CAN_BRIDGE_TX_GPIO,
            .rx = CAN_BRIDGE_RX_GPIO,
            .quanta_clk_out = GPIO_NUM_NC,
            .bus_off_indicator = GPIO_NUM_NC,
        },
        .bit_timing.bitrate = CAN_BRIDGE_BITRATE,
        .fail_retry_cnt = -1,  /* 2026-09-26(사용자 지시) — CAN 표준 동작: 성공할 때까지 하드웨어가 재전송(버스 오프는 on_state_change에서 정리+즉시 복구) */
        .tx_queue_depth = 8,
    };
    twai_event_callbacks_t cbs = {
        .on_tx_done = on_tx_done,
        .on_rx_done = on_rx_done,
        .on_error = on_error,
        .on_state_change = on_state_change,
    };
    esp_err_t err = can_bridge_node_start_on_core(&node_cfg, &cbs, 1, &s_node);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CAN node start failed: %s", esp_err_to_name(err));
        return;
    }

    /* 코어 1 송신 태스크(send_app_msg 주석) — ctx보다 먼저 만들어 둠(ctx가 생기는 순간부터 send_app_msg가 쓸 수 있음) */
    s_ctrl_tx_q = xQueueCreate(8, sizeof(tx_req_t *));
    s_data_tx_q = xQueueCreate(8, sizeof(tx_req_t *));
    static StaticTask_t s_ctrl_tx_tcb, s_data_tx_tcb;
    StackType_t *ctrl_tx_stack = (StackType_t *)heap_caps_malloc(3072, MALLOC_CAP_SPIRAM);
    StackType_t *data_tx_stack = (StackType_t *)heap_caps_malloc(3072, MALLOC_CAP_SPIRAM);
    xTaskCreateStaticPinnedToCore(can_tx_task, "ctrl_tx", 3072 / sizeof(StackType_t), s_ctrl_tx_q, 17, ctrl_tx_stack, &s_ctrl_tx_tcb, 1);
    xTaskCreateStaticPinnedToCore(can_tx_task, "data_tx", 3072 / sizeof(StackType_t), s_data_tx_q, 15, data_tx_stack, &s_data_tx_tcb, 1);

    for (int b = 0; b < NB; b++) {
        s_br[b].ctx[P_DATA] = can_bridge_ctx_create(s_node, CAN_BRIDGE_ID_CNTL_TO_BRIDGE_DATA(b));
        s_br[b].ctx[P_CTRL] = can_bridge_ctx_create(s_node, CAN_BRIDGE_ID_CNTL_TO_BRIDGE_CONTROL(b));
    }
    /* 2026-09-26(설계 6단계) — 브 생존 확인(코어 1 — CAN 송신은 코어 1에서) */
    /* 2026-10-01(사용자 지시 — 메모리 검토 N) — 다른 CAN 태스크처럼 스택을 PSRAM에(예전엔 이것만 내부 RAM 3KB).
     * XiP from PSRAM이라 플래시 쓰기 중에도 캐시가 꺼지지 않아 PSRAM 스택 제약 없음(ESP-IDF 외부 RAM 문서) */
    static StaticTask_t s_bridge_ping_tcb;
    StackType_t *bridge_ping_stack = (StackType_t *)heap_caps_malloc(3072, MALLOC_CAP_SPIRAM);
    xTaskCreateStaticPinnedToCore(bridge_ping_task, "bridge_ping", 3072 / sizeof(StackType_t), NULL, 5, bridge_ping_stack, &s_bridge_ping_tcb, 1);

    static StaticTask_t s_can_rx_tcb, s_can_consume_tcb, s_ctrl_consume_tcb, s_can_status_tcb;
    StackType_t *can_rx_stack = (StackType_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    StackType_t *can_consume_stack = (StackType_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    StackType_t *ctrl_consume_stack = (StackType_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    StackType_t *can_status_stack = (StackType_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);  /* 10-02 — 화면 알림(ui_log_add_err) 추가로 3072→4096 */
    /* 2026-09-25(사용자 설계 — 통신/UI 코어 분리) — LVGL은 코어 0, CAN 통신은 코어 1에 고정.
     * 코어 1 안에서는 CAN(수신/소비)이 가장 높아야 함 — 통신 등급 17(project_cntl_task_priority_scheme),
     * power_relay(15)보다 위. can_status는 5초 주기 상태 로그뿐이라 낮은 5 유지 */
    /* 소비 태스크를 먼저 만들고 알림 대상으로 등록한 뒤에 수신 태스크를 띄움 — 첫 push부터
     * 알림이 가도록(등록 전에 들어온 게 있어도 소비 태스크의 첫 비우기에서 처리됨) */
    /* 2026-09-26(설계 §3) — 경로별 소비 태스크: Control 17, Data(SR) 15 */
    TaskHandle_t ctrl_task = xTaskCreateStaticPinnedToCore(can_consume_task, "ctrl_consume", 4096 / sizeof(StackType_t), (void *)(intptr_t)P_CTRL, 17, ctrl_consume_stack, &s_ctrl_consume_tcb, 1);
    TaskHandle_t consume_task = xTaskCreateStaticPinnedToCore(can_consume_task, "data_consume", 4096 / sizeof(StackType_t), (void *)(intptr_t)P_DATA, 15, can_consume_stack, &s_can_consume_tcb, 1);
    for (int b = 0; b < NB; b++) {
        can_bridge_queue_set_notify_task(&s_br[b].complete_q[P_CTRL], ctrl_task);
        can_bridge_queue_set_notify_task(&s_br[b].complete_q[P_DATA], consume_task);
    }
    xTaskCreateStaticPinnedToCore(can_rx_task, "can_rx", 4096 / sizeof(StackType_t), NULL, 17, can_rx_stack, &s_can_rx_tcb, 1);
    s_status_task = xTaskCreateStaticPinnedToCore(can_status_task, "can_status", 4096 / sizeof(StackType_t), NULL, 5, can_status_stack, &s_can_status_tcb, 1);
    can_bridge_tx_pool_set_abort_notify_task(s_status_task);

    ESP_LOGI(TAG, "Cntl CAN link started (TX=%d RX=%d %dbps)", CAN_BRIDGE_TX_GPIO, CAN_BRIDGE_RX_GPIO, CAN_BRIDGE_BITRATE);
}
