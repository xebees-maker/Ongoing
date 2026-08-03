#include "esp_now_cam.h"
#include "cam_storage.h"
#include "cam_node.h"

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "status_led.h"

static const char *TAG = "esp_now_cam";

/* --- 광고/채널스캔/페어링 — Sens/main/esp_now_node.c와 동일 패턴, 자세한 이유는
 * 그쪽 주석 참고(이 세션에서 같이 설계함) --- */
#define ADVERTISE_PERIOD_US        (1000 * 1000)
#define KEEPALIVE_PERIOD_US        (1000 * 1000)
#define SEND_FAIL_THRESHOLD        3
#define UNPAIRED_FAILED_TIMEOUT_US (30 * 1000 * 1000)
#define SCAN_DWELL_US              (300 * 1000)
#define SCAN_CHANNEL_MIN           1
#define SCAN_CHANNEL_MAX           13

static char    s_name[ESP_NOW_LINK_NAME_LEN] = "";
static uint8_t s_mac[6] = { 0 };
static esp_timer_handle_t s_advertise_timer = NULL;
static esp_timer_handle_t s_keepalive_timer = NULL;
static esp_timer_handle_t s_unpaired_failed_timer = NULL;

static bool    s_paired = false;
static uint8_t s_hub_mac[6] = { 0 };
static int     s_send_fail_count = 0;

static bool    s_channel_locked = false;
static uint8_t s_scan_channel   = SCAN_CHANNEL_MIN;

static gpio_num_t s_led_pin = GPIO_NUM_NC;

static const uint8_t s_broadcast_addr[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* --- 사진 전송 --- */
/* recv_cb(WiFi 태스크)에서 바로 처리하기엔 무거운 요청(촬영/파일 I/O/여러 건 전송)을
 * 전용 태스크로 넘기는 큐 — PHOTO_REQUEST와 PHOTO_LIST_REQUEST 둘 다 여기로 옴 */
typedef enum {
    CAM_TASK_REQ_PHOTO       = 0,
    CAM_TASK_REQ_LIST        = 1,
    CAM_TASK_REQ_DELETE_ALL  = 2,
} cam_task_req_kind_t;

typedef struct {
    cam_task_req_kind_t      kind;
    esp_now_photo_request_t  photo_req;  /* kind==CAM_TASK_REQ_PHOTO일 때만 유효 */
    uint32_t                 generation; /* kind==CAM_TASK_REQ_PHOTO일 때만 유효 — 아래 참고 */
} cam_task_request_t;

static QueueHandle_t s_photo_request_queue = NULL;

/* 청크 신뢰성 재설계(2026-08-03) — "매 청크마다 로컬 라디오의 물리계층 ACK를 기다렸다가
 * 다음으로 넘어가는" 예전 방식을 완전히 버림. 그 "ACK"는 상대(Cntl) 애플리케이션이 실제로
 * 받았다는 확인이 아니라 CAM 자신의 송신 성공 여부일 뿐이라 신뢰성 지표로 쓸 수 없었고,
 * 게다가 keepalive 등 다른 독립적인 esp_now_send()와 완료 콜백이 뒤섞이는 레이스까지 있었음
 * (send_cb는 어느 send() 호출의 완료인지 구분할 방법이 API 자체에 없음). 신뢰도 높은
 * 브로드캐스팅에서 쓰는 방식으로 교체: 청크는 그냥 순서대로 쭉 스트리밍(기다리지 않음),
 * DONE 이후 수신측이 빠진 chunk_idx만 NACK으로 콕 집어 재전송 요청 — 이게 진짜 종단간
 * 확인이라 로컬 ACK의 애매함에 의존하지 않음(esp_now_link.h의 esp_now_photo_chunk_nack_t
 * 주석 참고). 이제 esp_now_send() 완료를 굳이 기다릴 이유가 없어져서 s_awaiting_chunk_ack/
 * s_send_done_sem/s_send_serialize_mutex 전부 제거 — send_cb는 다시 단순히 실패 카운트만
 * 추적함 */
static QueueHandle_t s_nack_queue = NULL;
#define MAX_NACK_ROUNDS   3
#define NACK_WAIT_MS      800  /* 수신측이 DONE 받고 목록 대조해서 NACK 보내기까지 여유 */

/* 사진 요청 세대 번호(2026-08-02) — Cntl은 사진 전송을 취소하는 프로토콜 메시지가 없어서
 * (지금까지 "취소" 버튼은 로컬 팝업만 닫고 CAM엔 아무 통보도 안 갔음), 사용자가 목록에서
 * 다른 사진을 빠르게 다시 선택하면 CAM은 예전 요청을 여전히 전송 중인 채로 새 요청을 큐에
 * 받게 됨. 그 상태로 예전 청크를 계속 보내면: (1) 새 요청이 끝날 때까지 한참 밀리고,
 * (2) Cntl이 새 META를 받기 전에 예전 file_id의 뒤늦은 청크가 도착하면 file_id가 우연히
 * 같았던 적이 있을 때 새 수신버퍼에 잘못 섞여 들어갈 위험도 있음(사용자 지적: "통신
 * 에러가 나면 양쪽 다 상태머신을 초기화해야 하는데 CAM쪽은 CNTL이 정보를 안 주면
 * 어떤 상태에서 대기하는지조차 모른다"). PHOTO_REQUEST를 새로 받을 때마다 이 번호를
 * 증가시키고, 전송 루프(청크 단위)마다 "내가 시작될 때의 세대"와 비교해서 더 최신
 * 요청이 들어왔으면 그 자리에서 즉시 중단 — 별도 취소 메시지 없이도 "새 요청 자체가
 * 곧 취소 신호"가 되게 함(무식하지만 확실한 방법) */
static volatile uint32_t s_request_generation = 0;

static void set_led(led_pattern_t pattern)
{
    if (s_led_pin == GPIO_NUM_NC) return;
    status_led_set_pattern(s_led_pin, pattern);
}

static void enter_advertising(bool immediately_failed)
{
    s_paired = false;
    s_send_fail_count = 0;

    if (s_keepalive_timer) esp_timer_stop(s_keepalive_timer);

    s_channel_locked = false;
    s_scan_channel   = SCAN_CHANNEL_MIN;
    esp_wifi_set_channel(s_scan_channel, WIFI_SECOND_CHAN_NONE);

    esp_timer_stop(s_advertise_timer);
    esp_timer_start_periodic(s_advertise_timer, SCAN_DWELL_US);

    if (immediately_failed) {
        set_led(LED_PATTERN_BLINK_SLOW);
        if (s_unpaired_failed_timer) esp_timer_stop(s_unpaired_failed_timer);
    } else {
        set_led(LED_PATTERN_BLINK_FAST);
        if (s_unpaired_failed_timer) {
            esp_timer_stop(s_unpaired_failed_timer);
            esp_timer_start_once(s_unpaired_failed_timer, UNPAIRED_FAILED_TIMEOUT_US);
        }
    }
}

static void unpaired_failed_timer_cb(void *arg)
{
    (void)arg;
    if (s_paired) return;
    set_led(LED_PATTERN_BLINK_SLOW);
}

static void keepalive_timer_cb(void *arg)
{
    (void)arg;
    /* Sens의 SENSOR_DATA와 달리 CAM은 페어링 후 딱히 매초 보낼 데이터가 없음 —
     * PAIR_ACK를 그대로 재전송해서 "아직 살아있음"을 알림. Cntl의 기존 PAIR_ACK
     * 핸들러가 이미 paired=true/last_data_ms 갱신을 해주므로 Cntl 쪽 변경 불필요.
     * 청크 스트리밍 재설계(2026-08-03) 이후로는 완료를 기다릴 이유가 없어져서 단순
     * fire-and-forget — 위 s_nack_queue 주석 참고 */
    esp_now_pair_ack_t ack = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_PAIR_ACK,
    };
    memcpy(ack.node_mac, s_mac, sizeof(ack.node_mac));
    esp_now_send(s_hub_mac, (const uint8_t *)&ack, sizeof(ack));
}

static void send_cb(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    (void)info;

    if (!s_paired) return;

    if (status == ESP_NOW_SEND_SUCCESS) {
        s_send_fail_count = 0;
        set_led(LED_PATTERN_HEARTBEAT);
        return;
    }

    s_send_fail_count++;
    ESP_LOGW(TAG, "전송 실패 (연속 %d회)", s_send_fail_count);
    if (s_send_fail_count >= SEND_FAIL_THRESHOLD) {
        ESP_LOGW(TAG, "허브 응답 끊김으로 판단 — 재광고 시작");
        enter_advertising(true);
    }
}

/* 요청받은 chunk_idx들만 파일에서 다시 읽어 재전송 — NACK 처리용.
 * nack 구조체 전체를 받아서 필요한 원소만 memcpy로 읽음 — __attribute__((packed))라
 * missing_idx 배열 원소의 주소가 2바이트 정렬을 보장 못 해서, 포인터로 직접 넘기면
 * -Werror=address-of-packed-member에 걸림(빌드로 확인) */
static void resend_chunks(uint32_t file_id, const esp_now_photo_chunk_nack_t *nack)
{
    FILE *fp = NULL;
    uint32_t size = 0;
    if (cam_storage_open_read(file_id, &fp, &size) != ESP_OK) {
        ESP_LOGW(TAG, "재전송용 파일 열기 실패: id=%u", (unsigned)file_id);
        return;
    }
    esp_now_photo_chunk_t chunk = { .version = ESP_NOW_LINK_VERSION, .msg_type = ESP_NOW_MSG_PHOTO_CHUNK, .file_id = file_id };
    for (uint16_t i = 0; i < nack->missing_count; i++) {
        uint16_t idx;
        memcpy(&idx, &nack->missing_idx[i], sizeof(idx));
        if (fseek(fp, (long)idx * ESP_NOW_PHOTO_CHUNK_DATA_LEN, SEEK_SET) != 0) continue;
        size_t n = fread(chunk.data, 1, ESP_NOW_PHOTO_CHUNK_DATA_LEN, fp);
        chunk.chunk_idx = idx;
        chunk.chunk_len = (uint16_t)n;
        esp_now_send(s_hub_mac, (const uint8_t *)&chunk, sizeof(chunk));
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    fclose(fp);
    ESP_LOGI(TAG, "NACK 재전송 완료: file_id=%u %u개 청크", (unsigned)file_id, (unsigned)nack->missing_count);
}

/* file_id 하나를 META + CHUNK*로 스트리밍 전송(2026-08-03 재설계) — 청크마다 응답을
 * 기다리지 않고 그냥 순서대로 다 보낸 뒤 DONE. 신뢰성은 여기서 확보 안 함(로컬 라디오 ACK는
 * 종단간 확인이 아니라서 애초에 의미가 없었음, 위 s_nack_queue 주석 참고) — 대신 DONE 뒤에
 * Cntl이 빠진 chunk_idx를 NACK으로 콕 집어 요청하면 그것만 재전송하는 걸 최대
 * MAX_NACK_ROUNDS번 반복. NACK이 안 오면(수신측이 다 받았다는 뜻) 바로 끝.
 * my_generation: 세대번호 — 도중에 더 최신 요청이 들어오면 즉시 중단(위 s_request_generation
 * 주석 참고). 반환값은 "끝까지 이 요청으로 진행했는가"(대체당하지 않았는가)일 뿐, 수신측이
 * 실제로 다 받았는지는 이 함수가 알 방법이 없음(NACK이 그 판단을 대신함) */
static bool send_one_photo(uint32_t file_id, uint32_t my_generation)
{
    ESP_LOGI(TAG, "CKPT: send_one_photo 시작 file_id=%u", (unsigned)file_id);
    FILE *fp = NULL;
    uint32_t size = 0;
    if (cam_storage_open_read(file_id, &fp, &size) != ESP_OK) {
        ESP_LOGW(TAG, "파일 열기 실패: id=%u", (unsigned)file_id);
        return false;
    }
    ESP_LOGI(TAG, "CKPT: 파일 열기 완료 size=%u", (unsigned)size);

    uint16_t total_chunks = (uint16_t)((size + ESP_NOW_PHOTO_CHUNK_DATA_LEN - 1) / ESP_NOW_PHOTO_CHUNK_DATA_LEN);

    /* META를 보내기 전에 파일 전체를 한 번 스트리밍으로 훑어 CRC32를 계산해둔다(끝나면
     * rewind해서 실제 전송은 처음부터 다시 읽음) — Cntl이 청크 개수만 맞다고 안심하지 않고
     * 재조립된 내용이 실제로 같은지 확인할 수 있게. "크기/개수가 맞다 ≠ 내용이 맞다"는
     * 이번 CAM corruption 조사에서 직접 겪은 교훈(project_cam_dvp_corruption_investigation
     * 메모리 참고) — 같은 실수를 이 전송 경로에서도 반복하지 않기 위함. */
    uint32_t crc = 0;
    {
        uint8_t crc_buf[256];
        size_t n;
        while ((n = fread(crc_buf, 1, sizeof(crc_buf), fp)) > 0) {
            crc = esp_rom_crc32_le(crc, crc_buf, n);
        }
        rewind(fp);
    }
    ESP_LOGI(TAG, "CKPT: CRC 계산 완료 crc=%08x total_chunks=%u", (unsigned)crc, total_chunks);

    esp_now_photo_meta_t meta = {
        .version      = ESP_NOW_LINK_VERSION,
        .msg_type     = ESP_NOW_MSG_PHOTO_META,
        .file_id      = file_id,
        .total_size   = size,
        .total_chunks = total_chunks,
        .crc32        = crc,
    };
    /* META는 청크와 달리 NACK으로 복구할 방법이 없음(NACK 자체가 META로 받은 total_chunks
     * 기준으로 "빠진 chunk_idx"를 판단하는 거라, META가 아예 안 왔으면 Cntl은 RECEIVING
     * 상태 진입조차 못 해서 그 뒤에 오는 청크를 전부 조용히 버림 — 실기 로그로 확인
     * (2026-08-03, PHOTO_REQUEST는 도착했는데 그 직후 esp_now_send 실패가 찍히고 그대로
     * 끝나버림). META 하나 유실이 전체 전송을 통째로 무효화시키는 치명적 단일 지점이라,
     * 작고 저렴한 메시지인 만큼 여러 번 반복 전송해서 유실 확률을 크게 낮춤 */
    for (int i = 0; i < 3; i++) {
        esp_err_t meta_err = esp_now_send(s_hub_mac, (const uint8_t *)&meta, sizeof(meta));
        ESP_LOGI(TAG, "CKPT: META 전송[%d] sizeof=%u -> %s", i, (unsigned)sizeof(meta), esp_err_to_name(meta_err));
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    esp_now_photo_chunk_t chunk = { .version = ESP_NOW_LINK_VERSION, .msg_type = ESP_NOW_MSG_PHOTO_CHUNK, .file_id = file_id };
    ESP_LOGI(TAG, "CKPT: 청크 전송 시작 sizeof(chunk)=%u", (unsigned)sizeof(chunk));

    for (uint16_t idx = 0; idx < total_chunks; idx++) {
        if (s_request_generation != my_generation) {
            ESP_LOGI(TAG, "더 최신 요청으로 대체됨 — 전송 중단(file_id=%u, %u/%u청크)",
                     (unsigned)file_id, idx, total_chunks);
            fclose(fp);
            return false;
        }
        size_t n = fread(chunk.data, 1, ESP_NOW_PHOTO_CHUNK_DATA_LEN, fp);
        chunk.chunk_idx = idx;
        chunk.chunk_len = (uint16_t)n;

        /* ESP_ERR_ESPNOW_NO_MEM은 "무선으로 유실됐을 수도"가 아니라 "드라이버 내부 송신큐가
         * 꽉 차서 애초에 큐잉조차 안 됐다"는, 그 자리에서 바로 확정적으로 알 수 있는 실패임
         * (2026-08-03, 실기 로그로 확인 — 33번째 청크부터 대부분 이걸로 실패, 30cm 근거리에서
         * 유실이 이렇게 많을 리 없다던 사용자 지적이 결국 이거였음: 2ms 페이싱이 큐 배수
         * 속도보다 빨라서 33개쯤 지나면 큐가 상시 꽉 차 있었음). NACK은 "한 바퀴 다 보낸 뒤"
         * 확인하는 거라 이런 즉시-거부까지 매번 NACK 라운드로 넘기면 낭비가 크므로, 여기서
         * 짧게 대기했다가 같은 청크를 바로 재시도(큐잉 자체가 안 된 거라 재전송이 아니라
         * 최초 시도의 연장) */
        esp_err_t chunk_err;
        int attempt;
        for (attempt = 0; attempt < 10; attempt++) {
            chunk_err = esp_now_send(s_hub_mac, (const uint8_t *)&chunk, sizeof(chunk));
            if (chunk_err != ESP_ERR_ESPNOW_NO_MEM) break;
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        if (idx < 3 || chunk_err != ESP_OK) {
            /* 처음 몇 개만 상세 로그(전부 찍으면 수백 줄 쏟아짐) + 실패는 항상 로그 */
            ESP_LOGI(TAG, "CKPT: 청크[%u/%u] -> %s(시도 %d회)", idx, total_chunks, esp_err_to_name(chunk_err), attempt + 1);
        }
        vTaskDelay(pdMS_TO_TICKS(4));  /* 큐 과부하 방지 페이싱 — 2ms는 너무 빨랐음(위 주석) */
    }
    fclose(fp);
    ESP_LOGI(TAG, "CKPT: 청크 전송 루프 완료(%u개)", total_chunks);

    /* 한 바퀴 다 보냈다는 신호 + NACK 대기/재전송 라운드 — 이게 이 전송의 실제 신뢰성
     * 보장 지점(스트리밍 자체엔 신뢰성이 없음, 위 함수 설명 참고) */
    esp_now_photo_done_t done = { .version = ESP_NOW_LINK_VERSION, .msg_type = ESP_NOW_MSG_PHOTO_DONE };
    esp_err_t done_err = esp_now_send(s_hub_mac, (const uint8_t *)&done, sizeof(done));
    ESP_LOGI(TAG, "CKPT: DONE 전송 -> %s, NACK 대기 시작", esp_err_to_name(done_err));

    /* static — 808바이트짜리 구조체를 photo_tx 태스크 스택에 두지 않음(24KB로 늘리긴
     * 했지만 큰 지역변수는 습관적으로 피함, 2026-08-03) */
    static esp_now_photo_chunk_nack_t nack;
    for (int round = 0; round < MAX_NACK_ROUNDS; round++) {
        if (s_request_generation != my_generation) return false;

        if (xQueueReceive(s_nack_queue, &nack, pdMS_TO_TICKS(NACK_WAIT_MS)) != pdTRUE) {
            ESP_LOGI(TAG, "CKPT: NACK 안 옴(라운드 %d) — 전송 완료로 판단", round + 1);
            return true;  /* NACK 안 옴 — 수신측이 전부 받았다고 판단, 끝 */
        }
        if (nack.file_id != file_id || nack.missing_count == 0) continue;

        ESP_LOGI(TAG, "NACK 수신: file_id=%u %u개 재요청(라운드 %d/%d)",
                 (unsigned)file_id, (unsigned)nack.missing_count, round + 1, MAX_NACK_ROUNDS);
        resend_chunks(file_id, &nack);

        if (s_request_generation != my_generation) return false;
        esp_now_send(s_hub_mac, (const uint8_t *)&done, sizeof(done));  /* 재확인 요청 */
    }
    ESP_LOGI(TAG, "CKPT: NACK 라운드 소진");
    return true;  /* 라운드 소진 — 남은 판단은 Cntl 쪽 최종 타임아웃/에러 처리에 맡김 */
}

/* 목록 요청 — 파일 내용 전송 없이 file_id/크기만 하나씩 알려줌. 최대 500장까지 있을 수
 * 있는 순회+개별 send()라 recv_cb(WiFi 태스크)에서 바로 안 하고 여기서 처리 */
static void send_photo_list(void)
{
    uint32_t ids[CAM_STORAGE_MAX_FILES];
    int count = cam_storage_list(PHOTO_REQUEST_MODE_ALL, 0, ids, CAM_STORAGE_MAX_FILES);
    ESP_LOGI(TAG, "PHOTO_LIST_REQUEST -> %d개 항목 전송", count);

    int sent = 0;
    for (int i = 0; i < count && s_paired; i++) {
        uint32_t size = 0, capture_time = 0;
        char kind = 0;
        if (cam_storage_stat(ids[i], &size, &kind, &capture_time) != ESP_OK) continue;
        esp_now_photo_list_entry_t entry = {
            .version      = ESP_NOW_LINK_VERSION,
            .msg_type     = ESP_NOW_MSG_PHOTO_LIST_ENTRY,
            .file_id      = ids[i],
            .kind         = (uint8_t)kind,
            .capture_time = capture_time,
            .file_size    = size,
        };
        esp_now_send(s_hub_mac, (const uint8_t *)&entry, sizeof(entry));
        sent++;
        vTaskDelay(pdMS_TO_TICKS(5));  /* CHUNK 전송과 동일한 이유 — ESP-NOW 큐 과부하 방지 */
    }

    uint32_t sd_total_kb = 0, sd_used_kb = 0;
    cam_storage_get_sd_usage(&sd_total_kb, &sd_used_kb);  /* 실패해도 0/0으로 채워져서 그대로 보냄 */

    esp_now_photo_list_done_t done = {
        .version     = ESP_NOW_LINK_VERSION,
        .msg_type    = ESP_NOW_MSG_PHOTO_LIST_DONE,
        .count       = (uint16_t)sent,
        .sd_total_kb = sd_total_kb,
        .sd_used_kb  = sd_used_kb,
    };
    esp_now_send(s_hub_mac, (const uint8_t *)&done, sizeof(done));
    ESP_LOGI(TAG, "PHOTO_LIST_DONE 전송(%d개, SD %u/%uKB)", sent, (unsigned)sd_used_kb, (unsigned)sd_total_kb);
}

static void photo_transfer_task(void *arg)
{
    (void)arg;
    cam_task_request_t item;
    for (;;) {
        if (xQueueReceive(s_photo_request_queue, &item, portMAX_DELAY) != pdTRUE) continue;
        if (!s_paired) continue;

        if (item.kind == CAM_TASK_REQ_LIST) {
            send_photo_list();
            continue;
        }

        if (item.kind == CAM_TASK_REQ_DELETE_ALL) {
            int deleted = cam_storage_delete_all();
            esp_now_photo_delete_all_ack_t ack = {
                .version       = ESP_NOW_LINK_VERSION,
                .msg_type      = ESP_NOW_MSG_PHOTO_DELETE_ALL_ACK,
                .success       = (deleted >= 0) ? 1 : 0,
                .deleted_count = (uint16_t)(deleted >= 0 ? deleted : 0),
            };
            esp_err_t err = esp_now_send(s_hub_mac, (const uint8_t *)&ack, sizeof(ack));
            ESP_LOGI(TAG, "PHOTO_DELETE_ALL_ACK(성공=%d, %d개) 전송: %s", ack.success, deleted, esp_err_to_name(err));
            continue;
        }

        esp_now_photo_request_t req = item.photo_req;

        /* Cntl이 "지금 당장 새로 찍어라" 요청한 경우 — esp_camera_fb_get()이 최대 수 초
         * 블로킹될 수 있어서 recv_cb(콜백 컨텍스트)에서 바로 처리하지 않고 여기(전용 태스크)
         * 까지 큐로 넘겨서 처리한다. 접수 확인(RECEIVED)은 recv_cb에서 이미 보냈고, 여기선
         * 촬영 결과(성공/실패)만 알리고 끝 — 사진 자체는 자동 전송 안 함(2026-08-01, 촬영과
         * 전송을 분리: 예전엔 성공하면 곧바로 LATEST로 자동 전송했는데, 그 전송이 느리고
         * (200바이트/청크) 실기에서 자주 실패해서 지금촬영 팝업이 안 끝나는 문제가 있었음.
         * 이제 사진을 실제로 보려면 목록에서 선택해야 함(fetch_by_id) — 그쪽은 독립된
         * 진행 팝업으로 따로 다룸) */
        if (req.mode == PHOTO_REQUEST_MODE_CAPTURE_NOW) {
            ESP_LOGI(TAG, "CAPTURE_NOW 요청 — 즉시 촬영 시작");
            bool captured = cam_node_capture_now();
            ESP_LOGI(TAG, "CAPTURE_NOW 촬영 결과: %s", captured ? "성공" : "실패");
            esp_now_capture_status_t status = {
                .version  = ESP_NOW_LINK_VERSION,
                .msg_type = ESP_NOW_MSG_CAPTURE_STATUS,
                .status   = captured ? CAM_CAPTURE_STATUS_SUCCESS : CAM_CAPTURE_STATUS_FAILED,
            };
            esp_err_t err = esp_now_send(s_hub_mac, (const uint8_t *)&status, sizeof(status));
            ESP_LOGI(TAG, "CAPTURE_STATUS(%s) 전송: %s", captured ? "SUCCESS" : "FAILED", esp_err_to_name(err));
            continue;
        }

        uint32_t ids[CAM_STORAGE_MAX_FILES];
        int count = cam_storage_list((photo_request_mode_t)req.mode, req.param, ids, CAM_STORAGE_MAX_FILES);
        ESP_LOGI(TAG, "PHOTO_REQUEST mode=%d param=%u -> %d장 전송 시작", req.mode, (unsigned)req.param, count);

        /* send_one_photo가 이제 파일당 자기 DONE(+NACK 재전송 라운드)을 스스로 끝까지
         * 책임짐(2026-08-03 재설계) — 예전엔 여기서 전체 배치가 끝난 뒤 DONE을 한 번 더
         * 보냈는데, 이제 그러면 방금 send_one_photo가 이미 마친 완료-확인 사이클 위에
         * 불필요한 DONE이 하나 더 얹혀서 혼란만 더함 */
        for (int i = 0; i < count && s_paired; i++) {
            if (s_request_generation != item.generation) break;  /* 더 최신 요청으로 대체됨 */
            send_one_photo(ids[i], item.generation);
        }
    }
}

static void resolve_name(void)
{
    esp_wifi_get_mac(WIFI_IF_STA, s_mac);
#if defined(CONFIG_CAM_NODE_NAME)
    if (strlen(CONFIG_CAM_NODE_NAME) > 0) {
        snprintf(s_name, sizeof(s_name), "%s", CONFIG_CAM_NODE_NAME);
        return;
    }
#endif
    snprintf(s_name, sizeof(s_name), "Cam-%02X%02X", s_mac[4], s_mac[5]);
}

static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len < 2) return;
    uint8_t msg_type = data[1];

    if (!s_paired && !s_channel_locked && msg_type == ESP_NOW_MSG_ADVERTISE_ACK) {
        if (len < (int)sizeof(esp_now_advertise_ack_t)) return;
        /* s_scan_channel을 그대로 믿으면 안 됨 — advertise_timer_cb(별도 타이머 콜백)가
         * 300ms마다 이 변수를 증가시키고 채널을 바꾸는데, recv_cb(ESP-NOW 드라이버 태스크)가
         * 실제로 이 콜백을 실행하는 시점은 패킷을 "물리적으로 수신한" 시점보다 늦어질 수
         * 있어서, 그 사이에 advertise_timer_cb가 먼저 끼어들면 s_scan_channel이 이미 다음
         * 채널로 넘어간 뒤임 — 그러면 실제로는 이전 채널에서 응답을 받았는데 엉뚱하게 다음
         * 채널로 락되는 레이스가 생김(2026-08-02, 실기에서 Cntl은 항상 CH1 고정인데 CAM이
         * CH2로 락되는 게 반복 재현돼서 발견). info->rx_ctrl->channel은 드라이버가 패킷을
         * 실제로 수신한 채널을 그대로 담고 있어서 이 레이스에서 자유로움 — 이 값을 신뢰 */
        uint8_t actual_channel = (info && info->rx_ctrl) ? info->rx_ctrl->channel : s_scan_channel;
        s_scan_channel = actual_channel;
        esp_wifi_set_channel(s_scan_channel, WIFI_SECOND_CHAN_NONE);  /* 레이스로 이미 다른
                                                                          채널로 넘어갔으면 되돌림 */
        s_channel_locked = true;
        esp_timer_stop(s_advertise_timer);
        esp_timer_start_periodic(s_advertise_timer, ADVERTISE_PERIOD_US);
        ESP_LOGI(TAG, "Cntl 채널 확인됨(CH%d) — 스캔 중지, 페어링 대기", s_scan_channel);
        return;
    }

    if (msg_type == ESP_NOW_MSG_PHOTO_REQUEST) {
        if (!s_paired || len < (int)sizeof(esp_now_photo_request_t)) return;
        esp_now_photo_request_t req;
        memcpy(&req, data, sizeof(req));

        /* 지금촬영은 접수 확인을 여기서 바로 보냄(전용 태스크가 큐에서 뽑아 처리하기까지의
         * 지연과 별개로, Cntl UI 진행 팝업의 "1단계: 명령 전달" 즉시 반영용) */
        if (req.mode == PHOTO_REQUEST_MODE_CAPTURE_NOW) {
            esp_now_capture_status_t status = {
                .version  = ESP_NOW_LINK_VERSION,
                .msg_type = ESP_NOW_MSG_CAPTURE_STATUS,
                .status   = CAM_CAPTURE_STATUS_RECEIVED,
            };
            esp_err_t err = esp_now_send(s_hub_mac, (const uint8_t *)&status, sizeof(status));
            ESP_LOGI(TAG, "CAPTURE_STATUS(RECEIVED) 전송: %s", esp_err_to_name(err));
        }

        /* 새 PHOTO_REQUEST 자체가 "이전 요청은 이제 필요없다"는 신호 — 세대번호를 먼저
         * 올려서, 지금 한창 전송 중이거나 큐에서 대기 중인 이전 요청이 이 값을 보고
         * 스스로 중단하게 함(위 s_request_generation 주석 참고).
         * 단, ESP-NOW는 물리계층에서 자동 재전송을 하기 때문에 Cntl이 딱 한 번만 보낸
         * "같은" 요청이 recv_cb에는 두 번 들어올 수 있음 — 내용(mode+param)이 직전과
         * 똑같으면 진짜 새 요청이 아니라 중복 수신으로 보고 세대번호를 안 올림. 이걸
         * 안 하면 방금 시작한 자기 자신의 전송이 "새 요청이 왔다"고 스스로 오판해서
         * 첫 청크도 못 보내고 중단해버림(2026-08-02, 실기에서 "가져오기 매번 처음부터
         * 실패"로 확인) */
        static esp_now_photo_request_t s_last_req = { 0 };
        static bool s_has_last_req = false;
        bool is_duplicate = s_has_last_req && s_last_req.mode == req.mode && s_last_req.param == req.param;
        if (!is_duplicate) {
            s_request_generation++;
            s_last_req = req;
            s_has_last_req = true;
        }
        cam_task_request_t item = { .kind = CAM_TASK_REQ_PHOTO, .photo_req = req, .generation = s_request_generation };
        if (xQueueSend(s_photo_request_queue, &item, 0) != pdTRUE) {
            ESP_LOGW(TAG, "PHOTO_REQUEST 큐 가득 — 이전 전송 아직 진행중, 무시");
        }
        return;
    }

    if (msg_type == ESP_NOW_MSG_PHOTO_CHUNK_NACK) {
        if (!s_paired || len < (int)sizeof(esp_now_photo_chunk_nack_t)) return;
        /* static — 808바이트짜리 구조체를 recv_cb(드라이버 태스크) 스택에 두지 않음
         * (2026-08-03, 방금 겪은 스택 오버플로우 사고 이후 큰 지역변수는 되도록 피함) */
        static esp_now_photo_chunk_nack_t nack;
        memcpy(&nack, data, sizeof(nack));
        /* photo_tx 태스크(send_one_photo)가 직접 기다리는 큐 — 논블로킹, 못 넣으면(이미
         * 꽉 찼으면) 그냥 버림. 어차피 못 받으면 Cntl이 다음 라운드에 또 NACK을 보냄 */
        if (xQueueSend(s_nack_queue, &nack, 0) != pdTRUE) {
            ESP_LOGW(TAG, "NACK 큐 가득 — 무시(다음 라운드에 다시 옴)");
        }
        return;
    }

    if (msg_type == ESP_NOW_MSG_PHOTO_LIST_REQUEST) {
        if (!s_paired || len < (int)sizeof(esp_now_photo_list_request_t)) return;
        cam_task_request_t item = { .kind = CAM_TASK_REQ_LIST };
        if (xQueueSend(s_photo_request_queue, &item, 0) != pdTRUE) {
            ESP_LOGW(TAG, "PHOTO_LIST_REQUEST 큐 가득 — 무시");
        }
        return;
    }

    if (msg_type == ESP_NOW_MSG_PHOTO_DELETE_ALL_REQUEST) {
        if (!s_paired || len < (int)sizeof(esp_now_photo_delete_all_request_t)) return;
        cam_task_request_t item = { .kind = CAM_TASK_REQ_DELETE_ALL };
        if (xQueueSend(s_photo_request_queue, &item, 0) != pdTRUE) {
            ESP_LOGW(TAG, "PHOTO_DELETE_ALL_REQUEST 큐 가득 — 무시");
        }
        return;
    }

    if (msg_type == ESP_NOW_MSG_PHOTO_DELETE_REQUEST) {
        if (!s_paired || len < (int)sizeof(esp_now_photo_delete_request_t)) return;
        esp_now_photo_delete_request_t req;
        memcpy(&req, data, sizeof(req));
        esp_err_t err = cam_storage_delete(req.file_id);
        ESP_LOGI(TAG, "PHOTO_DELETE_REQUEST id=%u: %s", (unsigned)req.file_id, esp_err_to_name(err));
        esp_now_photo_delete_ack_t ack = {
            .version  = ESP_NOW_LINK_VERSION,
            .msg_type = ESP_NOW_MSG_PHOTO_DELETE_ACK,
            .file_id  = req.file_id,
            .success  = (err == ESP_OK) ? 1 : 0,
        };
        esp_now_send(s_hub_mac, (const uint8_t *)&ack, sizeof(ack));
        return;
    }

    /* Cntl이 연결 해제했다는 통보 — 우리 쪽 hub로 등록된 상대가 보낸 것만 인정하고
     * 페어링 전(광고/채널스캔) 상태로 돌아감. 이게 없으면 Cntl이 끊어도 CAM은 몰라서
     * keepalive를 계속 보내 Cntl 목록에서 "연결됨"으로 되돌아가버림(실기로 확인됨). */
    if (msg_type == ESP_NOW_MSG_UNPAIR) {
        if (!s_paired || len < (int)sizeof(esp_now_unpair_t)) return;
        if (memcmp(info->src_addr, s_hub_mac, sizeof(s_hub_mac)) != 0) return;
        ESP_LOGI(TAG, "Cntl이 연결 해제함 — 광고 재개");
        enter_advertising(false);
        return;
    }

    /* Cntl 소프트리셋 대응(2026-08-02) — Cntl이 재시작하면 노드 테이블이 통째로 비워지는데,
     * 우리는 이미 페어링됐다고 믿고 ADVERTISE를 멈춘 채 PAIR_ACK(keepalive)만 계속 보냄.
     * 근데 esp_now_send()의 성공/실패는 물리계층 ACK 기준이라 상대가 그 keepalive를
     * 애플리케이션 레벨에서 무시해도 우리는 계속 "성공"으로만 보여서 SEND_FAIL_THRESHOLD가
     * 절대 안 걸림 — 즉 스스로는 이 상태를 못 벗어남(실기로 확인). Cntl이 부팅 직후 이
     * 브로드캐스트를 한 번 보내주면, 페어링돼 있던 노드도 강제로 재광고 모드로 돌아감 */
    if (msg_type == ESP_NOW_MSG_HUB_RESET) {
        if (len < (int)sizeof(esp_now_hub_reset_t)) return;
        if (s_paired) {
            ESP_LOGI(TAG, "Cntl 재시작 감지(HUB_RESET) — 재광고 시작");
            enter_advertising(false);
        }
        return;
    }

    /* CAM은 자체 RTC가 없어서 부팅하면 시계가 1970-01-01 근처 — Cntl이 페어링될 때마다
     * 자기 시각을 알려주면 그걸로 시스템 클록을 맞춤(사진 file_id가 이 시각 기준이라
     * 정확한 촬영시각 표시에 필요, 2026-08-01 추가) */
    if (msg_type == ESP_NOW_MSG_SET_TIME) {
        if (!s_paired || len < (int)sizeof(esp_now_set_time_t)) return;
        if (memcmp(info->src_addr, s_hub_mac, sizeof(s_hub_mac)) != 0) return;
        const esp_now_set_time_t *msg = (const esp_now_set_time_t *)data;
        struct timeval tv = { .tv_sec = (time_t)msg->unix_time, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "SET_TIME 수신 — 시각 동기화: %u", (unsigned)msg->unix_time);
        return;
    }

    if (s_paired || msg_type != ESP_NOW_MSG_PAIR_REQUEST) return;
    if (len < (int)sizeof(esp_now_pair_request_t)) return;
    const esp_now_pair_request_t *req = (const esp_now_pair_request_t *)data;

    memcpy(s_hub_mac, req->hub_mac, sizeof(s_hub_mac));

    esp_now_peer_info_t peer = { 0 };
    memcpy(peer.peer_addr, s_hub_mac, sizeof(peer.peer_addr));
    peer.ifidx   = WIFI_IF_STA;
    peer.channel = 0;
    peer.encrypt = false;
    if (!esp_now_is_peer_exist(s_hub_mac)) {
        ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    }

    s_paired = true;
    s_send_fail_count = 0;
    esp_timer_stop(s_advertise_timer);
    if (s_unpaired_failed_timer) esp_timer_stop(s_unpaired_failed_timer);
    if (!s_keepalive_timer) {
        const esp_timer_create_args_t ka_args = { .callback = keepalive_timer_cb, .name = "cam_keepalive" };
        ESP_ERROR_CHECK(esp_timer_create(&ka_args, &s_keepalive_timer));
    }
    esp_timer_start_periodic(s_keepalive_timer, KEEPALIVE_PERIOD_US);
    set_led(LED_PATTERN_HEARTBEAT);

    esp_now_pair_ack_t ack = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_PAIR_ACK,
    };
    memcpy(ack.node_mac, s_mac, sizeof(ack.node_mac));
    esp_err_t err = esp_now_send(s_hub_mac, (const uint8_t *)&ack, sizeof(ack));
    ESP_LOGI(TAG, "페어링됨: hub " MACSTR ", PAIR_ACK %s", MAC2STR(s_hub_mac), esp_err_to_name(err));
}

static void advertise_timer_cb(void *arg)
{
    (void)arg;
    esp_now_advertise_t msg = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_ADVERTISE,
    };
    memcpy(msg.name, s_name, sizeof(msg.name));
    memcpy(msg.mac, s_mac, sizeof(msg.mac));
    esp_now_send(s_broadcast_addr, (const uint8_t *)&msg, sizeof(msg));

    if (!s_channel_locked) {
        s_scan_channel++;
        if (s_scan_channel > SCAN_CHANNEL_MAX) s_scan_channel = SCAN_CHANNEL_MIN;
        esp_wifi_set_channel(s_scan_channel, WIFI_SECOND_CHAN_NONE);
    }
}

void esp_now_cam_set_status_led(gpio_num_t pin)
{
    s_led_pin = pin;
    status_led_init(pin);
}

void esp_now_cam_init(void)
{
    resolve_name();
    ESP_LOGI(TAG, "노드 이름: %s (MAC " MACSTR ")", s_name, MAC2STR(s_mac));

    s_photo_request_queue = xQueueCreate(4, sizeof(cam_task_request_t));
    /* NACK은 send_one_photo()가 자기 전송 도중(같은 photo_tx 태스크) 직접 기다리므로 슬롯
     * 1~2개면 충분 — 여러 개 쌓일 상황 자체가 없음(파일 하나 처리 끝나야 다음 큐 항목으로
     * 넘어가는 단일 소비자 구조) */
    s_nack_queue = xQueueCreate(2, sizeof(esp_now_photo_chunk_nack_t));
    /* 4096으로는 촬영(esp_camera_fb_get)+SD 저장(FATFS) 경로에서 스택 오버플로우 실기 확인
     * (2026-08-01) — 여유있게 증설했었으나, 목록조회(LIST) 경로에서 또 다른 스택 오버플로우가
     * 실기에서 확인됨(2026-08-03) — CAM 크래시 후 재부팅되면서 Cntl 쪽엔 그냥 "무응답
     * 타임아웃(3006/3007)"으로만 보여서 오랫동안 원인을 못 찾았음(assert failed:
     * xTaskRemoveFromEventList, backtrace가 photo_transfer_task->xQueueReceive를 가리킴 —
     * 스택 손상이 다음 xQueueReceive 호출 시점에야 드러난 것). 이 태스크 안에서 LIST
     * 처리 시 photo_transfer_task의 uint32_t ids[500](2000B) + send_photo_list()의 자체
     * uint32_t ids[500](2000B) + cam_storage_list()의 file_entry_t all[500](~6~8KB)가
     * 중첩 호출로 스택에 동시에 쌓여서 12KB를 넘겼던 것으로 추정 — 24KB로 증설 */
    xTaskCreate(photo_transfer_task, "photo_tx", 24576, NULL, 5, NULL);

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));
    ESP_ERROR_CHECK(esp_now_register_send_cb(send_cb));

    esp_now_peer_info_t peer = { 0 };
    memcpy(peer.peer_addr, s_broadcast_addr, sizeof(peer.peer_addr));
    peer.ifidx   = WIFI_IF_STA;
    peer.channel = 0;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    const esp_timer_create_args_t adv_args = { .callback = advertise_timer_cb, .name = "cam_adv" };
    ESP_ERROR_CHECK(esp_timer_create(&adv_args, &s_advertise_timer));

    const esp_timer_create_args_t unpaired_args = { .callback = unpaired_failed_timer_cb, .name = "cam_unpaired" };
    ESP_ERROR_CHECK(esp_timer_create(&unpaired_args, &s_unpaired_failed_timer));

    enter_advertising(false);
}

const char *esp_now_cam_get_name(void) { return s_name; }
bool esp_now_cam_is_paired(void) { return s_paired; }
