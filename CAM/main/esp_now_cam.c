#include "esp_now_cam.h"
#include "cam_storage.h"
#include "cam_node.h"  /* cam_node_set_capture_interval_sec/set_response_interval_sec/get_wake_reason/note_activity */

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "esp_now.h"
#include "esp_now_channelsync.h"
#include "esp_now_reliable.h"
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

/* --- 페어링 --- 채널 추적(스캔/광고/생존확인)은 전부 esp_now_channelsync로 이관됨
 * (2026-08-04 재설계 — 예전엔 keepalive가 PAIR_ACK를 재사용하고 send_cb의 물리계층 ACK로
 * 생존을 판정했는데, 실기에서 "사진 전송 중엔 이 판정을 아예 쉬는" 플래그(s_transfer_active)
 * 때문에 30분 벤치마크 내내 공유기 채널전환 감지가 통째로 잠들어 있던 게 발견됨 —
 * esp_now_channelsync.h 헤더 설명 참고). 여기 남은 건 "채널이 맞다는 전제 하에, 사용자가
 * 승인한 페어링(PAIR_REQUEST/PAIR_ACK)"만 다룸 */

static char    s_name[ESP_NOW_LINK_NAME_LEN] = "";
static uint8_t s_mac[6] = { 0 };

/* 2026-08-23(사용자 지시) — 흩어진 s_synced(esp_now_channelsync.c)/s_paired 두 변수가
 * 서로 다른 타이밍에 바뀌면서 레이스가 나던 문제의 근본 해법: 이 장치의 연결 상태를 하나의
 * 변수로만 관리. ORPHAN(소속 허브 없음, 스캔 중) -> FOUND(채널 찾음, 아직 미승인) ->
 * PAIRED(PAIR_REQUEST/PAIR_ACK 완료). channelsync 쪽 s_synced는 그 레이어 자신의 내부
 * 구현 디테일(채널을 찾았는가)로 남겨두고, 이 enum이 앱 레벨의 진짜 상태를 대표함 */
typedef enum {
    CAM_CONN_ORPHAN = 0,
    CAM_CONN_FOUND,
    CAM_CONN_PAIRED,
} cam_conn_state_t;

static cam_conn_state_t s_conn_state = CAM_CONN_ORPHAN;
static uint8_t s_hub_mac[6] = { 0 };

static gpio_num_t s_led_pin = GPIO_NUM_NC;

/* Deep Sleep 사이클 통계(2026-08-10) — 요청-응답 아님(ACK 없음), CHANNEL_PING처럼 그냥
 * 보내기만 함. CAM_CONFIG_SET 처리 직후 1회만 보냄(recv_cb의 ESP_NOW_MSG_CAM_CONFIG_SET
 * 핸들러 참고) — 페어링 직후 바로 보내면 아직 Cntl의 실제 설정값을 못 받은 상태라
 * sleep_interval_sec이 Kconfig 기본값으로 잘못 찍히는 문제가 있었음(실사용 중 발견) —
 * 반드시 설정 반영 이후여야 정확함. Light Sleep 시절의 주기 타이머 방식과 달리, 딥슬립
 * 사이클마다 재부팅되는 구조에서는 "깨어있는 동안 1회"가 곧 "사이클마다 1회"와 같음. */
static void send_deep_sleep_stats(void)
{
    esp_now_deep_sleep_stats_t stats = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_DEEP_SLEEP_STATS,
        .wake_reason             = (uint8_t)cam_node_get_wake_reason(),
        .awake_uptime_ms         = (uint32_t)(esp_timer_get_time() / 1000),
        .sleep_interval_sec      = cam_node_get_response_interval_sec(),
        .actual_last_sleep_sec   = cam_node_get_last_actual_sleep_sec(),
    };
    /* 2026-08-22 — 배터리 잔량 보고(반응형 주기에 실어 보냄, 사용자 지시). 읽기 실패해도
     * (raw/mv 0으로 남음) 리포트 자체는 그대로 보냄 — 다른 필드는 유효하므로 */
    uint16_t batt_raw = 0, batt_mv = 0;
    cam_node_read_battery_mv(&batt_raw, &batt_mv);
    stats.battery_adc_raw = batt_raw;
    stats.battery_mv      = batt_mv;

    esp_err_t err = esp_now_send(s_hub_mac, (const uint8_t *)&stats, sizeof(stats));
    ESP_LOGI(TAG, "DEEP_SLEEP_STATS 전송: wake_reason=%u awake_ms=%u interval=%us 실제잔시간=%us "
             "batt_raw=%u batt_mv=%u (%s)",
             stats.wake_reason, (unsigned)stats.awake_uptime_ms,
             (unsigned)stats.sleep_interval_sec, (unsigned)stats.actual_last_sleep_sec,
             (unsigned)batt_raw, (unsigned)batt_mv, esp_err_to_name(err));
}

/* 2026-08-23(사용자 설계) — 전력 로그를 "웨이크 사이클당 1회"가 아니라 응답성 주기마다
 * 보내도록 확장. 사이클당 1회만 보내면 깨어있는 시간이 (적응형 대기 등으로) 들쭉날쭉할 때
 * 보고 간격도 같이 들쭉날쭉해짐 — 응답성 주기 동안 얼마나 자고 얼마나 깼는지가 더 중요하다는
 * 지적(위 send_deep_sleep_stats 필드 awake_uptime_ms/actual_last_sleep_sec이 그대로 실어감).
 * 응답성=0(즉시/Live)이면 애초에 안 잘 생각이라 "자고 깬 비율" 자체가 의미 없어서 이 주기
 * 리포트는 아예 안 돎(사용자 지시) — 기존의 설정 반영 직후 1회성 리포트는 그대로 유지 */
static esp_timer_handle_t s_stats_timer = NULL;

static void stats_timer_cb(void *arg)
{
    (void)arg;
    if ((s_conn_state == CAM_CONN_PAIRED)) send_deep_sleep_stats();
}

static void restart_stats_timer(uint32_t interval_sec)
{
    if (!s_stats_timer) {
        const esp_timer_create_args_t args = { .callback = stats_timer_cb, .name = "cam_stats" };
        esp_timer_create(&args, &s_stats_timer);
    }
    esp_timer_stop(s_stats_timer);  /* 이미 멈춰있어도 안전(ESP_ERR_INVALID_STATE 무시) */
    if (interval_sec != 0) {
        esp_timer_start_periodic(s_stats_timer, (uint64_t)interval_sec * 1000000ULL);
    }
}

/* --- 사진 전송 --- */
/* recv_cb(WiFi 태스크)에서 바로 처리하기엔 무거운 요청(촬영/파일 I/O/여러 건 전송)을
 * 전용 태스크로 넘기는 큐 — PHOTO_REQUEST와 PHOTO_LIST_REQUEST 둘 다 여기로 옴 */
typedef enum {
    CAM_TASK_REQ_PHOTO       = 0,
    CAM_TASK_REQ_LIST        = 1,
    CAM_TASK_REQ_DELETE_ALL  = 2,
    CAM_TASK_REQ_BENCH       = 3,
} cam_task_req_kind_t;

typedef struct {
    cam_task_req_kind_t      kind;
    esp_now_photo_request_t  photo_req;  /* kind==CAM_TASK_REQ_PHOTO일 때만 유효 */
    uint32_t                 generation; /* kind==CAM_TASK_REQ_PHOTO일 때만 유효 — 아래 참고 */
    uint16_t                 bench_duration_sec; /* kind==CAM_TASK_REQ_BENCH일 때만 유효 */
    uint8_t                  bench_mode;         /* kind==CAM_TASK_REQ_BENCH일 때만 유효 —
                                                     esp_now_bench_mode_t(2026-08-05, SR 실험) */
} cam_task_request_t;

static QueueHandle_t s_photo_request_queue = NULL;
static volatile bool s_list_request_pending = false;  /* recv_cb 중복 LIST_REQUEST 억제용 */
static volatile bool s_list_abort_requested = false;  /* 2026-08-11 — Cntl이 PHOTO_LIST_ERROR를
                                                           보내면 세팅, send_photo_list()가
                                                           다음 배치 보내기 전에 확인하고 중단 */

/* 청크 신뢰성 재설계(2026-08-03) — "매 청크마다 로컬 라디오의 물리계층 ACK를 기다렸다가
 * 다음으로 넘어가는" 예전 방식을 완전히 버림. 그 "ACK"는 상대(Cntl) 애플리케이션이 실제로
 * 받았다는 확인이 아니라 CAM 자신의 송신 성공 여부일 뿐이라 신뢰성 지표로 쓸 수 없었고,
 * 게다가 keepalive 등 다른 독립적인 esp_now_send()와 완료 콜백이 뒤섞이는 레이스까지 있었음
 * (send_cb는 어느 send() 호출의 완료인지 구분할 방법이 API 자체에 없음). 신뢰도 높은
 * 브로드캐스팅에서 쓰는 방식으로 교체: 청크는 그냥 순서대로 쭉 스트리밍(기다리지 않음),
 * DONE 이후 수신측이 빠진 chunk_idx만 NACK으로 콕 집어 재전송 요청 — 이게 진짜 종단간
 * 확인이라 로컬 ACK의 애매함에 의존하지 않음(esp_now_link.h의 esp_now_photo_chunk_nack_t
 * 주석 참고). 이제 esp_now_send() 완료를 굳이 기다릴 이유가 없어져서 s_awaiting_chunk_ack/
 * s_send_done_sem/s_send_serialize_mutex 전부 제거 — send_cb는 더 이상 실패 카운트도 안 추적함
 * (2026-08-04 재설계, 생존판정은 esp_now_channelsync가 전담 — 아래 send_cb 참고).
 * 2026-08-05 — DONE 확인 대기용이었던 s_nack_queue/ESP_NOW_MSG_PHOTO_CHUNK_NACK 수신 경로는
 * esp_now_reliable_request()로 대체되어 제거됨(send_one_photo() 참고) — 이제 DONE_ACK가
 * reliable 레이어의 응답으로 직접 돌아옴 */
/* 2026-08-21 — 예전엔 이 값을 여기 상수로 하드코딩하고 Cntl(esp_now_photo.c)도 똑같은 값을
 * 따로 하드코딩했었음. 두 쪽 다 "몇 라운드째인가"를 각자 판단 기준으로 쓰는 값이라 반드시
 * 같아야 하는데, 그 전제가 코드로 강제되지 않아서 실제로 off-by-one이 나서 어긋난 적 있음
 * (Cntl이 CAM은 이미 포기한 라운드를 계속 기다리는 버그, 3006 오탐으로 나타남) — 이제
 * CNTL이 유일한 소유자, CAM_CONFIG_SET으로 전달받은 값을 씀(기본값 3은 구버전 CNTL/값
 * 미수신 시에만 쓰이는 안전값, feedback_cntl_owns_mutually_judged_values 메모리 참고) */
static uint8_t s_nack_max_rounds = 3;
/* 청크 버스트 중 CHANNEL_PING이 큐에서 밀리는 문제 완화용(2026-08-05, 위 청크 루프 주석
 * 참고) — 10개마다 50ms 쉬어서 큐를 비움. 10개×(10ms 페이싱)=100ms 주기에 50ms를 더 얹는
 * 셈이라 전송 시간이 그만큼 늘지만(약 1.5배), PING 왕복(현재 500ms 타임아웃) 안에 여유있게
 * 끼어들 수 있는 수준 */
#define CHUNK_QUEUE_DRAIN_INTERVAL 10
#define CHUNK_QUEUE_DRAIN_MS       50

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

/* 2026-08-23(사용자 지시) — 광고 전송 직전 게이트. PAIRED가 아닐 때만(ORPHAN/FOUND) 보냄 —
 * esp_now_channelsync_set_should_advertise_cb()로 등록됨(esp_now_cam_init() 참고) */
static bool should_advertise(void)
{
    return s_conn_state == CAM_CONN_ORPHAN || s_conn_state == CAM_CONN_FOUND;
}

/* 2026-08-23(사용자 지시: "로그는 수행하는 함수 바로 밑에 있어야지") — 주기 폴링이 아니라
 * 상태가 실제로 바뀌는 그 자리마다 바로 찍음(이벤트 기반). 숫자 말고 enum 이름으로 */
static const char *conn_state_name(cam_conn_state_t s)
{
    switch (s) {
        case CAM_CONN_ORPHAN: return "ORPHAN";
        case CAM_CONN_FOUND:  return "FOUND";
        case CAM_CONN_PAIRED: return "PAIRED";
    }
    return "?";
}

/* esp_now_channelsync 콜백(2026-08-04) — 채널 동기화될 때마다 호출됨. 페어링 자체는 여기서
 * 안 건드림(그건 PAIR_REQUEST/PAIR_ACK 핸드셰이크의 몫, recv_cb 참고) — 채널이 다시 맞았다는
 * 것만 반영하고, LED로 "허브를 찾았다"를 표시 */
static void on_channel_synced(uint8_t channel, const uint8_t *hub_mac)
{
    (void)channel; (void)hub_mac;
    if (s_conn_state == CAM_CONN_ORPHAN) {
        s_conn_state = CAM_CONN_FOUND;
        ESP_LOGI(TAG, "[STATE] -> %s", conn_state_name(s_conn_state));
    }
    set_led(LED_PATTERN_BLINK_FAST);
    cam_node_signal_recheck();  /* 2026-08-23 — 페어링 상태 변화, 대기 루프 즉시 재판정 */
}

/* 연속 PING 무응답으로 채널 동기화가 끊겼다고 판단된 순간 호출됨 — 채널이 안 맞는 상태에서
 * "페어링됨"으로 남아있으면 desync이므로 여기서 바로 정리(오늘 실기에서 겪은 문제의 근본
 * 원인이자, 이 재설계의 핵심 목적) */
static void on_channel_lost_sync(void)
{
    s_conn_state = CAM_CONN_ORPHAN;
    ESP_LOGI(TAG, "[STATE] -> %s (lost_sync)", conn_state_name(s_conn_state));
    set_led(LED_PATTERN_BLINK_SLOW);
    cam_node_note_scan_restarted();  /* 2026-08-23 — 재스캔 시작, 이전 스윕완료 기록 무효화 */
    cam_node_signal_recheck();  /* 2026-08-23 — 페어링 상태 변화, 대기 루프 즉시 재판정 */
}

/* 순수 로컬 표시(LED)용으로만 남김 — 생존/연결 판정은 전부 esp_now_channelsync의
 * CHANNEL_PING/PONG(애플리케이션 레벨 왕복)이 전담함. 물리계층 ACK(send_cb의 성공/실패)는
 * "진짜 도달 확인"이 아니라는 게 오늘 세션에서 확정된 원칙이라 판정 기준으로 안 씀.
 * 2026-08-24 — 다만 "광고가 실제로 무선에 나갔는가"는 이 콜백만이 알 수 있는 정보라(esp_now_send()의
 * 동기 리턴값은 큐잉 확인일 뿐, 위 esp_now_channelsync.h 주석 참고), 목적지가 브로드캐스트면
 * (이 프로젝트에서 광고만 브로드캐스트로 나감) 채널싱크로 완료를 알려줌 — "전송됨" 로그/소리가
 * 여기서 성공 확인된 순간에만 나가게 됨 */
static const uint8_t s_broadcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static void send_cb(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    if (info && info->des_addr && memcmp(info->des_addr, s_broadcast_mac, sizeof(s_broadcast_mac)) == 0) {
        if (status == ESP_NOW_SEND_SUCCESS) {
            esp_now_channelsync_notify_advertise_send_done();
        }
        return;
    }
    if ((s_conn_state == CAM_CONN_PAIRED) && status == ESP_NOW_SEND_SUCCESS) {
        set_led(LED_PATTERN_HEARTBEAT);
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
        /* 최초 스트리밍 루프와 동일한 NO_MEM 재시도+페이싱(2026-08-04) — 이 함수는 그
         * 수정 이전의 2ms/재시도없음 그대로 남아있었음. 큐가 꽉 차서 재전송 자체가
         * 조용히 실패하면 NACK 라운드를 다 써도 복구가 안 되고, 심하면 Cntl이 DONE을
         * 영영 못 받아 8초 정체(UI_ERR_FETCH_NORESPONSE=3006)로 이어질 수 있음 — 아래
         * DONE 재전송 수정과 함께 봐야 하는 짝 */
        esp_err_t err;
        int attempt;
        for (attempt = 0; attempt < 6; attempt++) {
            err = esp_now_send(s_hub_mac, (const uint8_t *)&chunk, sizeof(chunk));
            if (err != ESP_ERR_ESPNOW_NO_MEM) break;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "재전송 실패: chunk[%u] -> %s(시도 %d회)", idx, esp_err_to_name(err), attempt + 1);
        }
        /* 2026-08-08 — 10ms->5ms: MCS0(6.5Mbps)로 올리면 프레임 전송시간이 ~1.5ms로 줄어서
         * 1Mbps 기준으로 튜닝됐던 10ms에는 더 이상 못 미칠 이유가 없음(위 esp_now_add_peer
         * 옆 레이트설정 주석 참고). NO_MEM 재시도(위 6회*20ms)가 여전히 안전망이라 큐가
         * 실제로 못 따라가면 이 루프 자체가 자동으로 더 기다림 — 5ms는 "시도값"일 뿐. */
        vTaskDelay(pdMS_TO_TICKS(5));
        if ((i + 1) % CHUNK_QUEUE_DRAIN_INTERVAL == 0) {
            vTaskDelay(pdMS_TO_TICKS(CHUNK_QUEUE_DRAIN_MS));  /* CHANNEL_PING 큐잉 지연 완화 — 위 청크 루프 주석 참고 */
        }
    }
    fclose(fp);
    ESP_LOGI(TAG, "NACK 재전송 완료: file_id=%u %u개 청크", (unsigned)file_id, (unsigned)nack->missing_count);
}

/* file_id 하나를 META + CHUNK*로 스트리밍 전송(2026-08-03 재설계) — 청크마다 응답을
 * 기다리지 않고 그냥 순서대로 다 보낸 뒤 DONE. 신뢰성은 여기서 확보 안 함(로컬 라디오 ACK는
 * 종단간 확인이 아니라서 애초에 의미가 없었음) — 대신 DONE을 esp_now_reliable_request()로
 * 보내고 PHOTO_DONE_ACK을 기다림(2026-08-05, Layer 1). missing_count가 0이면 완료, 아니면
 * 그 chunk_idx만 재전송하는 걸 최대 MAX_NACK_ROUNDS번 반복.
 * my_generation: 세대번호 — 도중에 더 최신 요청이 들어오면 즉시 중단(위 s_request_generation
 * 주석 참고). 반환값은 "끝까지 이 요청으로 진행했는가"(대체당하지 않았는가)일 뿐, 수신측이
 * 실제로 다 받았는지는 이 함수가 알 방법이 없음(DONE_ACK가 그 판단을 대신함) */
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
        vTaskDelay(pdMS_TO_TICKS(20));  /* 5ms는 큐가 회복하기엔 너무 짧았음(2026-08-04 실기 로그) */
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
        /* 2026-08-04 실기 로그로 확인: 5ms/10회 재시도로는 큐가 회복이 안 됨 — 청크
         * 32~69 중 36개가 재시도를 전부 소진하고도 여전히 NO_MEM(한 번 포화되면 그
         * 뒤로 계속 포화 상태였다는 뜻, 일시적 버스트가 아님). 재시도 간격을 5ms->20ms로
         * 늘려서 큐가 실제로 비워질 시간을 줌(대신 최대 시도는 10->6으로 줄여서 한
         * 청크가 막힐 때 최악의 경우 지연시간은 비슷하게 유지: 6*20=120ms vs 기존 10*5=50ms,
         * 약간 늘었지만 사진 1장(수십~수백 청크) 기준 감내 가능한 수준) */
        esp_err_t chunk_err;
        int attempt;
        for (attempt = 0; attempt < 6; attempt++) {
            chunk_err = esp_now_send(s_hub_mac, (const uint8_t *)&chunk, sizeof(chunk));
            if (chunk_err != ESP_ERR_ESPNOW_NO_MEM) break;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (idx < 3 || chunk_err != ESP_OK) {
            /* 처음 몇 개만 상세 로그(전부 찍으면 수백 줄 쏟아짐) + 실패는 항상 로그 */
            ESP_LOGI(TAG, "CKPT: 청크[%u/%u] -> %s(시도 %d회)", idx, total_chunks, esp_err_to_name(chunk_err), attempt + 1);
        }
        vTaskDelay(pdMS_TO_TICKS(10));  /* 큐 과부하 방지 페이싱 — 4ms도 여전히 너무 빨랐음
                                          * (2026-08-04 실기 로그, 위 재시도 주석 참고) */

        /* 2026-08-05 — CHANNEL_PING(esp_now_channelsync)이 이 청크들과 같은 ESP-NOW 송신큐를
         * 공유함(ESP-IDF에 우선순위/QoS 태깅 API가 없어서 분리 불가 — 소스 확인함, 사전컴파일
         * libespnow.a). 청크를 쉼 없이 계속 큐잉하면 PING이 그 뒤에 밀려서 500ms 타임아웃을
         * 넘기고, 실제로 채널은 멀쩡한데 "동기화 끊김"으로 오판해 CAM이 채널을 이탈하는 문제가
         * 실기에서 재현됨(사진가져오기 반복 중 3006). CHUNK_QUEUE_DRAIN_INTERVAL개마다 큐가
         * 완전히 비워질 만큼(CHUNK_QUEUE_DRAIN_MS) 쉬어서 PING이 그 틈에 밀리지 않고 나갈 수
         * 있게 함 — 사용자 지시: "지연시간 때문에 문제되지 않는 크기로 N을 잡는다" */
        if ((idx + 1) % CHUNK_QUEUE_DRAIN_INTERVAL == 0) {
            vTaskDelay(pdMS_TO_TICKS(CHUNK_QUEUE_DRAIN_MS));
        }
    }
    fclose(fp);
    ESP_LOGI(TAG, "CKPT: 청크 전송 루프 완료(%u개)", total_chunks);

    /* 한 바퀴 다 보냈다는 신호 + 확인/재전송 라운드 — 이게 이 전송의 실제 신뢰성 보장 지점
     * (스트리밍 자체엔 신뢰성이 없음, 위 함수 설명 참고).
     * 2026-08-05 Layer 1 재설계 — 예전엔 DONE을 3번 반복 전송한 뒤 별도 큐(s_nack_queue)로
     * NACK이 오나 안 오나 지켜보고 "안 오면 완료로 판단"하는 낙관적 방식이었음(물리 ACK와
     * 마찬가지로 "응답이 없다"를 "성공"으로 해석하는 게 원리적으로 찜찜한 지점이었음).
     * 이제 esp_now_reliable_request()가 DONE을 보내고 PHOTO_DONE_ACK을 "반드시" 기다림 —
     * 응답이 진짜 안 오면 그 자체를 무응답으로 취급(재시도는 레이어가 대신 함), 응답이 오면
     * missing_count로 완료/누락을 명시적으로 구분함 */
    esp_now_photo_done_t done = { .version = ESP_NOW_LINK_VERSION, .msg_type = ESP_NOW_MSG_PHOTO_DONE };
    static const uint8_t s_done_ack_types[] = { ESP_NOW_MSG_PHOTO_DONE_ACK };
    static esp_now_photo_chunk_nack_t ack;  /* static — 800B+ 구조체를 스택에 안 둠(2026-08-03
                                                스택 오버플로우 사고 이후 원칙) */

    for (int round = 0; round < s_nack_max_rounds; round++) {
        if (s_request_generation != my_generation) return false;

        size_t reply_len = 0;
        esp_err_t err = esp_now_reliable_request(s_hub_mac, &done, sizeof(done),
                                                  s_done_ack_types, 1,
                                                  800, 3,
                                                  &ack, sizeof(ack), &reply_len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "CKPT: DONE_ACK 무응답(라운드 %d) — 판단 보류(Cntl 쪽 타임아웃에 맡김)", round + 1);
            return true;
        }
        esp_now_channelsync_notify_alive();  /* 방금 진짜 왕복 성공 — PING 큐잉 지연 오탐 완화(위 헤더 참고) */
        if (ack.missing_count == 0) {
            ESP_LOGI(TAG, "CKPT: DONE_ACK 완료 확인(라운드 %d)", round + 1);
            return true;
        }

        ESP_LOGI(TAG, "DONE_ACK: %u개 누락 — 재전송(라운드 %d/%d)", (unsigned)ack.missing_count, round + 1, s_nack_max_rounds);
        resend_chunks(file_id, &ack);
        if (s_request_generation != my_generation) return false;
    }
    ESP_LOGW(TAG, "CKPT: 재전송 라운드 소진");
    return true;  /* 라운드 소진 — 남은 판단은 Cntl 쪽 최종 타임아웃/에러 처리에 맡김 */
}

/* Selective Repeat 실험(2026-08-05) — send_one_photo()는 그대로 두고 나란히 추가(A/B 비교가
 * 목적이라 기존 경로 보존 필수). 파일을 SR_WINDOW_SIZE개씩 윈도우로 나눠서, 윈도우 하나 다
 * 보낼 때마다 그 범위 안에서 뭘 못 받았는지 물어보고(WINDOW_STATUS_REQUEST/ACK) 빠진 것만
 * 즉시 메꾼 뒤 다음 윈도우로 넘어감 — send_one_photo()처럼 "끝까지 다 쏘고 한 번에 확인"
 * 대신 전송 도중 계속 확인하는 게 차이점.
 * resend_chunks()가 이미 "임의의 missing_idx[] 목록만 골라 보낸다"는 범용 함수라 그대로
 * 재사용 — 윈도우의 "새로 보낼 청크들"도 그 인덱스 목록을 nack 구조체에 담아 넘기면 똑같이
 * 처리됨(신규 전송이든 재전송이든 "이 인덱스들 보내"는 동일한 동작이라 재사용 의미가 정확함).
 * 신뢰성 안전망(DONE/DONE_ACK/NACK라운드)은 기존과 완전히 동일하게 마지막에 유지 — 윈도우
 * 도중 놓친 게 있어도 마지막에 한 번 더 전체 확인. */
#define SR_WINDOW_SIZE 16          /* CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM 기본값(16)에 맞춤 —
                                       이 값이면 이론상 로컬 큐 포화(NO_MEM) 자체를 거의 안 만남 */
#define SR_STATUS_TIMEOUT_MS 400   /* DONE의 800ms보다 짧게 — 윈도우당 훨씬 자주 도니까 */
#define SR_STATUS_MAX_ATTEMPTS 3

static bool send_one_photo_sr(uint32_t file_id, uint32_t my_generation)
{
    ESP_LOGI(TAG, "CKPT(SR): 시작 file_id=%u", (unsigned)file_id);
    FILE *fp = NULL;
    uint32_t size = 0;
    if (cam_storage_open_read(file_id, &fp, &size) != ESP_OK) {
        ESP_LOGW(TAG, "SR: 파일 열기 실패: id=%u", (unsigned)file_id);
        return false;
    }
    uint16_t total_chunks = (uint16_t)((size + ESP_NOW_PHOTO_CHUNK_DATA_LEN - 1) / ESP_NOW_PHOTO_CHUNK_DATA_LEN);

    uint32_t crc = 0;
    {
        uint8_t crc_buf[256];
        size_t n;
        while ((n = fread(crc_buf, 1, sizeof(crc_buf), fp)) > 0) {
            crc = esp_rom_crc32_le(crc, crc_buf, n);
        }
    }
    fclose(fp);  /* 이후 실제 청크 전송은 resend_chunks()가 매번 자체적으로 열어서 읽음 */
    ESP_LOGI(TAG, "CKPT(SR): CRC 계산 완료 crc=%08x total_chunks=%u", (unsigned)crc, total_chunks);

    esp_now_photo_meta_t meta = {
        .version      = ESP_NOW_LINK_VERSION,
        .msg_type     = ESP_NOW_MSG_PHOTO_META,
        .file_id      = file_id,
        .total_size   = size,
        .total_chunks = total_chunks,
        .crc32        = crc,
    };
    /* 2026-08-21 — 예전엔 ACK 없이 그냥 3번 쏘고 바로 청크 전송을 시작했음(META만 reliable
     * 전환에서 빠져있었음). 이게 실제 버그의 근본원인이었음: 3개의 중복 사본 중 하나가 늦게
     * 도착하면 CNTL의 handle_meta()가 이미 SR로 받고 있던 진행상황(비트맵)을 그 시점에
     * 무조건 리셋해버려서, 청크 몇 개가 실제로는 잘 도착했는데도 맨 마지막 DONE 전체스캔에서
     * 뒤늦게 "누락"으로 잘못 잡히는 현상으로 나타남(실기 재현). ACK을 받을 때까지 청크 전송
     * 자체를 시작 안 하면 이 경쟁상태가 구조적으로 없어짐(사용자 지적: "SR 자체가 가드니까"
     * — 별도 중복방어는 불필요, META를 reliable로만 바꾸면 충분) */
    static const uint8_t s_meta_ack_types[] = { ESP_NOW_MSG_PHOTO_META_ACK };
    esp_err_t meta_err = esp_now_reliable_request(s_hub_mac, &meta, sizeof(meta),
                                                   s_meta_ack_types, 1,
                                                   800, 3,
                                                   NULL, 0, NULL);
    ESP_LOGI(TAG, "CKPT(SR): META_ACK: %s", esp_err_to_name(meta_err));
    if (meta_err != ESP_OK) {
        ESP_LOGW(TAG, "CKPT(SR): META 무응답 — 전송 포기(file_id=%u)", (unsigned)file_id);
        return false;
    }

    static esp_now_photo_chunk_nack_t range_req;  /* "새로 보낼 인덱스 목록"으로 재사용해
                                                       resend_chunks()에 그대로 넘김 */
    static esp_now_photo_chunk_nack_t status_ack;
    uint32_t total_sent_chunks = 0;  /* 통계용 — 재전송분 포함 */
    uint32_t status_requests   = 0;

    for (uint16_t window_base = 0; window_base < total_chunks; ) {
        if (s_request_generation != my_generation) {
            ESP_LOGI(TAG, "SR: 더 최신 요청으로 대체됨 — 중단(file_id=%u)", (unsigned)file_id);
            return false;
        }
        uint16_t window_count = total_chunks - window_base;
        if (window_count > SR_WINDOW_SIZE) window_count = SR_WINDOW_SIZE;

        range_req.file_id       = file_id;
        range_req.missing_count = window_count;
        for (uint16_t i = 0; i < window_count; i++) range_req.missing_idx[i] = window_base + i;
        resend_chunks(file_id, &range_req);
        total_sent_chunks += window_count;

        uint16_t window_end = window_base + window_count;

        esp_now_photo_window_status_req_t req = {
            .version     = ESP_NOW_LINK_VERSION,
            .msg_type    = ESP_NOW_MSG_PHOTO_WINDOW_STATUS_REQUEST,
            .file_id     = file_id,
            .range_start = window_base,
            .range_count = window_count,
        };
        static const uint8_t s_status_ack_types[] = { ESP_NOW_MSG_PHOTO_WINDOW_STATUS_ACK };
        size_t reply_len = 0;
        esp_err_t err = esp_now_reliable_request(s_hub_mac, &req, sizeof(req),
                                                  s_status_ack_types, 1,
                                                  SR_STATUS_TIMEOUT_MS, SR_STATUS_MAX_ATTEMPTS,
                                                  &status_ack, sizeof(status_ack), &reply_len);
        status_requests++;
        if (err == ESP_OK) {
            esp_now_channelsync_notify_alive();  /* 윈도우마다 자주 왕복 — DONE_ACK보다 더 촘촘한
                                                      생존 신호(위 헤더 설명 참고) */
            if (status_ack.missing_count > 0) {
                resend_chunks(file_id, &status_ack);  /* 이 윈도우 안의 누락분만 즉시 메꿈 */
                total_sent_chunks += status_ack.missing_count;
            }
        } else {
            ESP_LOGW(TAG, "SR: WINDOW_STATUS_ACK 무응답([%u,%u)) — 다음 윈도우로 진행(끝의 DONE/NACK가 안전망)",
                     window_base, window_end);
        }
        window_base = window_end;
    }
    ESP_LOGI(TAG, "CKPT(SR): 윈도우 루프 완료 — 총 전송청크(재전송포함)=%u/%u, 상태확인 %u회",
             (unsigned)total_sent_chunks, (unsigned)total_chunks, (unsigned)status_requests);

    /* 안전망 — 기존 DONE/DONE_ACK/NACK라운드 그대로(윈도우 도중 놓친 게 있어도 마지막에
     * 한 번 더 전체 확인) */
    esp_now_photo_done_t done = { .version = ESP_NOW_LINK_VERSION, .msg_type = ESP_NOW_MSG_PHOTO_DONE };
    static const uint8_t s_sr_done_ack_types[] = { ESP_NOW_MSG_PHOTO_DONE_ACK };
    static esp_now_photo_chunk_nack_t done_ack;

    for (int round = 0; round < s_nack_max_rounds; round++) {
        if (s_request_generation != my_generation) return false;

        size_t reply_len = 0;
        esp_err_t err = esp_now_reliable_request(s_hub_mac, &done, sizeof(done),
                                                  s_sr_done_ack_types, 1,
                                                  800, 3,
                                                  &done_ack, sizeof(done_ack), &reply_len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "CKPT(SR): DONE_ACK 무응답(라운드 %d) — 판단 보류", round + 1);
            return true;
        }
        esp_now_channelsync_notify_alive();
        if (done_ack.missing_count == 0) {
            ESP_LOGI(TAG, "CKPT(SR): DONE_ACK 완료 확인(라운드 %d)", round + 1);
            return true;
        }

        ESP_LOGI(TAG, "SR DONE_ACK: %u개 누락 — 재전송(라운드 %d/%d)", (unsigned)done_ack.missing_count, round + 1, s_nack_max_rounds);
        resend_chunks(file_id, &done_ack);
        if (s_request_generation != my_generation) return false;
    }
    ESP_LOGW(TAG, "CKPT(SR): 재전송 라운드 소진");
    return true;
}

/* 목록 요청 — 파일 내용 전송 없이 file_id/크기만 알려줌. 최대 500장 처리라
 * recv_cb(WiFi 태스크)에서 바로 안 하고 여기서 처리.
 * 2026-08-11 전면 재설계(사용자 지시) — 예전(2026-08-10 SR 방식: 파일당 1메시지 unreliable
 * 스트리밍 + LIST_DONE_ACK 누락 인덱스 재전송)도 실사용 중 3005/3007 동시발생으로 또 문제가
 * 발견됨(자동목록갱신은 항상 성공하는데 수동은 실패 — 조사 중 SD 스캔이 파일마다 stat()을
 * 두 번씩 하던 비효율과, 스트리밍이 애초에 unreliable이라 유실 자체가 사후에만 발견되는
 * 구조적 문제가 같이 드러남). 이번엔 "reliable로 보내면 사후 누락복구 자체가 필요 없다"는
 * 방향 — 순서: 개수 먼저(LIST_COUNT) -> 항목들을 배치로 묶어 reliable 전송(LIST_BATCH) ->
 * 완료(LIST_DONE). 개수 대조는 Cntl이 하고, 어긋나면 PHOTO_LIST_ERROR로 알려줌
 * (esp_now_photo.c 참고) — 받으면 s_list_abort_requested로 다음 배치 전에 중단 */
static void send_photo_list(void)
{
    static esp_now_photo_list_item_t items[CAM_STORAGE_MAX_FILES];  /* static — 500*15=7500B,
                                                                        스택 회피(기존 원칙) */
    int count = cam_storage_list_full(items, CAM_STORAGE_MAX_FILES);
    if (count < 0) count = 0;
    ESP_LOGI(TAG, "PHOTO_LIST_REQUEST -> %d개 항목(reliable, 배치 %d개씩)",
             count, ESP_NOW_PHOTO_LIST_BATCH_MAX);

    uint32_t sd_total_kb = 0, sd_used_kb = 0;
    cam_storage_get_sd_usage(&sd_total_kb, &sd_used_kb);  /* 실패해도 0/0으로 채워져서 그대로 보냄 */

    /* 1) 개수 먼저 — Cntl이 이후 배치 수신 중 진행률/최종 대조 기준으로 씀 */
    esp_now_photo_list_count_t count_msg = {
        .version = ESP_NOW_LINK_VERSION, .msg_type = ESP_NOW_MSG_PHOTO_LIST_COUNT,
        .count = (uint16_t)count, .sd_total_kb = sd_total_kb, .sd_used_kb = sd_used_kb,
    };
    static const uint8_t s_count_ack_types[] = { ESP_NOW_MSG_PHOTO_LIST_COUNT_ACK };
    static esp_now_photo_list_count_t count_ack;
    esp_err_t err = esp_now_reliable_request(s_hub_mac, &count_msg, sizeof(count_msg),
                                              s_count_ack_types, 1, 800, 3,
                                              &count_ack, sizeof(count_ack), NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PHOTO_LIST_COUNT_ACK 무응답 — 목록 전송 포기");
        return;
    }
    esp_now_channelsync_notify_alive();

    /* 2) 항목들을 배치로 나눠서 reliable 전송 */
    static const uint8_t s_batch_ack_types[] = { ESP_NOW_MSG_PHOTO_LIST_BATCH_ACK };
    static esp_now_photo_list_batch_t batch;      /* static — 963B, 스택 회피 */
    static esp_now_photo_list_batch_t batch_ack;
    int sent = 0;
    while (sent < count && (s_conn_state == CAM_CONN_PAIRED) && !s_list_abort_requested) {
        int n = count - sent;
        if (n > ESP_NOW_PHOTO_LIST_BATCH_MAX) n = ESP_NOW_PHOTO_LIST_BATCH_MAX;
        batch.version     = ESP_NOW_LINK_VERSION;
        batch.msg_type    = ESP_NOW_MSG_PHOTO_LIST_BATCH;
        batch.entry_count = (uint8_t)n;
        memcpy(batch.entries, &items[sent], (size_t)n * sizeof(esp_now_photo_list_item_t));
        size_t send_len = 3 + (size_t)n * sizeof(esp_now_photo_list_item_t);  /* 채운 만큼만
                                                                                   보냄(대역폭 절약) */
        err = esp_now_reliable_request(s_hub_mac, &batch, send_len,
                                        s_batch_ack_types, 1, 800, 3,
                                        &batch_ack, sizeof(batch_ack), NULL);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "PHOTO_LIST_BATCH_ACK 무응답(%d/%d 전송됨) — 목록 전송 포기", sent, count);
            return;
        }
        sent += n;
        esp_now_channelsync_notify_alive();
    }
    if (s_list_abort_requested) {
        ESP_LOGW(TAG, "PHOTO_LIST_ERROR 수신으로 목록 전송 중단(%d/%d)", sent, count);
        s_list_abort_requested = false;
        return;
    }
    if (!(s_conn_state == CAM_CONN_PAIRED)) return;

    /* 3) 완료 신호 — 더 이상 개수/누락 정보 안 실음(개수는 이미 1번에서 옴, 배치 자체가
     * reliable이라 누락 추적 자체가 불필요) */
    esp_now_photo_list_done_t done = { .version = ESP_NOW_LINK_VERSION, .msg_type = ESP_NOW_MSG_PHOTO_LIST_DONE };
    static const uint8_t s_done_ack_types[] = { ESP_NOW_MSG_PHOTO_LIST_DONE_ACK };
    static esp_now_photo_list_done_t done_ack;
    err = esp_now_reliable_request(s_hub_mac, &done, sizeof(done),
                                    s_done_ack_types, 1, 800, 3,
                                    &done_ack, sizeof(done_ack), NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PHOTO_LIST_DONE_ACK 무응답 — Cntl 쪽 판단(개수 대조 타임아웃)에 맡김");
        return;
    }
    ESP_LOGI(TAG, "목록 전송 완료(%d개, SD %u/%uKB)", sent, (unsigned)sd_used_kb, (unsigned)sd_total_kb);
}

/* 처리량 벤치마크(2026-08-04) — 프로토콜 신뢰성 레이어를 만들기 전에, 지금 이 채널
 * (STA-공유채널이든 격리-AP든)에서 순수하게 뽑을 수 있는 최대 처리량이 얼마인지 기준치를
 * 먼저 재둠. 청크와 같은 크기의 더미 바이트를 큐가 허용하는 한 최대 속도로 계속 쏘고,
 * ESP_ERR_ESPNOW_NO_MEM(로컬 송신큐 포화)만 잠깐 기다렸다 재시도 — 그 외 실패는 실패로
 * 세고 다음 것으로 넘어감(재전송 안 함, 신뢰성 측정이 아니라 처리량 측정이 목적).
 *
 * 1시간 연속 실행 지원(2026-08-04, 사용자 요청: 에러율/대역폭 추이를 오래 관찰하고 싶다) —
 * 매초 로그를 남기면 1시간에 3600줄이라 너무 많음. BENCH_LOG_INTERVAL_US마다 그 구간만의
 * 집계(구간 처리량 + 구간 오류율)를 찍고 리셋 — 전체 누적치는 함수 끝의 최종 요약 한 줄로.
 * NO_MEM 재시도는 "진짜 실패"가 아니라 로컬 큐가 잠깐 찬 것뿐이라 fail_count와 분리 집계 —
 * 섞으면 오류율이 실제보다 훨씬 나빠 보임(로컬 큐 포화는 무선 유실이 아님, 위 함수 설명 참고) */
#define BENCH_LOG_INTERVAL_US (30 * 1000 * 1000)

static void run_bench_blast(uint16_t duration_sec)
{
    ESP_LOGI(TAG, "BENCH: %u초간 최대 속도 전송 시작", duration_sec);

    static esp_now_bench_blast_t blast;  /* static — 1200+바이트를 태스크 스택에 두지 않음 */
    blast.version  = ESP_NOW_LINK_VERSION;
    blast.msg_type = ESP_NOW_MSG_BENCH_BLAST;
    blast.seq      = 0;
    memset(blast.data, 0xAA, sizeof(blast.data));

    int64_t start_us = esp_timer_get_time();
    int64_t end_us   = start_us + (int64_t)duration_sec * 1000000LL;

    uint32_t ok_count = 0, fail_count = 0, nomem_retry_count = 0;
    uint64_t bytes_sent = 0;
    uint32_t win_ok = 0, win_fail = 0, win_nomem = 0;
    uint64_t win_bytes = 0;
    int64_t  win_start_us = start_us;
    int64_t  next_log_us  = start_us + BENCH_LOG_INTERVAL_US;

    while (esp_timer_get_time() < end_us) {
        esp_err_t err = esp_now_send(s_hub_mac, (const uint8_t *)&blast, sizeof(blast));
        if (err == ESP_ERR_ESPNOW_NO_MEM) {
            nomem_retry_count++;
            win_nomem++;
            /* pdMS_TO_TICKS(5)는 CONFIG_FREERTOS_HZ=100(틱당 10ms)에서 정수 나눗셈으로
             * 0틱이 됨 — vTaskDelay(0)은 사실상 지연 없이 즉시 재시도라 큐가 계속 찬 상태에서
             * photo_tx가 IDLE 태스크를 굶겨 task watchdog가 반복 트리거됨(2026-08-04, 1시간
             * 실기 로그로 확인, 26회 발생). 최소 1틱(10ms)을 보장하도록 수정 */
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;  /* 큐 포화 — 같은 seq로 재시도(측정 왜곡 방지, 성공한 것만 카운트) */
        }
        if (err == ESP_OK) {
            ok_count++;
            win_ok++;
            bytes_sent += sizeof(blast);
            win_bytes += sizeof(blast);
        } else {
            fail_count++;
            win_fail++;
        }
        blast.seq++;

        int64_t now_us = esp_timer_get_time();
        if (now_us >= next_log_us) {
            double win_sec = (now_us - win_start_us) / 1e6;
            uint32_t win_total = win_ok + win_fail;
            double win_err_rate = win_total > 0 ? (100.0 * win_fail / win_total) : 0.0;
            ESP_LOGI(TAG, "BENCH 중간집계(%.0fs 경과): %.1fKB/s, 성공%u/실패%u(오류율%.2f%%), NO_MEM재시도%u회",
                     (now_us - start_us) / 1e6,
                     win_sec > 0 ? (win_bytes / 1024.0) / win_sec : 0.0,
                     (unsigned)win_ok, (unsigned)win_fail, win_err_rate, (unsigned)win_nomem);
            win_ok = 0; win_fail = 0; win_nomem = 0; win_bytes = 0;
            win_start_us = now_us;
            next_log_us  = now_us + BENCH_LOG_INTERVAL_US;
        }
    }
    double sec = (double)duration_sec;
    uint32_t total = ok_count + fail_count;
    double err_rate = total > 0 ? (100.0 * fail_count / total) : 0.0;
    ESP_LOGI(TAG, "BENCH 완료: 성공 %u개(평균 %.1fKB/s), 실패 %u개(오류율 %.2f%%), NO_MEM재시도 %u회, 총 %llu바이트",
             (unsigned)ok_count, sec > 0 ? (bytes_sent / 1024.0) / sec : 0.0,
             (unsigned)fail_count, err_rate, (unsigned)nomem_retry_count, (unsigned long long)bytes_sent);
}

/* 사진전송 방식 벤치마크(2026-08-05, Selective Repeat 실험) — 위 run_bench_blast()는 프로토콜
 * 오버헤드 없는 순수 채널 처리량만 재는데, 이건 그 반대로 실제 META/CHUNK/DONE(+SR이면
 * WINDOW_STATUS) 왕복까지 전부 포함해서 "진짜 사진 한 장 보내는 데 얼마나 걸리는지"를
 * duration_sec 동안 반복 측정 — send_one_photo() vs send_one_photo_sr() 실측 비교가 목적.
 * CAM의 최근 촬영 사진을 반복 대상으로 삼음. Cntl 쪽은 이 벤치마크 때문에 바뀐 게 하나도
 * 없음 — 기존 handle_meta()가 무조건 재초기화라 CAM이 미는 대로(Cntl이 매번 요청 안 해도)
 * 그냥 정상 수신됨(확인함) */
static void run_transfer_bench(esp_now_bench_mode_t mode, uint16_t duration_sec)
{
    uint32_t ids[1];
    int count = cam_storage_list(PHOTO_REQUEST_MODE_LATEST, 0, ids, 1);
    if (count < 1) {
        ESP_LOGW(TAG, "XFER_BENCH: 저장된 사진이 없음 — 중단");
        return;
    }
    uint32_t file_id = ids[0];
    ESP_LOGI(TAG, "XFER_BENCH: mode=%d file_id=%u %u초간 반복 전송 시작", mode, (unsigned)file_id, duration_sec);

    int64_t start_us    = esp_timer_get_time();
    int64_t end_us       = start_us + (int64_t)duration_sec * 1000000LL;
    int64_t next_log_us = start_us + BENCH_LOG_INTERVAL_US;

    uint32_t xfer_count = 0, xfer_fail = 0;
    uint64_t total_elapsed_ms = 0, min_elapsed_ms = UINT64_MAX, max_elapsed_ms = 0;
    uint32_t win_xfer_count = 0, win_fail = 0;
    uint64_t win_elapsed_ms = 0;

    while (esp_timer_get_time() < end_us) {
        int64_t t0 = esp_timer_get_time();
        bool ok = (mode == ESP_NOW_BENCH_MODE_XFER_SR)
                      ? send_one_photo_sr(file_id, s_request_generation)
                      : send_one_photo(file_id, s_request_generation);
        uint64_t elapsed_ms = (uint64_t)((esp_timer_get_time() - t0) / 1000);

        if (ok) {
            xfer_count++; win_xfer_count++;
            total_elapsed_ms += elapsed_ms; win_elapsed_ms += elapsed_ms;
            if (elapsed_ms < min_elapsed_ms) min_elapsed_ms = elapsed_ms;
            if (elapsed_ms > max_elapsed_ms) max_elapsed_ms = elapsed_ms;
        } else {
            xfer_fail++; win_fail++;
        }

        int64_t now_us = esp_timer_get_time();
        if (now_us >= next_log_us) {
            ESP_LOGI(TAG, "XFER_BENCH(mode=%d) 중간집계(%.0fs 경과): 완료 %u건(실패 %u), 평균 %.0fms/건",
                     mode, (now_us - start_us) / 1e6, (unsigned)win_xfer_count, (unsigned)win_fail,
                     win_xfer_count > 0 ? (double)win_elapsed_ms / win_xfer_count : 0.0);
            win_xfer_count = 0; win_fail = 0; win_elapsed_ms = 0;
            next_log_us = now_us + BENCH_LOG_INTERVAL_US;
        }
    }

    ESP_LOGI(TAG, "XFER_BENCH(mode=%d) 완료: 총 %u건(실패 %u), 평균 %.0fms/건, 최소 %llums, 최대 %llums",
             mode, (unsigned)xfer_count, (unsigned)xfer_fail,
             xfer_count > 0 ? (double)total_elapsed_ms / xfer_count : 0.0,
             (unsigned long long)(xfer_count > 0 ? min_elapsed_ms : 0), (unsigned long long)max_elapsed_ms);
}

/* 딥슬립 웨이크 윈도우 판정(cam_node.c의 cam_node_wake_window_done())이 "지금 자도 되는지"
 * 확인할 때 이 큐가 처리 중인지 알아야 함(2026-08-10) — Light Sleep 시절의 esp_pm_lock
 * (cam_node_sleep_lock_*)이 하던 "바쁜 구간" 표시를 대체. 처리 종류가 여럿이라 매
 * return/continue 지점마다 짝을 맞추는 대신, 이번 반복 시작에 세우고 끝에 내림 */
static volatile bool s_transfer_busy = false;

bool esp_now_cam_is_busy(void)
{
    return s_transfer_busy || (s_photo_request_queue && uxQueueMessagesWaiting(s_photo_request_queue) > 0);
}

/* 2026-08-23 — busy 해제 지점마다 cam_node.c의 이벤트드리븐 대기 루프를 깨움(CAML에서
 * 검증 후 이식). 기존에 여러 return/continue 지점마다 s_transfer_busy=false만 하던 걸
 * 이 헬퍼로 통일해서 신호까지 같이 나가게 함 */
static void mark_transfer_idle(void)
{
    s_transfer_busy = false;
    cam_node_signal_recheck();
}

/* 2026-08-21 — 지금촬영 핸드셰이크 재설계용 공용 헬퍼. cam_capture_status_t의 어떤 값이든
 * CAPTURE_STATUS_ACK을 기다리는 reliable로 보냄(feedback_default_to_reliable_messaging
 * 메모리 참고) — RECEIVED만 recv_cb 컨텍스트라 예외(그쪽은 여전히 fire-and-forget) */
static void send_capture_status(uint8_t status)
{
    esp_now_capture_status_t msg = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_CAPTURE_STATUS,
        .status   = status,
    };
    static const uint8_t s_capture_ack_types[] = { ESP_NOW_MSG_CAPTURE_STATUS_ACK };
    esp_err_t err = esp_now_reliable_request(s_hub_mac, &msg, sizeof(msg),
                                              s_capture_ack_types, 1,
                                              800, 3,
                                              NULL, 0, NULL);
    if (err == ESP_OK) esp_now_channelsync_notify_alive();
    ESP_LOGI(TAG, "CAPTURE_STATUS(%u) 전송: %s", (unsigned)status, esp_err_to_name(err));
}

static void photo_transfer_task(void *arg)
{
    (void)arg;
    cam_task_request_t item;
    for (;;) {
        if (xQueueReceive(s_photo_request_queue, &item, portMAX_DELAY) != pdTRUE) continue;
        s_transfer_busy = true;

        if (item.kind == CAM_TASK_REQ_BENCH) {
            if ((s_conn_state == CAM_CONN_PAIRED)) {
                if (item.bench_mode == ESP_NOW_BENCH_MODE_RAW_BLAST) {
                    run_bench_blast(item.bench_duration_sec);
                } else {
                    run_transfer_bench((esp_now_bench_mode_t)item.bench_mode, item.bench_duration_sec);
                }
            }
            mark_transfer_idle();
            continue;
        }

        if (item.kind == CAM_TASK_REQ_LIST) {
            /* 2026-08-04 실기로 확인된 레이스: 큐에서 뽑히자마자(=send_photo_list() 시작
             * 전에) 플래그를 지우면, 그 함수가 아직 도는 중(파일 여러 개면 수십~수백ms)에
             * 3번 반복 전송의 2/3번째 요청이 도착해서 "새 요청"으로 오인 — 실제로 목록을
             * 두 번 보냈음(로그 확인). send_photo_list()가 끝난 뒤에 해제해야 그 시간
             * 동안 들어오는 중복을 계속 걸러낼 수 있음 */
            if ((s_conn_state == CAM_CONN_PAIRED)) send_photo_list();
            s_list_request_pending = false;
            mark_transfer_idle();
            continue;
        }
        if (!(s_conn_state == CAM_CONN_PAIRED)) {
            mark_transfer_idle();
            continue;
        }

        if (item.kind == CAM_TASK_REQ_DELETE_ALL) {
            /* 2026-08-21 — 실제 삭제(파일마다 순차 unlink, 개수 많으면 수십 초) 시작 "전"에
             * 지울 개수를 먼저 알림(지금촬영의 RECEIVED와 동일 원칙) — Cntl이 이걸로 완료
             * 대기 예산을 계산함. 지능 없이 그냥 사실 보고만 하는 것 — 실패해도(count<0)
             * 0으로 보내고 그대로 진행(뒤이은 delete_all 결과가 최종 판정) */
            int to_delete = cam_storage_count_files();
            esp_now_photo_delete_all_received_t received = {
                .version  = ESP_NOW_LINK_VERSION,
                .msg_type = ESP_NOW_MSG_PHOTO_DELETE_ALL_RECEIVED,
                .count    = (uint16_t)(to_delete >= 0 ? to_delete : 0),
            };
            esp_err_t recv_err = esp_now_send(s_hub_mac, (const uint8_t *)&received, sizeof(received));
            ESP_LOGI(TAG, "PHOTO_DELETE_ALL_RECEIVED(%d개) 전송: %s", to_delete, esp_err_to_name(recv_err));

            int deleted = cam_storage_delete_all();
            esp_now_photo_delete_all_ack_t ack = {
                .version       = ESP_NOW_LINK_VERSION,
                .msg_type      = ESP_NOW_MSG_PHOTO_DELETE_ALL_ACK,
                .success       = (deleted >= 0) ? 1 : 0,
                .deleted_count = (uint16_t)(deleted >= 0 ? deleted : 0),
            };
            esp_err_t err = esp_now_send(s_hub_mac, (const uint8_t *)&ack, sizeof(ack));
            ESP_LOGI(TAG, "PHOTO_DELETE_ALL_ACK(성공=%d, %d개) 전송: %s", ack.success, deleted, esp_err_to_name(err));
            mark_transfer_idle();
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

            /* 2026-08-21 핸드셰이크 재설계(사용자 설계) — RECEIVED 이후 아무 중간신호 없이
             * 촬영이 끝날 때까지 블로킹 대기하던 걸(4004 오탐의 근본원인) 단계별로 나눔.
             * 카메라 초기화가 필요 없으면 INIT_NEEDED/INIT_DONE 두 단계는 아예 안 보내고
             * 건너뜀 — Cntl 팝업도 그 두 단계를 안 보여줌(esp_now_link.h 주석 참고) */
            bool needs_init = !cam_node_is_camera_ready();
            if (needs_init) {
                send_capture_status(CAM_CAPTURE_STATUS_INIT_NEEDED);
                bool init_ok = cam_node_ensure_camera_ready();
                ESP_LOGI(TAG, "CAPTURE_NOW: 카메라 초기화 %s", init_ok ? "완료" : "실패");
                if (!init_ok) {
                    send_capture_status(CAM_CAPTURE_STATUS_FAILED);
                    mark_transfer_idle();
                    continue;
                }
                send_capture_status(CAM_CAPTURE_STATUS_INIT_DONE);
            }

            send_capture_status(CAM_CAPTURE_STATUS_CAPTURING);
            bool captured = cam_node_capture_now();
            ESP_LOGI(TAG, "CAPTURE_NOW 촬영 결과: %s", captured ? "성공" : "실패");
            /* 2026-08-05 Layer 1 재설계 — CAPTURE_STATUS_ACK를 기다리는 reliable_request로
             * 교체(3번 수동 반복 대신 레이어가 재시도). 이 최종 결과가 안 가면 Cntl은
             * 지금촬영이 끝났는지 몰라서 UI_ERR_CAPTURE_NORESPONSE(4004)로 빠짐(실기 확인) */
            send_capture_status(captured ? CAM_CAPTURE_STATUS_SUCCESS : CAM_CAPTURE_STATUS_FAILED);
            mark_transfer_idle();
            continue;
        }

        uint32_t ids[CAM_STORAGE_MAX_FILES];
        int count = cam_storage_list((photo_request_mode_t)req.mode, req.param, ids, CAM_STORAGE_MAX_FILES);
        ESP_LOGI(TAG, "PHOTO_REQUEST mode=%d param=%u -> %d장 전송 시작", req.mode, (unsigned)req.param, count);

        /* send_one_photo_sr가 이제 파일당 자기 DONE(+NACK 재전송 라운드)을 스스로 끝까지
         * 책임짐(2026-08-03 재설계) — 예전엔 여기서 전체 배치가 끝난 뒤 DONE을 한 번 더
         * 보냈는데, 이제 그러면 방금 send_one_photo_sr가 이미 마친 완료-확인 사이클 위에
         * 불필요한 DONE이 하나 더 얹혀서 혼란만 더함.
         * 2026-08-05 — 실기 30분 BMT 실측 비교 후 send_one_photo(블라스트+끝에 NACK)에서
         * send_one_photo_sr(윈도우+주기적 확인)로 교체. 처리량은 비슷했지만(SR이 약 6.5% 더
         * 많이 처리, 1696건 vs 1593건/30분) 결정적 차이는 꼬리 지연시간 — 현재 방식은 최대
         * 8840ms(평균의 8배)까지 튄 반면 SR은 최악의 경우도 1166ms로 평균과 거의 차이 없었음
         * (project_cntl_cam_esp_now_reliability_layers 메모리 참고). send_one_photo 자체는
         * 안 지움 — BENCH_START mode=1(XFER_BENCH 현재방식)로 계속 비교용으로 남겨둠 */
        for (int i = 0; i < count && (s_conn_state == CAM_CONN_PAIRED); i++) {
            if (s_request_generation != item.generation) break;  /* 더 최신 요청으로 대체됨 */
            send_one_photo_sr(ids[i], item.generation);
        }
        mark_transfer_idle();
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
    /* MAC 뒤 3바이트(제조사가 기기마다 실제로 다르게 부여하는 유니크 구간, 24비트)를 전부
     * 사용 — 2바이트(16비트)만 쓰면 이론적으로 충돌 가능성이 있고, 딱히 2바이트로 줄일
     * 이유도 없었음(이름 길이 여유 충분, ESP_NOW_LINK_NAME_LEN=16)(2026-08-05, 사용자 지적) */
    /* 2026-08-22 — 전력로그 한 줄이 화면폭을 넘겨서 "..."로 잘리는 문제(사용자 지적) —
     * "Cam-" 4글자를 "C" 1글자로 줄임(사용자 지시). classify_name()도 같이 맞춰야 함
     * (esp_now_hub.c) */
    snprintf(s_name, sizeof(s_name), "C%02X%02X%02X", s_mac[3], s_mac[4], s_mac[5]);
}

static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len < 2) return;
    uint8_t msg_type = data[1];

    /* ADVERTISE_ACK/CHANNEL_PONG 소비 — 그 외 타입은 조용히 무시하고 리턴하므로 아래 기존
     * dispatch와 안전하게 병행됨(2026-08-04, esp_now_channelsync.h 참고) */
    esp_now_channelsync_on_recv(info, msg_type, data, len);
    /* Reliable 모드(Layer 1) 요청/응답 매칭 — photo_tx 태스크가 기다리는 응답이면 깨움
     * (2026-08-05, esp_now_reliable.h 참고, 위와 동일한 병행 안전성) */
    esp_now_reliable_on_recv(msg_type, info ? info->src_addr : NULL, data, len);

    if (msg_type == ESP_NOW_MSG_PHOTO_REQUEST) {
        if (!(s_conn_state == CAM_CONN_PAIRED) || len < (int)sizeof(esp_now_photo_request_t)) return;
        cam_node_note_activity();
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
            cam_task_request_t item = { .kind = CAM_TASK_REQ_PHOTO, .photo_req = req, .generation = s_request_generation };
            if (xQueueSend(s_photo_request_queue, &item, 0) != pdTRUE) {
                ESP_LOGW(TAG, "PHOTO_REQUEST 큐 가득 — 이전 전송 아직 진행중, 무시");
            }
        } else {
            /* 2026-08-04 — 예전엔 세대번호만 안 올리고 큐에는 그대로 다시 넣었음. ESP-NOW
             * 물리계층 자동 재전송으로 "같은" 요청이 두 번 들어오는 경우뿐 아니라, Cntl이
             * 신뢰성을 위해 같은 요청을 의도적으로 여러 번 보내는 경우(아래 esp_now_photo.c
             * 참고)에도 그대로 큐에 쌓여서 같은 사진/배치를 몇 번씩 중복 전송하고 있었음 —
             * 진짜 새 요청이 아니면 아예 무시 */
            ESP_LOGI(TAG, "PHOTO_REQUEST 중복 수신(mode=%d param=%u) — 무시", req.mode, (unsigned)req.param);
        }
        return;
    }

    if (msg_type == ESP_NOW_MSG_BENCH_START) {
        if (!(s_conn_state == CAM_CONN_PAIRED) || len < (int)sizeof(esp_now_bench_start_t)) return;
        cam_node_note_activity();
        esp_now_bench_start_t req;
        memcpy(&req, data, sizeof(req));
        cam_task_request_t item = { .kind = CAM_TASK_REQ_BENCH, .bench_duration_sec = req.duration_sec,
                                     .bench_mode = req.mode };
        if (xQueueSend(s_photo_request_queue, &item, 0) != pdTRUE) {
            ESP_LOGW(TAG, "BENCH_START 큐 가득 — 무시");
        }
        return;
    }

    if (msg_type == ESP_NOW_MSG_PHOTO_LIST_REQUEST) {
        if (!(s_conn_state == CAM_CONN_PAIRED) || len < (int)sizeof(esp_now_photo_list_request_t)) return;
        /* 2026-08-04 — Cntl이 신뢰성을 위해 LIST_REQUEST를 여러 번 보내면(아래 esp_now_photo.c
         * 참고), 예전엔 그때마다 큐에 다시 쌓여서 SD 목록조회(최대 500개 파일 stat)를
         * 몇 번씩 중복 실행했음. 이미 대기/진행 중이면 추가로 안 쌓음 — PHOTO_REQUEST의
         * 중복 무시와 같은 이유 */
        if (s_list_request_pending) {
            ESP_LOGI(TAG, "PHOTO_LIST_REQUEST 중복 수신 — 무시(이미 처리 중)");
            return;
        }
        cam_node_note_activity();
        cam_task_request_t item = { .kind = CAM_TASK_REQ_LIST };
        if (xQueueSend(s_photo_request_queue, &item, 0) != pdTRUE) {
            ESP_LOGW(TAG, "PHOTO_LIST_REQUEST 큐 가득 — 무시");
        } else {
            s_list_request_pending = true;
        }
        return;
    }

    if (msg_type == ESP_NOW_MSG_PHOTO_DELETE_ALL_REQUEST) {
        if (!(s_conn_state == CAM_CONN_PAIRED) || len < (int)sizeof(esp_now_photo_delete_all_request_t)) return;
        cam_node_note_activity();
        cam_task_request_t item = { .kind = CAM_TASK_REQ_DELETE_ALL };
        if (xQueueSend(s_photo_request_queue, &item, 0) != pdTRUE) {
            ESP_LOGW(TAG, "PHOTO_DELETE_ALL_REQUEST 큐 가득 — 무시");
        }
        return;
    }

    if (msg_type == ESP_NOW_MSG_PHOTO_LIST_ERROR) {
        /* 2026-08-11 — Cntl이 목록 수신 중 개수 불일치를 판정하면 보냄(esp_now_photo.c 참고).
         * send_photo_list()가 다음 배치 전송 전에 이 플래그를 확인하고 중단함(위 함수 참고).
         * 별도 상태/캐시를 갖고 있지 않은 설계(배치가 reliable이라 재조회 캐시가 애초에
         * 불필요)라 여기서 더 정리할 메모리는 없음 — 플래그 세팅 + ACK 회신이 전부 */
        if (!(s_conn_state == CAM_CONN_PAIRED) || len < (int)sizeof(esp_now_photo_list_done_t)) return;
        s_list_abort_requested = true;
        ESP_LOGW(TAG, "PHOTO_LIST_ERROR 수신 — 목록 전송 중단 요청됨");
        esp_now_photo_list_done_t ack = { .version = ESP_NOW_LINK_VERSION, .msg_type = ESP_NOW_MSG_PHOTO_LIST_ERROR_ACK };
        esp_now_send(s_hub_mac, (const uint8_t *)&ack, sizeof(ack));
        return;
    }

    if (msg_type == ESP_NOW_MSG_CAM_CONFIG_SET) {
        /* 2026-08-08 — 촬영주기(배터리/SD)+응답성(연결성/절전) 원격 설정. recv_cb(ESP-NOW
         * 드라이버 태스크)에서 바로 처리 — SD 파일 쓰기 하나뿐이라 photo_transfer_task
         * 큐로 넘길 만큼 무겁지 않음(다른 핸들러들과 동일 판단, 예: PHOTO_DELETE_REQUEST) */
        if (!(s_conn_state == CAM_CONN_PAIRED) || len < (int)sizeof(esp_now_cam_config_t)) return;
        cam_node_note_activity();  /* 2026-08-10 — 페어링 직후 항상 오는 메시지, 이걸 처리하는
                                       동안은 곧바로 딥슬립에 들어가지 않게 유휴타이머 갱신 */
        esp_now_cam_config_t cfg;
        memcpy(&cfg, data, sizeof(cfg));
        cam_node_set_capture_interval_sec(cfg.capture_interval_sec);
        cam_node_set_response_interval_sec(cfg.response_interval_sec);
        restart_stats_timer(cfg.response_interval_sec);
        cam_node_set_agc_enable(cfg.agc_enable != 0);
        cam_node_set_aec_enable(cfg.aec_enable != 0);
        if (cfg.xclk_mhz != 0) cam_node_set_xclk_target_mhz(cfg.xclk_mhz);
        if (cfg.nack_max_rounds != 0) s_nack_max_rounds = cfg.nack_max_rounds;
        if (cfg.wb_mode < CAM_WB_MODE_COUNT) {
            /* 화이트밸런스는 기존 필드라 그대로 적용 — 센서 API가 cam_node.c에 없어서 여기서
             * 직접 처리(다른 촬영 파라미터 setter들과 달리 이건 esp_camera 센서 핸들을
             * cam_node.c 밖으로 안 내보내서, sensor_t 직접 접근은 그쪽에만 있음 — 지금은
             * wb_mode 저장/전달만 하고 실제 센서 적용은 TODO로 남김, 촬영주기/응답성이 이번
             * 세션의 핵심 스코프) */
            ESP_LOGI(TAG, "CAM_CONFIG_SET: wb_mode=%u(적용 TODO) capture=%us response=%us",
                     cfg.wb_mode, (unsigned)cfg.capture_interval_sec, (unsigned)cfg.response_interval_sec);
        }
        esp_now_cam_config_ack_t ack = {
            .version  = ESP_NOW_LINK_VERSION,
            .msg_type = ESP_NOW_MSG_CAM_CONFIG_ACK,
            .success  = 1,
        };
        esp_err_t err = esp_now_send(s_hub_mac, (const uint8_t *)&ack, sizeof(ack));
        ESP_LOGI(TAG, "CAM_CONFIG_ACK 전송: %s", esp_err_to_name(err));
        /* 2026-08-10 — 딥슬립 통계는 여기(설정 반영 직후)에서 보내야 정확함. 예전엔 페어링
         * 직후(recv_cb의 PAIR_REQUEST 핸들러)에 바로 보냈는데, 그 시점엔 아직 CAM_CONFIG_SET을
         * 못 받아서 s_response_interval_sec이 Kconfig 기본값(3초)인 채로 보고됨 — 실제로는
         * 딴 값(예: 10초)으로 곧 재설정될 예정인데도 통계탭엔 옛 값이 찍히는 버그가 실사용
         * 중 발견됨("응답성을 10초로 적용했는데 I=3으로 계속 나옴") */
        send_deep_sleep_stats();
        return;
    }

    if (msg_type == ESP_NOW_MSG_PHOTO_DELETE_REQUEST) {
        if (!(s_conn_state == CAM_CONN_PAIRED) || len < (int)sizeof(esp_now_photo_delete_request_t)) return;
        cam_node_note_activity();
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

    /* Cntl이 연결 해제했다는 통보 — 페어링만 정리(2026-08-04 — 채널 동기화는 그대로 유지,
     * esp_now_channelsync가 독립적으로 계속 확인 중이라 다시 스캔할 필요 없음. 예전엔
     * enter_advertising()으로 채널 스캔부터 다시 했는데, 채널이 안 바뀐 이상 불필요한
     * 낭비였음) */
    if (msg_type == ESP_NOW_MSG_UNPAIR) {
        if (!(s_conn_state == CAM_CONN_PAIRED) || len < (int)sizeof(esp_now_unpair_t)) return;
        if (memcmp(info->src_addr, s_hub_mac, sizeof(s_hub_mac)) != 0) return;
        ESP_LOGI(TAG, "Cntl이 연결 해제함");
        s_conn_state = CAM_CONN_ORPHAN;
        ESP_LOGI(TAG, "[STATE] -> %s (unpair)", conn_state_name(s_conn_state));
        set_led(LED_PATTERN_BLINK_FAST);
        return;
    }

    /* Cntl 소프트리셋 대응(2026-08-02) — Cntl이 재시작하면 노드 테이블이 통째로 비워지는데,
     * 우리는 이미 페어링됐다고 믿고 있음. 2026-08-04 재설계: 생존확인이 이제 CHANNEL_PING/PONG
     * 기반이라(esp_now_channelsync) Cntl이 재부팅해도 응답만 하면 되므로 채널 동기화 자체는
     * 안 끊김 — 여기선 페어링 상태만 정리하면 됨(재스캔 불필요) */
    if (msg_type == ESP_NOW_MSG_HUB_RESET) {
        if (len < (int)sizeof(esp_now_hub_reset_t)) return;
        if (s_conn_state == CAM_CONN_PAIRED) {
            ESP_LOGI(TAG, "Cntl 재시작 감지(HUB_RESET)");
            s_conn_state = CAM_CONN_ORPHAN;
            ESP_LOGI(TAG, "[STATE] -> %s (hub_reset)", conn_state_name(s_conn_state));
            set_led(LED_PATTERN_BLINK_FAST);
        }
        return;
    }

    /* 적응형 반응시간(2026-08-10) — Cntl이 "이번 사이클에 더 할 일 없다"고 판단하면 보냄.
     * 남은 유휴여유 타이머를 기다리지 않고 곧바로 잘 수 있게 함(cam_node.c 참고) */
    if (msg_type == ESP_NOW_MSG_SLEEP_NOW) {
        if (!(s_conn_state == CAM_CONN_PAIRED) || len < (int)sizeof(esp_now_sleep_now_t)) return;
        ESP_LOGI(TAG, "SLEEP_NOW 수신 — 유휴여유 생략하고 즉시 딥슬립 준비");
        cam_node_note_sleep_now_requested();
        /* 2026-08-10 — reliable stack 전환("chunk는 SR, 나머지는 reliable" 원칙 적용).
         * esp_now_sleep_now_t를 msg_type만 바꿔 그대로 재사용(기존 관례) */
        esp_now_sleep_now_t ack = { .version = ESP_NOW_LINK_VERSION, .msg_type = ESP_NOW_MSG_SLEEP_NOW_ACK };
        esp_err_t ack_err = esp_now_send(s_hub_mac, (const uint8_t *)&ack, sizeof(ack));
        ESP_LOGI(TAG, "SLEEP_NOW_ACK 전송: %s", esp_err_to_name(ack_err));
        return;
    }

    /* CAM은 자체 RTC가 없어서 부팅하면 시계가 1970-01-01 근처 — Cntl이 페어링될 때마다
     * 자기 시각을 알려주면 그걸로 시스템 클록을 맞춤(사진 file_id가 이 시각 기준이라
     * 정확한 촬영시각 표시에 필요, 2026-08-01 추가) */
    if (msg_type == ESP_NOW_MSG_SET_TIME) {
        if (!(s_conn_state == CAM_CONN_PAIRED) || len < (int)sizeof(esp_now_set_time_t)) return;
        if (memcmp(info->src_addr, s_hub_mac, sizeof(s_hub_mac)) != 0) return;
        const esp_now_set_time_t *msg = (const esp_now_set_time_t *)data;
        struct timeval tv = { .tv_sec = (time_t)msg->unix_time, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "SET_TIME 수신 — 시각 동기화: %u", (unsigned)msg->unix_time);
        /* 2026-08-21 — reliable stack 전환(다른 모든 Cntl->노드 요청과 통일). 페이로드 없음,
         * 응답이 왔다는 사실 자체가 의미의 전부 */
        esp_now_set_time_ack_t ack = { .version = ESP_NOW_LINK_VERSION, .msg_type = ESP_NOW_MSG_SET_TIME_ACK };
        esp_err_t ack_err = esp_now_send(info->src_addr, (const uint8_t *)&ack, sizeof(ack));
        ESP_LOGI(TAG, "SET_TIME_ACK 전송: %s", esp_err_to_name(ack_err));
        return;
    }

    if (s_conn_state == CAM_CONN_PAIRED || msg_type != ESP_NOW_MSG_PAIR_REQUEST) return;
    if (len < (int)sizeof(esp_now_pair_request_t)) return;
    const esp_now_pair_request_t *req = (const esp_now_pair_request_t *)data;

    memcpy(s_hub_mac, req->hub_mac, sizeof(s_hub_mac));

    /* 2026-08-23(사용자 지시, 레이스 케이스3) — s_conn_state=PAIRED + notify_paired()(스캔
     * 정지, 뮤텍스로 보호됨)를 최대한 앞으로 당겨서, scan_timer_cb가 끼어들어 광고 한 통을
     * 더 보낼 수 있는 창을 최소화함(peer 등록/레이트 설정 같은 뒤쪽 작업이 끝날 때까지
     * 기다릴 이유가 없음 — 어차피 hub_mac은 이미 확정됐으므로) */
    s_conn_state = CAM_CONN_PAIRED;
    ESP_LOGI(TAG, "[STATE] -> %s (pair_request)", conn_state_name(s_conn_state));
    esp_now_channelsync_notify_paired();
    set_led(LED_PATTERN_HEARTBEAT);

    esp_now_peer_info_t peer = { 0 };
    memcpy(peer.peer_addr, s_hub_mac, sizeof(peer.peer_addr));
    peer.ifidx   = WIFI_IF_STA;
    peer.channel = 0;
    peer.encrypt = false;
    if (!esp_now_is_peer_exist(s_hub_mac)) {
        ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    }
    /* 2026-08-08 — ESP-NOW 기본 TX 레이트는 1Mbps(802.11b), 청크 페이싱(10ms)이 사실상 이
     * 레이트에서의 프레임 전송시간(~1220B/1Mbps≈9.8ms)에 맞춰 튜닝된 값이었을 가능성이 큼.
     * MCS0(HT20, 6.5Mbps)로 올려서 프레임당 전송시간을 줄임 — 30cm 근접/강한 신호 조건이라
     * 안정성 우선으로 가장 낮은 MCS만 시도(사용자 지시: 불안정하면 속도는 완전히 포기).
     * 실패해도(ESP_ERR 리턴) 치명적이지 않음 — 기본 1Mbps로 계속 동작하니 CHECK 안 함 */
    esp_now_rate_config_t rate_cfg = { .phymode = WIFI_PHY_MODE_HT20, .rate = WIFI_PHY_RATE_MCS0_LGI, .ersu = false, .dcm = false };
    esp_err_t rate_err = esp_now_set_peer_rate_config(s_hub_mac, &rate_cfg);
    ESP_LOGI(TAG, "CKPT: 허브 피어 레이트 설정(MCS0/HT20) -> %s", esp_err_to_name(rate_err));

    esp_now_pair_ack_t ack = {
        .version  = ESP_NOW_LINK_VERSION,
        .msg_type = ESP_NOW_MSG_PAIR_ACK,
    };
    memcpy(ack.node_mac, s_mac, sizeof(ack.node_mac));
    esp_err_t err = esp_now_send(s_hub_mac, (const uint8_t *)&ack, sizeof(ack));
    ESP_LOGI(TAG, "페어링됨: hub " MACSTR ", PAIR_ACK %s", MAC2STR(s_hub_mac), esp_err_to_name(err));

    /* 딥슬립 통계 전송은 CAM_CONFIG_SET 처리 직후로 옮김(2026-08-10) — 그때가 돼야
     * s_response_interval_sec이 실제 적용될 값으로 갱신돼 있음(recv_cb의
     * ESP_NOW_MSG_CAM_CONFIG_SET 핸들러 참고) */
    cam_node_note_activity();
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

    /* 광고/채널스캔/생존확인은 전부 esp_now_channelsync가 전담(2026-08-04 재설계) —
     * 브로드캐스트 피어 등록도 이 컴포넌트가 알아서 함 */
    esp_now_channelsync_init(s_name, s_mac, on_channel_synced, on_channel_lost_sync);
    /* 2026-08-23(사용자 지시) — 상태(ORPHAN/FOUND일 때만 보냄, PAIRED면 안 보냄)가 광고
     * 전송 자체를 직접 결정하게 함(방어적 이중 게이트, 위 should_advertise 정의 참고) */
    esp_now_channelsync_set_should_advertise_cb(should_advertise);
    set_led(LED_PATTERN_BLINK_FAST);
    /* 딥슬립 통계(send_deep_sleep_stats)는 여기서 주기 전송하지 않음 — 페어링 완료 직후
     * recv_cb의 PAIR_REQUEST 핸들러에서 1회만 보냄(위 함수 정의 주석 참고) */
}

const char *esp_now_cam_get_name(void) { return s_name; }
bool esp_now_cam_is_paired(void) { return s_conn_state == CAM_CONN_PAIRED; }

/* 2026-08-11 — 예전의 70초 자율취침 폴백을 대체(사용자 지시). fire-and-forget으로 충분 —
 * 유실돼도 cam_node_wake_window_done()이 다음 주기에 또 보냄(SLEEP_NOW_ACK 같은 확인
 * 응답 체계는 불필요, 반복 자체가 재시도 역할을 함) */
void esp_now_cam_send_sleep_now_request(void)
{
    if (!(s_conn_state == CAM_CONN_PAIRED)) return;
    esp_now_sleep_now_t req = { .version = ESP_NOW_LINK_VERSION, .msg_type = ESP_NOW_MSG_SLEEP_NOW_REQUEST };
    esp_err_t err = esp_now_send(s_hub_mac, (const uint8_t *)&req, sizeof(req));
    ESP_LOGI(TAG, "SLEEP_NOW_REQUEST 전송(재요청): %s", esp_err_to_name(err));
}
