#include "esp_now_tx.h"
#include "esp_now_reliable.h"
#include "esp_now_hub.h"
#include "ui_log.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static const char *TAG = "esp_now_tx";

/* 2026-08-10 도입 -> 2026-08-26 삭제(사용자 지시) — 원래는 "CAM이 자고 있을 때 도착한 명령도
 * CAM의 다음 자연스러운 웨이크까지는 재시도해서 언젠가 닿게 하자"는 취지로, 응답성 설정
 * 기준으로 재시도 횟수를 시간 단위로 부풀렸었음(effective_max_attempts, 최대 30초+3초
 * 마진까지 — 즉 메시지 하나가 최대 33초까지 걸릴 수 있었음). 그런데 CASK 재설계 이후
 * esp_now_tx_enqueue()를 부르는 모든 경우(CONFIG/할일/SLEEP_NOW/PAIR_REQUEST 등)가 전부
 * 캠이 방금 먼저 연락해왔을 때(WAKE_HELLO/ADVERTISE)의 응답으로만 나가서, 부르는 그 순간
 * 캠이 깨어있다는 게 이미 보장됨 — "자고 있을지 모르니 시간을 두고 재시도"할 이유 자체가
 * 없어짐. 이 시간 기반 부풀리기가 오히려 SLEEP_NOW 하나가 못 가면 캠이 CASK_SILENCE_TIMEOUT_MS
 * 없이 무한정 기다리게 되는 버그의 실제 원인 중 하나로 드러남(실기에서 확인) — 이제 호출부가
 * 넘긴 고정 횟수를 그대로 씀(count 기반) */

/* 2026-09-10(재설계 — 사용자 설계, TX_QUEUE_FULL 근본원인 조사 후) — 예전엔 "큐 하나 +
 * 소비 태스크 하나"였음. 문제는 PAIR_REQUEST 하나가 응답 없을 때 최대 6.5초까지 재시도하며
 * 이 태스크를 독점하는데, 그동안 전혀 무관한 다른 기기(예: 이미 페어링된 캠의 WAKE_HELLO
 * 응답)의 전송 요청도 같은 줄에서 같이 막혀버림(head-of-line blocking) — 콘 리붓 직후
 * 재페어링 폭주 상황에서 이게 누적되어 32칸 큐가 넘침(근본원인, 메모리:
 * project_cntl_tx_queue_full_root_cause_2026_09_10.md).
 *
 * 새 구조: 공유 큐(s_tx_queue)는 그대로 입구로 쓰되, 이제 "디스패처" 태스크 하나만 이걸
 * 소비함 — 디스패처는 절대 무선 전송을 직접 하지 않고, 항목의 mac을 보고 그 기기 전용
 * 워커 태스크가 있으면 그 워커의 개인 큐로 넘기고, 없으면 새로 만들어서 넘기기만 함(매우
 * 빠름, 블로킹 없음). 실제 esp_now_reliable_request() 블로킹 호출은 각 워커 태스크 안에서만
 * 일어나므로, 기기 A가 6.5초 걸리는 동안에도 기기 B의 워커는 독립적으로 동시에 진행됨.
 * 워커는 자기 큐가 일정 시간(TX_WORKER_IDLE_MS) 비어있으면(더 보낼 게 없으면) 스스로
 * vTaskDelete(NULL)로 종료 — 사용자가 명시적으로 기각한 두 대안: (1) 기기별 고정 8개 태스크
 * 상시 할당(~31KB 영구 RAM 낭비, "안 쓰는데도 계속 떠있잖아"), (2) 큐셋(xQueueSelectFromSet)
 * 단일 태스크(사용자: "그냥 큐를 키운 것과 마찬가지잖아" — 공정성만 있을 뿐 진짜 동시성은
 * 없음, 여전히 완전 직렬). 대신 필요할 때만 만들고 다 쓰면 없어지는 동적 방식 채택 */

#define TX_REQ_MAX_LEN 32   /* PAIR_REQUEST/PHOTO_REQUEST/LIST_REQUEST/DELETE_*_REQUEST 모두
                              * 20바이트 이하 — 여유있게 32 */
/* 2026-08-26(사용자 지시) — CASK 재설계로 WAKE_HELLO 하나당 항상 3개(CONFIG/할일/SLEEP_NOW)가
 * 들어오는데, 옛 8은 노드 3대만 겹쳐도 꽉 참. 30개 이상으로 늘림(사용자 지정) */
#define TX_QUEUE_LEN   32

/* 2026-09-10(재설계) — 기기별 워커 슬롯 수. esp_now_tx_enqueue()로 들어오는 mac은 전부
 * esp_now_hub.c가 이미 등록해둔 노드(ESP_NOW_HUB_MAX_NODES칸)에서만 나오므로, 동시에
 * 존재할 수 있는 서로 다른 mac 수도 이 값을 절대 못 넘음 — 상시 태스크 사전할당이 아니라
 * 단순 "북키핑용 배열" 크기라 메모리 비용은 무시할 수준(태스크 스택은 실제로 만들어질
 * 때만 소비됨) */
#define TX_MAX_WORKERS       ESP_NOW_HUB_MAX_NODES
#define TX_WORKER_QUEUE_LEN  8      /* WAKE_HELLO 1회당 최대 3개(CONFIG/할일/SLEEP_NOW) +
                                     * 여유 — 이 정도면 한 기기에 몰려도 넉넉함 */
#define TX_WORKER_IDLE_MS    2000   /* 이 시간 동안 새 항목이 없으면 워커가 스스로 종료 */
#define TX_WORKER_STACK      4096   /* 예전 tx_task와 동일 — 실제 esp_now_reliable_request()
                                     * 블로킹 호출이 여기서 일어남 */
#define TX_WORKER_PRIORITY   17     /* 통신 계층(사용자 설계: 통신17/SR제어15/파일처리10) —
                                     * 디스패처와 동일 우선순위, 예전 tx_task 값 그대로 */

typedef struct {
    uint8_t  mac[6];
    uint8_t  req[TX_REQ_MAX_LEN];
    size_t   req_len;
    const uint8_t *accept_reply_types;
    size_t   accept_reply_types_count;
    uint32_t timeout_ms;
    int      max_attempts;
    const char *what;
} tx_item_t;

typedef struct {
    bool          in_use;
    uint8_t       mac[6];
    QueueHandle_t queue;
} tx_worker_t;

static QueueHandle_t     s_tx_queue = NULL;       /* 입구 — 디스패처만 소비 */
static tx_worker_t       s_workers[TX_MAX_WORKERS];
static SemaphoreHandle_t s_workers_mutex = NULL;  /* s_workers[] 배열 + 각 워커의 생존여부를
                                                    * 디스패처(라우팅)와 워커 자신(자진종료)이
                                                    * 함께 건드리므로 보호 필요 */

static bool mac_eq(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 6) == 0;
}

/* 반드시 s_workers_mutex를 쥔 채로 호출 */
static tx_worker_t *find_worker_locked(const uint8_t *mac)
{
    for (int i = 0; i < TX_MAX_WORKERS; i++) {
        if (s_workers[i].in_use && mac_eq(s_workers[i].mac, mac)) return &s_workers[i];
    }
    return NULL;
}

/* 반드시 s_workers_mutex를 쥔 채로 호출 */
static tx_worker_t *find_free_slot_locked(void)
{
    for (int i = 0; i < TX_MAX_WORKERS; i++) {
        if (!s_workers[i].in_use) return &s_workers[i];
    }
    return NULL;
}

static void tx_worker_task(void *arg)
{
    tx_worker_t *w = (tx_worker_t *)arg;
    tx_item_t item;

    for (;;) {
        if (xQueueReceive(w->queue, &item, pdMS_TO_TICKS(TX_WORKER_IDLE_MS)) != pdTRUE) {
            /* 2026-09-10 — 유휴 타임아웃, 할 일이 더 없으면 종료. 단, "더 없다"는 판단을
             * 디스패처의 라우팅 판단(find_worker_locked)과 같은 뮤텍스 아래서 다시 한 번
             * 확인해야 함 — 그렇지 않으면 "이 워커가 곧 죽는다"는 걸 모르는 디스패처가
             * 바로 이 순간 이 워커의 큐로 항목을 넘기고, 그 항목이 곧 삭제될 큐와 함께
             * 유실되는 경쟁상태가 생김. 뮤텍스로 두 판단을 직렬화하면: 디스패처가 먼저
             * 넣었다면 아래 uxQueueMessagesWaiting()이 0보다 커서 종료를 취소하고 다시
             * 받으러 가고, 워커가 먼저 종료를 확정(in_use=false)했다면 디스패처의
             * find_worker_locked()가 이 워커를 못 찾아 새 워커를 만듦 — 어느 순서든 항목
             * 유실 없음 */
            xSemaphoreTake(s_workers_mutex, portMAX_DELAY);
            if (uxQueueMessagesWaiting(w->queue) > 0) {
                xSemaphoreGive(s_workers_mutex);
                continue;
            }
            QueueHandle_t q = w->queue;
            w->in_use = false;
            w->queue  = NULL;
            xSemaphoreGive(s_workers_mutex);
            vQueueDelete(q);
            vTaskDelete(NULL);
        }

        esp_err_t err = esp_now_reliable_request(item.mac, item.req, item.req_len,
                                                  item.accept_reply_types, item.accept_reply_types_count,
                                                  item.timeout_ms, item.max_attempts,
                                                  NULL, 0, NULL);
        if (err != ESP_OK) {
            /* 2026-08-10 — 예전엔 여기서 무조건 UI_ERR_NOT_PAIRED(2007, "페어링 끊김")를
             * 찍었는데, 이 모듈은 어떤 요청이든 다 거쳐가는 범용 전송 스케줄러라 "페어링
             * 끊김"이 실제 원인과 안 맞는 경우가 대부분이었음(실사용 중 발견 — CAM이
             * 딥슬립 중이라 사진요청이 무응답인 것뿐인데 2007이 3006과 같이 겹쳐서 뜸).
             * 이제 호출부마다 자기 상황에 맞는 전용 에러(UI_ERR_FETCH_NORESPONSE/
             * _LIST_NORESPONSE/_CAPTURE_NORESPONSE/_CONFIG_NORESPONSE 등, 각자의 상태머신
             * 폴링에서 이미 처리)가 있으므로 여기서는 진단용 로그만 남기고 UI 에러는 안 띄움.
             * 자동 재연결(esp_now_hub_pair)처럼 애초에 사용자에게 알릴 필요 없는 백그라운드
             * 요청도 있어서, 범용 계층에서 일괄 판단하는 게 애초에 무리였음 */
            ESP_LOGW(TAG, "%s 무응답(%d회 시도)", item.what, item.max_attempts);
        } else {
            ESP_LOGI(TAG, "%s 완료", item.what);
        }
    }
}

/* 2026-09-10(회귀버그 수정 — 사용자 지시 "워커큐 회귀버그수정부터") — SD카드 장애로 인한
 * esp_now RF 성능저하 실기 관찰: 캠 WAKE_HELLO가 ~200ms마다 계속 도는데, 워커가 응답없는
 * 요청 재시도로 몇 초씩 묶여있으면 8칸짜리 워커 큐가 금방 차서 이후 SLEEP_NOW/CONFIG가
 * 전부 버려짐(캠이 영영 못 잠, 통신 사실상 마비). 이 목록의 종류들은 전부 "지금 이 순간의
 * 최신 상태/의도"만 의미있고 과거에 큐잉됐던 값은 그 자체로 낡은 값이라 버려도 무해함
 * (Pairing도 같은 기기에 중복 요청 중이면 최신 것만 남아도 무방) — 그래서 큐가 꽉 찼을 때
 * "새로 온 걸 버리는" 대신 "제일 오래된 걸 버리고 새 걸 넣는" 쪽으로 바꿔서, 워커가
 * 느려져도 최소한 최신 상태는 항상 전달 시도됨. Photo/List/Delete 같은 1회성 사용자
 * 명령은 유실되면 안 되므로 이 목록에 안 넣고 예전처럼 그대로 버림(로그만 남김) */
static bool is_coalescable_what(const char *what)
{
    static const char *const kinds[] = { "SLEEP_NOW", "CAM config", "Sens config", "No work", "Pairing" };
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        if (strcmp(what, kinds[i]) == 0) return true;
    }
    return false;
}

/* 워커 큐(w->queue)로 item을 보냄 — 꽉 찼고 coalescable이면 가장 오래된 걸 비동기적으로
 * 하나 빼서 자리를 만들고 재시도(워커가 그 사이 직접 소비했으면 xQueueReceive가 그냥
 * pdFALSE를 주므로 재시도 자체가 즉시 성공 — 별도 락 없이도 안전, 표준 큐 API로만 해결됨) */
static void send_to_worker(tx_worker_t *w, const tx_item_t *item)
{
    if (xQueueSend(w->queue, item, 0) == pdTRUE) return;
    if (is_coalescable_what(item->what)) {
        tx_item_t discard;
        if (xQueueReceive(w->queue, &discard, 0) == pdTRUE) {
            xQueueSend(w->queue, item, 0);
            return;
        }
    }
    ESP_LOGW(TAG, "%s: 워커 큐 가득 — 버림 mac=%02X:%02X:%02X:%02X:%02X:%02X",
             item->what, item->mac[0], item->mac[1], item->mac[2],
             item->mac[3], item->mac[4], item->mac[5]);
}

static void tx_dispatcher_task(void *arg)
{
    (void)arg;
    tx_item_t item;

    for (;;) {
        if (xQueueReceive(s_tx_queue, &item, portMAX_DELAY) != pdTRUE) continue;

        xSemaphoreTake(s_workers_mutex, portMAX_DELAY);
        tx_worker_t *w = find_worker_locked(item.mac);
        if (w) {
            send_to_worker(w, &item);
        } else {
            w = find_free_slot_locked();
            if (!w) {
                /* 이론상 도달 불가 — mac은 항상 esp_now_hub.c에 이미 등록된 노드(최대
                 * ESP_NOW_HUB_MAX_NODES개)에서만 오므로 동시 워커 수도 그 이상 못 감 */
                ESP_LOGE(TAG, "%s: 워커 슬롯 부족(%d개 초과) — 버림", item.what, TX_MAX_WORKERS);
            } else {
                w->queue = xQueueCreate(TX_WORKER_QUEUE_LEN, sizeof(tx_item_t));
                memcpy(w->mac, item.mac, sizeof(w->mac));
                w->in_use = true;
                xQueueSend(w->queue, &item, 0);  /* 태스크 생성 전에 미리 넣어둠 — 유실 없음 */
                xTaskCreate(tx_worker_task, "tx_worker", TX_WORKER_STACK, w, TX_WORKER_PRIORITY, NULL);
            }
        }
        xSemaphoreGive(s_workers_mutex);
    }
}

void esp_now_tx_init(void)
{
    s_tx_queue = xQueueCreate(TX_QUEUE_LEN, sizeof(tx_item_t));
    s_workers_mutex = xSemaphoreCreateMutex();
    /* 2026-09-09(사용자 설계 — "통신 17 SR제어 15 파일처리 10") — 디스패처는 통신 계층이라
     * 예전 tx_task와 동일하게 17. 실제 워커 태스크들도 같은 우선순위(TX_WORKER_PRIORITY)로
     * 생성됨 */
    xTaskCreate(tx_dispatcher_task, "esp_now_tx_disp", 3072, NULL, 17, NULL);
}

void esp_now_tx_enqueue(const uint8_t *mac, const void *req, size_t req_len,
                         const uint8_t *accept_reply_types, size_t accept_reply_types_count,
                         uint32_t timeout_ms, int max_attempts, const char *what)
{
    if (req_len > TX_REQ_MAX_LEN) {
        ESP_LOGE(TAG, "%s: req_len(%u)이 TX_REQ_MAX_LEN(%d) 초과 — 무시", what, (unsigned)req_len, TX_REQ_MAX_LEN);
        return;
    }
    tx_item_t item = {
        .req_len               = req_len,
        .accept_reply_types    = accept_reply_types,
        .accept_reply_types_count = accept_reply_types_count,
        .timeout_ms             = timeout_ms,
        .max_attempts           = max_attempts,
        .what                   = what,
    };
    memcpy(item.mac, mac, sizeof(item.mac));
    memcpy(item.req, req, req_len);

    /* 2026-09-10(사용자 지시 — "어떤 원인으로 큐가 풀났는지를 알아야") — 큐가 꽉 차기
     * 시작하는 지점부터(20개 이상, 32개 중) 매 enqueue마다 무엇이/어디로/큐 깊이가 얼마인지
     * 남겨서, 실제로 넘칠 때 직전 로그를 보면 어떤 what/mac이 반복됐는지(재시도 폭주 등)
     * 또는 여러 노드가 동시에 몰렸는지 구분 가능하게 함. 평소(깊이 20 미만)엔 로그 안 남김.
     * 재설계 이후엔 이 입구 큐가 디스패처(블로킹 없음)만 소비하므로 거의 항상 비어있어야
     * 정상 — 그래도 안전망으로 유지 */
    UBaseType_t depth_before = uxQueueMessagesWaiting(s_tx_queue);
    if (depth_before >= 20) {
        ESP_LOGW(TAG, "TX_QUEUE_TRACE depth=%u/%d what=%s mac=%02X:%02X:%02X:%02X:%02X:%02X",
                 (unsigned)depth_before, TX_QUEUE_LEN, what,
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    if (xQueueSend(s_tx_queue, &item, 0) != pdTRUE) {
        /* 2026-08-26(사용자 지시) — "큐 풀이면 최소한 에러라도 냈어야" — 예전엔 시리얼
         * 로그(ESP_LOGW)만 남기고 화면엔 아무 표시가 없어서 시리얼 안 보고 있으면 통째로
         * 놓쳤음. 이건 재시도 여지없이 그 자리에서 완전히 버려지는 거라 워닝이 아니라
         * 에러 — ui_log_add_err()로 화면 토스트까지 뜨게 함(2xxx 통신 전송 대역) */
        ESP_LOGE(TAG, "%s: 큐 가득 — 버림", what);
        ui_log_add_err(UI_ERR_TX_QUEUE_FULL, "TX queue full, dropped: %s", what);
        /* 2026-09-10(사용자 지시 — "이 에러가 났을 때 TX 큐를 초기화하든 하는 거야") — 큐가
         * 꽉 찬 채로 계속 남아있으면 이후 모든 enqueue가 계속 실패해서 "한번 나면 계속
         * 나는" 상태가 될 수 있음. 밀린 항목을 전부 버리고 빈 상태로 되돌려서 최소한 다음
         * 요청부터는 다시 들어갈 수 있게 함 */
        UBaseType_t stuck_count = uxQueueMessagesWaiting(s_tx_queue);
        xQueueReset(s_tx_queue);
        ESP_LOGW(TAG, "TX 큐 초기화됨(밀려있던 %u개 버림)", (unsigned)stuck_count);
    }
}
