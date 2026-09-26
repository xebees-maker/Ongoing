#include "esp_now_cam.h"
#include "cam_node.h"  /* cam_node_set_capture_interval_sec/set_response_interval_sec/get_wake_reason/note_activity */
#include "cam_speaker.h"  /* 2026-08-25 — PAIR_REQUESTED/PAIR_ACK 소리 알림용 */

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

/* 2026-08-25(CASK 재설계) — "알려진 CNTL"을 딥슬립 경계 너머로 기억(RTC 슬로우메모리, 8KB 중
 * 지금까지 52바이트만 쓰던 여유 확인 후 추가). s_last_synced_channel(esp_now_channelsync.c,
 * 순수 스캔 힌트)과 달리 이건 "이 CNTL이다"라는 신원까지 기억함 — 웨이크마다 광고부터 다시
 * 하는 대신 곧장 유니캐스트로 WAKE_HELLO를 시도할 수 있게 해줌(esp_now_cam_try_wake_hello_
 * fast_path 참고). 실패(3회 재시도 소진)하면 s_wake_hub_known은 그대로 두고(다음 사이클에
 * 다시 시도할 가치가 있음 — 채널만 잠깐 어긋났을 수도 있으므로) 그냥 폴백 스캔으로 넘어감 */
static RTC_DATA_ATTR uint8_t s_wake_hub_mac[6]     = { 0 };
static RTC_DATA_ATTR uint8_t s_wake_hub_channel    = 0;
static RTC_DATA_ATTR bool    s_wake_hub_known      = false;

static gpio_num_t s_led_pin = GPIO_NUM_NC;

/* --- 사진 전송 --- */
/* recv_cb(WiFi 태스크)에서 바로 처리하기엔 무거운 요청(촬영+전송)을 전용 태스크로 넘기는 큐.
 * 2026-09-26 — SD 제거로 LIST/DELETE_ALL 요청 종류 삭제(CAM엔 저장된 사진이 없음) */
typedef enum {
    CAM_TASK_REQ_PHOTO         = 0,
    CAM_TASK_REQ_BENCH         = 3,
    CAM_TASK_REQ_AUTO_CAPTURE  = 4,  /* 2026-09-18(SD 제거 재설계) — 주기촬영 타이머가
                                        capture_timer_cb(작은 스택)에서 직접 촬영하지 않고
                                        여기로 큐잉, 이 태스크(24KB 스택)가 촬영+푸시를 함 */
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
/* 2026-08-10 도입 — photo_transfer_task가 뭔가 처리 중인지(2026-09-19: esp_now_cam_enqueue_
 * auto_capture()도 큐잉 시점에 앞당겨 세팅함 — mark_transfer_idle() 주석 참고) */
static volatile bool s_transfer_busy = false;

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
 * esp_now_reliable_request()로 대체되어 제거됨(send_photo_from_buffer_sr() 참고) — 이제 DONE_ACK가
 * reliable 레이어의 응답으로 직접 돌아옴 */
/* 2026-08-21 — 예전엔 이 값을 여기 상수로 하드코딩하고 Cntl(photo_rx.c)도 똑같은 값을
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
/* 2026-09-26 — cam_node.c가 "스윕 끝났는데 못 찾음 → 잠들기"로 정한 뒤에는 광고 금지
 * (esp_now_cam_stop_advertising()). 예전엔 잠들기 전 대기(스피커 최대 2초) 동안에도 스캔 타이머가
 * 계속 광고해서, 이미 포기한 뒤에 페어링이 성립했다가 바로 잠드는 일이 있었음(실기). 딥슬립 후
 * 재부팅되면 false로 초기화됨 */
static volatile bool s_advertise_stopped = false;

static bool should_advertise(void)
{
    if (s_advertise_stopped) return false;
    return s_conn_state == CAM_CONN_ORPHAN || s_conn_state == CAM_CONN_FOUND;
}

void esp_now_cam_stop_advertising(void)
{
    s_advertise_stopped = true;  /* 다음 scan_timer_cb 틱에서 게이트가 타이머를 스스로 멈춤 */
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

/* 순수 로컬 표시(LED)용으로만 남김 — 생존/연결 판정은 전부 WAKE_HELLO의 reliable
 * 요청/응답(esp_now_cam_try_wake_hello_fast_path 참고)이 전담함. 물리계층 ACK(send_cb의
 * 성공/실패)는 "진짜 도달 확인"이 아니라는 원칙이라 판정 기준으로 안 씀.
 * 2026-08-24 — 다만 "광고가 실제로 무선에 나갔는가"는 이 콜백만이 알 수 있는 정보라(esp_now_send()의
 * 동기 리턴값은 큐잉 확인일 뿐, 위 esp_now_channelsync.h 주석 참고), 목적지가 브로드캐스트면
 * (이 프로젝트에서 광고만 브로드캐스트로 나감) 채널싱크로 완료를 알려줌 — "전송됨" 로그/소리가
 * 여기서 성공 확인된 순간에만 나가게 됨 */
static const uint8_t s_broadcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static void send_cb(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    /* 2026-09-25 — 송신 1건 완료 = 드라이버 송신 큐에 자리 생김. NO_MEM으로 대기 중인
     * esp_now_reliable_request()를 깨움(이벤트 방식, 대기 중인 게 없으면 아무 일도 안 함) */
    esp_now_reliable_on_send_done();
    if (info && info->des_addr && memcmp(info->des_addr, s_broadcast_mac, sizeof(s_broadcast_mac)) == 0) {
        if (status == ESP_NOW_SEND_SUCCESS) {
            esp_now_channelsync_notify_advertise_send_done();
        }
    }
    /* 2026-09-26(사용자 지시 — 센스 그린 LED와 같은 방식) — 여기서 성공 송신마다 HEARTBEAT를
     * 다시 거는 건 뺌. LED는 상태가 바뀌는 자리에서만 바꿈: 광고/재스캔=BLINK_FAST,
     * 페어링·WAKE_HELLO 성공(통신 중)=HEARTBEAT (esp_now_node_cask.c와 동일) */
}

/* 요청받은 chunk_idx들만 카메라 드라이버의 PSRAM 프레임버퍼(jpeg_buf/jpeg_len)에서 오프셋
 * 계산으로 잘라 보냄 — SR 윈도우의 신규 전송과 NACK 재전송 둘 다 이걸 씀(2026-09-18 SD 제거
 * 재설계로 파일 버전 resend_chunks()를 대체, 2026-09-26 파일 버전 삭제). nack 구조체는
 * packed라 missing_idx 원소를 memcpy로 읽음(-Werror=address-of-packed-member) */
static void resend_chunks_from_buffer(const uint8_t *jpeg_buf, size_t jpeg_len, uint32_t file_id,
                                       const esp_now_photo_chunk_nack_t *nack)
{
    esp_now_photo_chunk_t chunk = { .version = ESP_NOW_LINK_VERSION, .msg_type = ESP_NOW_MSG_PHOTO_CHUNK, .file_id = file_id };
    for (uint16_t i = 0; i < nack->missing_count; i++) {
        uint16_t idx;
        memcpy(&idx, &nack->missing_idx[i], sizeof(idx));
        size_t offset = (size_t)idx * ESP_NOW_PHOTO_CHUNK_DATA_LEN;
        if (offset >= jpeg_len) continue;
        size_t n = jpeg_len - offset;
        if (n > ESP_NOW_PHOTO_CHUNK_DATA_LEN) n = ESP_NOW_PHOTO_CHUNK_DATA_LEN;
        memcpy(chunk.data, jpeg_buf + offset, n);
        chunk.chunk_idx = idx;
        chunk.chunk_len = (uint16_t)n;

        esp_err_t err;
        int attempt;
        for (attempt = 0; attempt < 6; attempt++) {
            err = esp_now_send(s_hub_mac, (const uint8_t *)&chunk, sizeof(chunk));
            if (err != ESP_ERR_ESPNOW_NO_MEM) break;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "재전송 실패(buf): chunk[%u] -> %s(시도 %d회)", idx, esp_err_to_name(err), attempt + 1);
        }
        vTaskDelay(pdMS_TO_TICKS(5));
        if ((i + 1) % CHUNK_QUEUE_DRAIN_INTERVAL == 0) {
            vTaskDelay(pdMS_TO_TICKS(CHUNK_QUEUE_DRAIN_MS));
        }
    }
    ESP_LOGI(TAG, "NACK 재전송 완료(buf): file_id=%u %u개 청크", (unsigned)file_id, (unsigned)nack->missing_count);
}

/* Selective Repeat(2026-08-05 도입, 30분 BMT에서 블라스트+끝에 NACK 방식보다 꼬리 지연이
 * 훨씬 작아 채택 — project_cntl_cam_esp_now_reliability_layers 메모리 참고). 사진을
 * SR_WINDOW_SIZE개씩 윈도우로 나눠서, 윈도우 하나 다 보낼 때마다 그 범위 안에서 뭘 못
 * 받았는지 물어보고(WINDOW_STATUS_REQUEST/ACK) 빠진 것만 즉시 메꾼 뒤 다음 윈도우로 넘어감.
 * 신규 전송이든 재전송이든 "이 인덱스들 보내"라 resend_chunks_from_buffer() 하나로 처리.
 * 신뢰성 안전망(DONE/DONE_ACK/NACK라운드)은 마지막에 유지 — 윈도우 도중 놓친 게 있어도
 * 마지막에 한 번 더 전체 확인. */
#define SR_WINDOW_SIZE 16          /* CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM 기본값(16)에 맞춤 —
                                       이 값이면 이론상 로컬 큐 포화(NO_MEM) 자체를 거의 안 만남 */
#define SR_STATUS_TIMEOUT_MS 400   /* DONE의 800ms보다 짧게 — 윈도우당 훨씬 자주 도니까 */
#define SR_STATUS_MAX_ATTEMPTS 3

/* 2026-09-18(SD 제거 재설계) — 촬영한 사진 1장을 SR로 CNTL에 푸시. 카메라 드라이버의 PSRAM
 * 프레임버퍼(jpeg_buf/jpeg_len)를 그대로 씀 — 호출자(cam_node.c의 camera_capture_one())가
 * esp_camera_fb_get()으로 받은 fb를 이 함수가 끝날 때까지(META~DONE_ACK 전부) 들고 있어야 함.
 * kind는 META에 실어 CNTL에 전달해서 CNTL이 자기 쪽 파일명(M/T 접두사)을 결정하는 데 씀.
 * (예전 SD 파일 버전 send_one_photo_sr()은 2026-09-26 삭제) */
static bool send_photo_from_buffer_sr(const uint8_t *jpeg_buf, size_t jpeg_len, uint32_t file_id,
                                       cam_capture_kind_t kind, uint32_t my_generation)
{
    ESP_LOGI(TAG, "CKPT(SR-push): 시작 file_id=%u kind=%c len=%u", (unsigned)file_id, (char)kind, (unsigned)jpeg_len);
    uint16_t total_chunks = (uint16_t)((jpeg_len + ESP_NOW_PHOTO_CHUNK_DATA_LEN - 1) / ESP_NOW_PHOTO_CHUNK_DATA_LEN);
    uint32_t crc = esp_rom_crc32_le(0, jpeg_buf, jpeg_len);
    ESP_LOGI(TAG, "CKPT(SR-push): CRC 계산 완료 crc=%08x total_chunks=%u", (unsigned)crc, total_chunks);

    esp_now_photo_meta_t meta = {
        .version      = ESP_NOW_LINK_VERSION,
        .msg_type     = ESP_NOW_MSG_PHOTO_META,
        .kind         = (uint8_t)kind,
        .file_id      = file_id,
        .total_size   = (uint32_t)jpeg_len,
        .total_chunks = total_chunks,
        .crc32        = crc,
    };
    static const uint8_t s_meta_ack_types[] = { ESP_NOW_MSG_PHOTO_META_ACK };
    esp_err_t meta_err = esp_now_reliable_request(s_hub_mac, &meta, sizeof(meta),
                                                   s_meta_ack_types, 1,
                                                   800, 3,
                                                   NULL, 0, NULL);
    ESP_LOGI(TAG, "CKPT(SR-push): META_ACK: %s", esp_err_to_name(meta_err));
    if (meta_err != ESP_OK) {
        ESP_LOGW(TAG, "CKPT(SR-push): META 무응답 — 전송 포기(file_id=%u)", (unsigned)file_id);
        return false;
    }

    static esp_now_photo_chunk_nack_t range_req;
    static esp_now_photo_chunk_nack_t status_ack;
    uint32_t total_sent_chunks = 0;
    uint32_t status_requests   = 0;

    for (uint16_t window_base = 0; window_base < total_chunks; ) {
        if (s_request_generation != my_generation) {
            ESP_LOGI(TAG, "SR-push: 더 최신 요청으로 대체됨 — 중단(file_id=%u)", (unsigned)file_id);
            return false;
        }
        uint16_t window_count = total_chunks - window_base;
        if (window_count > SR_WINDOW_SIZE) window_count = SR_WINDOW_SIZE;

        range_req.file_id       = file_id;
        range_req.missing_count = window_count;
        for (uint16_t i = 0; i < window_count; i++) range_req.missing_idx[i] = window_base + i;
        resend_chunks_from_buffer(jpeg_buf, jpeg_len, file_id, &range_req);
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
            if (status_ack.missing_count > 0) {
                resend_chunks_from_buffer(jpeg_buf, jpeg_len, file_id, &status_ack);
                total_sent_chunks += status_ack.missing_count;
            }
        } else {
            ESP_LOGW(TAG, "SR-push: WINDOW_STATUS_ACK 무응답([%u,%u)) — 다음 윈도우로 진행(끝의 DONE/NACK가 안전망)",
                     window_base, window_end);
        }
        window_base = window_end;
    }
    ESP_LOGI(TAG, "CKPT(SR-push): 윈도우 루프 완료 — 총 전송청크(재전송포함)=%u/%u, 상태확인 %u회",
             (unsigned)total_sent_chunks, (unsigned)total_chunks, (unsigned)status_requests);

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
            ESP_LOGW(TAG, "CKPT(SR-push): DONE_ACK 무응답(라운드 %d) — 판단 보류", round + 1);
            return true;
        }
        if (done_ack.missing_count == 0) {
            ESP_LOGI(TAG, "CKPT(SR-push): DONE_ACK 완료 확인(라운드 %d)", round + 1);
            return true;
        }

        ESP_LOGI(TAG, "SR-push DONE_ACK: %u개 누락 — 재전송(라운드 %d/%d)", (unsigned)done_ack.missing_count, round + 1, s_nack_max_rounds);
        resend_chunks_from_buffer(jpeg_buf, jpeg_len, file_id, &done_ack);
        if (s_request_generation != my_generation) return false;
    }
    ESP_LOGW(TAG, "CKPT(SR-push): 재전송 라운드 소진");
    return true;
}

/* 2026-09-18(SD 제거 재설계) — 세션 로컬 file_id 카운터(재부팅마다 0부터 — CNTL이 실제
 * 영구 파일명/순번을 소유하므로 무관함) */
static uint32_t s_push_file_id_counter = 0;

bool esp_now_cam_push_captured_photo(const uint8_t *buf, size_t len, cam_capture_kind_t kind)
{
    if (s_conn_state != CAM_CONN_PAIRED) {
        ESP_LOGW(TAG, "촬영 푸시 스킵 — 페어링 안 됨");
        return false;
    }
    uint32_t file_id = ++s_push_file_id_counter;
    return send_photo_from_buffer_sr(buf, len, file_id, kind, s_request_generation);
}

bool esp_now_cam_enqueue_auto_capture(void)
{
    if (!s_photo_request_queue) return false;
    cam_task_request_t item = { .kind = CAM_TASK_REQ_AUTO_CAPTURE };
    if (xQueueSend(s_photo_request_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "AUTO_CAPTURE 큐잉 실패(큐 가득참, 직전 촬영 처리 중) — 이번 주기 건너뜀");
        return false;
    }
    /* 2026-09-19(주기촬영 재설계) — photo_transfer_task가 실제로 큐에서 꺼내기 *전*이라도
     * 여기서 바로 busy로 표시 — 그 틈에 호출부(cam_node.c의 CASK 루프)가 아직 안 바쁨으로
     * 오판해서 전송이 시작되기도 전에 잠들어버리는 레이스를 막음 */
    s_transfer_busy = true;
    return true;
}

bool esp_now_cam_is_transfer_busy(void)
{
    return s_transfer_busy;
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

/* 2026-08-10 도입, 처리 종류가 여럿이라 매 return/continue 지점마다 짝을 맞추는 대신 이번
 * 반복 시작에 세우고 끝에 내림. 2026-08-26 — 이걸 밖으로 노출하던 esp_now_cam_is_busy()는
 * 삭제됐다가, 2026-09-19(주기촬영 재설계)에 esp_now_cam_is_transfer_busy()로 다시 노출됨 —
 * cam_node.c의 CASK 루프가 "방금 큐잉한 촬영이 다 끝났는지" 확인해야 하기 때문(그 전엔
 * CNTL이 통신 중엔 SLEEP_NOW를 0으로 보내는 것만으로 충분했지만, 이번 촬영은 CNTL이 그 SLEEP_NOW를
 * 결정하는 바로 그 사이클에 막 큐잉되는 것이라 그 메커니즘이 아직 못 따라잡음 — s_transfer_busy
 * 선언은 파일 앞쪽(esp_now_cam_enqueue_auto_capture 등에서도 씀)으로 옮겨짐 */

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
                    /* 2026-09-26 — XFER 벤치(SD에 저장된 최근 사진을 반복 전송)는 SD 제거로 삭제 */
                    ESP_LOGW(TAG, "BENCH mode=%u 지원 안 함(SD 제거로 XFER 벤치 삭제) — 무시", (unsigned)item.bench_mode);
                }
            }
            mark_transfer_idle();
            continue;
        }

        /* 2026-09-18(SD 제거 재설계) — 주기촬영. capture_timer_cb()가 여기로 큐잉만 하고,
         * 실제 촬영+CNTL 푸시는 이 태스크(24KB 스택)에서 함 — cam_node_run_auto_capture()가
         * camera_capture_one(CAM_CAPTURE_KIND_AUTO)를 그대로 호출 */
        if (item.kind == CAM_TASK_REQ_AUTO_CAPTURE) {
            if ((s_conn_state == CAM_CONN_PAIRED)) {
                if (!cam_node_run_auto_capture()) {
                    ESP_LOGW(TAG, "AUTO_CAPTURE: 촬영 또는 푸시 실패 — 다음 주기에 재시도");
                }
            } else {
                ESP_LOGW(TAG, "AUTO_CAPTURE: 페어링 안 됨 — 이번 주기 건너뜀");
            }
            mark_transfer_idle();
            continue;
        }

        if (!(s_conn_state == CAM_CONN_PAIRED)) {
            mark_transfer_idle();
            continue;
        }

        esp_now_photo_request_t req = item.photo_req;

        /* Cntl이 "지금 당장 새로 찍어라" 요청한 경우 — esp_camera_fb_get()이 최대 수 초
         * 블로킹될 수 있어서 recv_cb(콜백 컨텍스트)에서 바로 처리하지 않고 여기(전용 태스크)
         * 까지 큐로 넘겨서 처리한다. 접수 확인(RECEIVED)은 recv_cb에서 이미 보냈음.
         * cam_node_capture_now()가 촬영 후 곧바로 CNTL에 푸시까지 함(2026-09-18 SD 제거 재설계) */
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

        /* 2026-09-26 — CAPTURE_NOW 외 모드(ALL/LATEST/BY_ID 등, SD에 저장된 사진 전송)는 SD
         * 제거로 삭제. CAM엔 저장된 사진이 없으니 응답할 것도 없음 */
        ESP_LOGW(TAG, "PHOTO_REQUEST mode=%d 지원 안 함(CAM에 저장된 사진 없음) — 무시", req.mode);
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
     * (node_hub.c) */
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
             * 신뢰성을 위해 같은 요청을 의도적으로 여러 번 보내는 경우(아래 photo_rx.c
             * 참고)에도 그대로 큐에 쌓여서 같은 사진/배치를 몇 번씩 중복 전송하고 있었음 —
             * 진짜 새 요청이 아니면 아예 무시 */
            ESP_LOGI(TAG, "PHOTO_REQUEST 중복 수신(mode=%d param=%u) — 무시", req.mode, (unsigned)req.param);
        }
        return;
    }

    if (msg_type == ESP_NOW_MSG_BENCH_START) {
        if (!(s_conn_state == CAM_CONN_PAIRED) || len < (int)sizeof(esp_now_bench_start_t)) return;
        esp_now_bench_start_t req;
        memcpy(&req, data, sizeof(req));
        cam_task_request_t item = { .kind = CAM_TASK_REQ_BENCH, .bench_duration_sec = req.duration_sec,
                                     .bench_mode = req.mode };
        if (xQueueSend(s_photo_request_queue, &item, 0) != pdTRUE) {
            ESP_LOGW(TAG, "BENCH_START 큐 가득 — 무시");
        }
        return;
    }

    if (msg_type == ESP_NOW_MSG_CAM_CONFIG_SET) {
        /* 2026-08-08 — 촬영주기+응답성(연결성/절전) 원격 설정. recv_cb(ESP-NOW 드라이버
         * 태스크)에서 바로 처리 — 값 적용뿐이라 photo_transfer_task 큐로 넘길 만큼 무겁지 않음 */
        if (!(s_conn_state == CAM_CONN_PAIRED) || len < (int)sizeof(esp_now_cam_config_t)) return;
        cam_speaker_notify(SPK_EVT_CAM_CONFIG);
        esp_now_cam_config_t cfg;
        memcpy(&cfg, data, sizeof(cfg));
        cam_node_set_capture_interval_sec(cfg.capture_interval_sec);
        cam_node_set_response_interval_sec(cfg.response_interval_sec);
        /* 2026-08-25(CASK 재설계) — SET_TIME이 별도 메시지였던 걸 여기로 흡수. 매번 무조건
         * 적용(딥슬립이 RTC 시간 도메인은 보존해서 사실 매번 다시 맞출 필요는 없지만, "필요한지
         * 판단하는 로직"을 추가하는 비용이 그냥 매번 적용하는 것보다 큼 — esp_now_link.h의
         * esp_now_cam_config_t.unix_time 주석 참고) */
        struct timeval tv = { .tv_sec = (time_t)cfg.unix_time, .tv_usec = 0 };
        settimeofday(&tv, NULL);
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
        return;
    }

    /* Cntl이 연결 해제했다는 통보 — 페어링만 정리(2026-08-04 — 채널 동기화는 그대로 유지,
     * esp_now_channelsync가 독립적으로 계속 확인 중이라 다시 스캔할 필요 없음. 예전엔
     * enter_advertising()으로 채널 스캔부터 다시 했는데, 채널이 안 바뀐 이상 불필요한
     * 낭비였음) */
    /* 2026-08-25(CASK 재설계) — UNPAIR은 유일하게 유지된 명시적 단발 통보(사용자의 실시간
     * 조작이라 즉각 반영돼야 함, reliable로 승격됨 — node_hub.c 참고). 이제 reliable
     * 요청이라 매칭되는 ACK를 반드시 돌려줘야 CNTL 쪽이 도달을 확인함 */
    if (msg_type == ESP_NOW_MSG_UNPAIR) {
        if (!(s_conn_state == CAM_CONN_PAIRED) || len < (int)sizeof(esp_now_unpair_t)) return;
        if (memcmp(info->src_addr, s_hub_mac, sizeof(s_hub_mac)) != 0) return;
        ESP_LOGI(TAG, "Cntl이 연결 해제함");
        s_conn_state = CAM_CONN_ORPHAN;
        s_wake_hub_known = false;  /* 이 CNTL로의 빠른 재연결 시도 자체를 그만둠 */
        ESP_LOGI(TAG, "[STATE] -> %s (unpair)", conn_state_name(s_conn_state));
        set_led(LED_PATTERN_BLINK_FAST);
        esp_now_unpair_t ack = { .version = ESP_NOW_LINK_VERSION, .msg_type = ESP_NOW_MSG_UNPAIR_ACK };
        esp_now_send(info->src_addr, (const uint8_t *)&ack, sizeof(ack));
        return;
    }

    /* CNTL이 "이번 사이클에 더 할 일 없다"고 판단하면 보냄(CASK의 마지막 단계) — sleep_sec이
     * 곧 다음 딥슬립 시간이거나(2026-08-25) 0이면 안 자고 곧바로 다음 WAKE_HELLO 루프 */
    if (msg_type == ESP_NOW_MSG_CASK_WORK_NONE) {
        /* 2026-08-26(사용자 지시) — CASK "할일" 단계가 비어있을 때 오는 명시적 신호. 특별히
         * 할 일은 없고 ACK만 돌려줌 — 이게 옴으로써 캠은 이번 CASK엔 사진요청 등 큐잉된
         * 액션이 없었다는 걸 확실히 앎(추측 불필요, node_hub.c의 dequeue_pending_action_locked
         * 참고) */
        if (!(s_conn_state == CAM_CONN_PAIRED) || len < (int)sizeof(esp_now_cask_work_none_t)) return;
        esp_now_cask_work_none_t ack = { .version = ESP_NOW_LINK_VERSION, .msg_type = ESP_NOW_MSG_CASK_WORK_NONE_ACK };
        esp_now_send(s_hub_mac, (const uint8_t *)&ack, sizeof(ack));
        return;
    }

    if (msg_type == ESP_NOW_MSG_SLEEP_NOW) {
        if (!(s_conn_state == CAM_CONN_PAIRED) || len < (int)sizeof(esp_now_sleep_now_t)) return;
        const esp_now_sleep_now_t *msg = (const esp_now_sleep_now_t *)data;
        ESP_LOGI(TAG, "SLEEP_NOW 수신(sleep_sec=%u)", (unsigned)msg->sleep_sec);
        cam_speaker_notify(SPK_EVT_SLEEP_NOW);
        cam_node_note_sleep_now_requested(msg->sleep_sec);
        esp_now_sleep_now_t ack = { .version = ESP_NOW_LINK_VERSION, .msg_type = ESP_NOW_MSG_SLEEP_NOW_ACK };
        esp_err_t ack_err = esp_now_send(s_hub_mac, (const uint8_t *)&ack, sizeof(ack));
        ESP_LOGI(TAG, "SLEEP_NOW_ACK 전송: %s", esp_err_to_name(ack_err));
        cam_speaker_notify(SPK_EVT_SLEEP_NOW_ACK);
        return;
    }

    if (s_conn_state == CAM_CONN_PAIRED || msg_type != ESP_NOW_MSG_PAIR_REQUEST) return;
    if (len < (int)sizeof(esp_now_pair_request_t)) return;

    /* 2026-09-23(사용자 지시 — 콘의 MAC은 내부망 프로토콜 어디에도 들어가면 안 됨,
     * feedback_cntl_mac_never_in_internal_protocol 메모리 참고) — hub_mac을 페이로드에서 읽지
     * 않고, 프레임의 실제 발신지(info->src_addr)를 씀. 이건 ESP-NOW 드라이버가 채워주는
     * 값이라 브가 실제로 쏜 MAC이 항상 정확히 들어있음 */
    memcpy(s_hub_mac, info->src_addr, sizeof(s_hub_mac));

    /* 2026-08-23(사용자 지시, 레이스 케이스3) — s_conn_state=PAIRED + notify_paired()(스캔
     * 정지, 뮤텍스로 보호됨)를 최대한 앞으로 당겨서, scan_timer_cb가 끼어들어 광고 한 통을
     * 더 보낼 수 있는 창을 최소화함(peer 등록/레이트 설정 같은 뒤쪽 작업이 끝날 때까지
     * 기다릴 이유가 없음 — 어차피 hub_mac은 이미 확정됐으므로) */
    s_conn_state = CAM_CONN_PAIRED;
    ESP_LOGI(TAG, "[STATE] -> %s (pair_request)", conn_state_name(s_conn_state));
    esp_now_channelsync_notify_paired();
    cam_speaker_notify(SPK_EVT_PAIR_REQUESTED);
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
    /* 2026-08-26(순서 버그 수정) — CNTL이 PAIR_ACK 수신 직후 곧바로 CONFIG+SLEEP_NOW를
     * 보내므로(최초 페어링/졸업 CASK), 전송 직전에 미리 대기 상태를 깨끗하게 함 —
     * esp_now_cam_try_wake_hello_fast_path()의 동일 조치와 같은 이유 */
    cam_node_reset_sleep_now_state();
    esp_err_t err = esp_now_send(s_hub_mac, (const uint8_t *)&ack, sizeof(ack));
    ESP_LOGI(TAG, "페어링됨: hub " MACSTR ", PAIR_ACK %s", MAC2STR(s_hub_mac), esp_err_to_name(err));
    cam_speaker_notify(SPK_EVT_PAIR_ACK);

    /* 2026-08-25(CASK 재설계) — "졸업": 지금 막 전체 스캔으로 찾아낸 이 CNTL을 다음 웨이크부터
     * 곧장 유니캐스트로 시도할 수 있게 기억해둠(esp_now_cam_try_wake_hello_fast_path 참고) */
    uint8_t current_channel = 0;
    wifi_second_chan_t second_chan;
    esp_wifi_get_channel(&current_channel, &second_chan);
    memcpy(s_wake_hub_mac, s_hub_mac, sizeof(s_wake_hub_mac));
    s_wake_hub_channel = current_channel;
    s_wake_hub_known   = true;
}

/* 2026-09-26 — 초기화(status_led_init/status_led_init_custom)는 호출측 몫. CAM의 LED는 GPIO가
 * 아니라 IO 익스팬더 핀이라 여기서 status_led_init(GPIO 전용)을 부를 수 없음(cam_node.c 참고) */
void esp_now_cam_set_status_led(gpio_num_t pin)
{
    s_led_pin = pin;
}

/* 2026-08-25(CASK 재설계) — 알려진 CNTL(s_wake_hub_*)이 있으면 광고 없이 곧장 유니캐스트로
 * "저 왔어요"를 시도. 성공하면 예전 3단계 핸드셰이크(ADVERTISE_ACK->PAIR_REQUEST->PAIR_ACK)
 * 전체를 요청 1번+reliable 응답 1번으로 대체함 — esp_now_reliable_request()의 재시도
 * 파라미터(100ms×3회, "빠른 재시도" 요구사항 그 자체) 하나로 충분, 별도 루프 불필요. 실패하면
 * (3회 소진) 그 자리에서 아무 것도 안 하고 false만 리턴 — 호출부(esp_now_cam_init)가 기존
 * 채널스캔 폴백으로 넘어감. 이 실패 하나로 채널변경/CNTL다운/CNTL이 이 캠을 모름, 세 원인을
 * 전부 동일하게 처리함(사용자 지시: "재연결 시퀀스를 동일하게 타야지" — 원인 구분 없음).
 * DEEP_SLEEP_STATS가 하던 통계 보고를 이 메시지가 그대로 흡수함 */
/* 2026-08-10 도입 -> 2026-08-26 수정(사용자 지시: "AW는 누적치를 보내고 있고, 최종 보고
 * 이후 리셋된 값에서 다시 시작했어야 해") — 예전엔 esp_timer_get_time()(이번 부팅 후 경과
 * 시간)을 그대로 실어보냈는데, 정상 딥슬립 사이클(매번 완전 재부팅)에선 부팅=깨어난 시점이라
 * 우연히 맞았지만, Live 모드처럼 한 부팅 안에서 WAKE_HELLO를 여러 번 보내는 경우엔 매번
 * "부팅 이후 전체" 값이라 보고할수록 계속 커지기만 했음(진짜 "이번 보고 이후 얼마나
 * 깨어있었는지"가 아니었음). 평범한 static(RTC 아님)이라 매 부팅 0에서 시작 — 그 부팅의
 * 첫 보고는 자연히 "부팅 이후"가 되고(정상 사이클과 동일), 같은 부팅 안의 다음 보고부터는
 * 직전 보고 이후의 델타만 잡힘 */
static uint32_t s_last_wake_hello_report_ms = 0;

static bool esp_now_cam_try_wake_hello_fast_path(void)
{
    if (!s_wake_hub_known) return false;

    esp_wifi_set_channel(s_wake_hub_channel, WIFI_SECOND_CHAN_NONE);
    memcpy(s_hub_mac, s_wake_hub_mac, sizeof(s_hub_mac));

    esp_now_peer_info_t peer = { 0 };
    memcpy(peer.peer_addr, s_hub_mac, sizeof(peer.peer_addr));
    peer.ifidx   = WIFI_IF_STA;
    peer.channel = 0;
    peer.encrypt = false;
    if (!esp_now_is_peer_exist(s_hub_mac)) {
        esp_now_add_peer(&peer);
    }
    /* MCS0/HT20 — PAIR_REQUEST 핸들러의 동일 설정과 같은 근거(esp_now_cam.c 위쪽 참고) */
    esp_now_rate_config_t rate_cfg = { .phymode = WIFI_PHY_MODE_HT20, .rate = WIFI_PHY_RATE_MCS0_LGI, .ersu = false, .dcm = false };
    esp_now_set_peer_rate_config(s_hub_mac, &rate_cfg);

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    esp_now_wake_hello_t hello = {
        .version             = ESP_NOW_LINK_VERSION,
        .msg_type            = ESP_NOW_MSG_WAKE_HELLO,
        .wake_reason          = (uint8_t)cam_node_get_wake_reason(),
        .awake_uptime_ms      = now_ms - s_last_wake_hello_report_ms,
        .sleep_interval_sec   = cam_node_get_response_interval_sec(),
        .actual_last_sleep_sec = cam_node_get_last_actual_sleep_sec(),
    };
    s_last_wake_hello_report_ms = now_ms;
    uint16_t batt_raw = 0, batt_mv = 0;
    cam_node_read_battery_mv(&batt_raw, &batt_mv);
    hello.battery_adc_raw = batt_raw;
    hello.battery_mv      = batt_mv;

    static const uint8_t s_wake_hello_ack_types[] = { ESP_NOW_MSG_WAKE_HELLO_ACK };
    esp_now_wake_hello_ack_t ack;
    cam_speaker_notify(SPK_EVT_WAKE_HELLO);
    /* 2026-08-26(순서 버그 수정) — CNTL이 WAKE_HELLO_ACK 직후 곧바로 CONFIG+SLEEP_NOW를
     * 보내므로, 그게 도착하기 전에(전송 직전) 미리 대기 상태를 깨끗하게 함 — cam_node.c의
     * cam_node_reset_sleep_now_state() 주석 참고 */
    cam_node_reset_sleep_now_state();
    esp_err_t err = esp_now_reliable_request(s_hub_mac, &hello, sizeof(hello),
                                              s_wake_hello_ack_types, 1,
                                              200, 3,  /* 2026-09-05 — 100ms는 CNTL이 사진전송
                                                          뒷정리 등으로 순간 바쁠 때 너무 타이트해서
                                                          불필요한 재전송을 유발(node_hub.c의
                                                          WAKE_HELLO_ACK 재시도 수정과 짝) */
                                              &ack, sizeof(ack), NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WAKE_HELLO 무응답(3회) — 폴백 스캔으로 전환");
        s_conn_state = CAM_CONN_ORPHAN;  /* 재시도 중이었다면(이미 PAIRED였을 수 있음) 정리 */
        ESP_LOGI(TAG, "[STATE] -> %s (wake_hello_fail)", conn_state_name(s_conn_state));
        set_led(LED_PATTERN_BLINK_FAST);  /* 센스와 동일 — 폴백 스캔(광고)으로 돌아감 */
        return false;
    }
    cam_speaker_notify(SPK_EVT_WAKE_HELLO_ACK);

    /* notify_paired()는 channelsync가 이미 초기화돼있을 때만(재시도 케이스) 의미 있음 —
     * 내부적으로 스캔/휴식 타이머를 null-check하므로 아직 초기화 전(최초 성공)이어도 안전 */
    esp_now_channelsync_notify_paired();
    s_conn_state = CAM_CONN_PAIRED;
    ESP_LOGI(TAG, "[STATE] -> %s (wake_hello)", conn_state_name(s_conn_state));
    set_led(LED_PATTERN_HEARTBEAT);
    return true;
}

/* 2026-08-25(CASK 재설계) — channelsync는 부팅 사이클 중 최초 1회만 init(), 그 뒤로는
 * resume_scan()만 씀(타이머 중복 생성 방지). fast path가 실패할 때마다(최초 시도든, 이후
 * 침묵 타임아웃으로 인한 재시도든) 이 함수 하나만 부르면 알아서 처리됨 */
static bool s_channelsync_initialized = false;

bool esp_now_cam_reconnect(void)
{
    if (esp_now_cam_try_wake_hello_fast_path()) {
        return true;
    }
    if (!s_channelsync_initialized) {
        s_channelsync_initialized = true;
        /* 광고/채널스캔은 esp_now_channelsync가 전담 — 브로드캐스트 피어 등록도 이
         * 컴포넌트가 알아서 함 */
        esp_now_channelsync_init(s_name, s_mac, on_channel_synced);
        /* 2026-08-23(사용자 지시) — 상태(ORPHAN/FOUND일 때만 보냄, PAIRED면 안 보냄)가 광고
         * 전송 자체를 직접 결정하게 함(방어적 이중 게이트, 위 should_advertise 정의 참고) */
        esp_now_channelsync_set_should_advertise_cb(should_advertise);
    } else {
        esp_now_channelsync_resume_scan();
    }
    cam_node_note_scan_restarted();  /* 이전 스윕완료 기록 무효화(cam_node.h 참고) */
    set_led(LED_PATTERN_BLINK_FAST);
    return false;
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
     * 중첩 호출로 스택에 동시에 쌓여서 12KB를 넘겼던 것으로 추정 — 24KB로 증설.
     * 2026-09-26 — SD/LIST 경로는 삭제됐지만 촬영+푸시가 이 태스크에서 돌므로 크기는 그대로 둠
     * (줄이려면 실측 스택 최고치 확인 후) */
    xTaskCreate(photo_transfer_task, "photo_tx", 24576, NULL, 5, NULL);

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));
    ESP_ERROR_CHECK(esp_now_register_send_cb(send_cb));

    esp_now_cam_reconnect();
}

const char *esp_now_cam_get_name(void) { return s_name; }
bool esp_now_cam_is_paired(void) { return s_conn_state == CAM_CONN_PAIRED; }
