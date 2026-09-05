/**
 * @file    sens_deep_sleep_node.c
 * @brief   SCD41 헤드리스 Sens 노드 — CAM(cam_node.c)의 웨이크/CASK/딥슬립 구조를 그대로
 *          이식(2026-09-05, 사용자 지시: "캠에서 구현된 걸 가져다 써야되").
 *
 * 예전 sensor_node.c(계속실행+라이트슬립)에서 재사용 범위로 명시된 것만 옮겨옴 — 센서
 * 연동(SCD41), 배터리 잔량 체크, LED 제어. 네트워크/웹(wifi_dashboard.c)과 history_log는
 * 이번 범위에서 제외(사용자 지시). 통신/전원관리 뼈대는 esp_now_node_cask.c/rwdt_guard.c를
 * 통해 CAM 것을 그대로 씀.
 *
 * 2026-09-05(두 번째 수정, 사용자 지시: "캠과 동일한 구조로 가라") — 이 파일이 캠의
 * cam_node.c와 대칭되는 자리 — CASK 이벤트기반 대기(세마포어/스윕완료/SLEEP_NOW 상태)를
 * 여기서 소유(esp_now_node_cask.c는 esp_now_cam.c와 대칭, 연결 상태만 다룸). 측정 "시도"는
 * 캠의 주기촬영처럼 네트워크/캐스크와 완전히 독립적으로 하드웨어 초기화 직후 곧장 하되,
 * 실제 SCD41 트리거는 게이팅됨(사용자 지시: "전력소모 큰 센서가 있어서... 측정주기에만
 * 측정해야되") — 마지막 실측정 이후 실제 경과시간이 측정주기 이상일 때만 재측정, 아니면
 * 캐시값 재사용. 값은 RTC 메모리에 캐시해 딥슬립 넘어서도 유지, 측정ID(measurement_id)를
 * 새로 측정 성공할 때마다 증가시켜 캐스크(WAKE_HELLO_SENS)에 실어보냄(사용자 지시: "측정
 * 값이 없으면 그냥 웨헬 보내고, 있으면 보내도록", "콘이 달라지지 않은 측정값을 처리할
 * 필요 없도록"). 딥슬립 실제 시간은 캠과 동일하게 SLEEP_NOW.sleep_sec 그 자체를 그대로 씀
 * (esp_now_node_get_last_sleep_sec()) — 콘 쪽(esp_now_hub.c의 send_cask_sleep_now())이
 * MIN(응답성, 이 센스의 측정주기)을 계산해서 보내므로, 응답성<측정주기일 때는 측정주기보다
 * 자주 깨지만(콘의 제어 기회 확보용) 위 게이팅 덕분에 그 웨이크들에서 재측정하지는 않음.
 * 센서종류/채널구성(sensor_kind/chan_type)은 매 캐스크가 아니라 페어링(PAIR_ACK) 때 1회만
 * 보냄(esp_now_link.h의 esp_now_pair_ack_t 참고) — esp_now_node_init()에 그때 한 번 넘김.
 */

#include <string.h>
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "bsp_c3_pico.h"
#include "battery.h"
#include "esp_now_node.h"
#include "esp_now_channelsync.h"
#include "status_led.h"
#include "rwdt_guard.h"
#include "scd41.h"

static const char *TAG = "sens_deep_sleep_node";

#define SENSOR_KIND_CURRENT  SENSOR_KIND_SCD41
#define SENSOR_CHAN_COUNT    3
static const uint8_t s_chan_types[SENSOR_CHAN_COUNT] = {
    SENSOR_CHAN_CO2_PPM, SENSOR_CHAN_TEMP_C, SENSOR_CHAN_HUMI_PCT,
};

#define BATT_PLATEAU_MIN_MV     3900.0f
#define BATT_FULL_MV_DEFAULT    4020.0f
#define NVS_NS_BATTCAL           "battcal"
#define NVS_KEY_FULL_MV          "full_mv"

/* SCD41 single-shot 측정 소요시간(~5초) 대비 여유 — 트리거 후 이 안에 완료 안 되면 실패로
 * 처리(cam_node.c류 "무한정 안 기다린다" 원칙과 동일). 2026-09-05 — 센서마다 측정 소요시간이
 * 다름(사용자 지시: 온습도~100ms, CO2~1초, 암모니아~3초) — 이 값은 SCD41 전용이고, 다른
 * 센서의 딥슬립 포팅이 나중에 필요해지면 그 센서 고유 타임아웃으로 따로 정의해야 함(지금은
 * SCD41 헤드리스 빌드만 범위) */
#define SCD41_MEASURE_POLL_MS    200
#define SCD41_MEASURE_TIMEOUT_MS 6000

/* CAM의 cam_node.c와 동일 상수(esp_now_link.h의 ESP_NOW_NODE_UNPAIRED_RETRY_SEC 공용값 재사용) */
#define UNPAIRED_BACKOFF_SHORT_UNTIL_SEC   60          /* 1분까지: 짧게(3초) */
#define UNPAIRED_BACKOFF_MID_SEC           10          /* 1~11분: 10초 */
#define UNPAIRED_BACKOFF_MID_UNTIL_SEC     (60 + 600)
#define UNPAIRED_BACKOFF_LONG_SEC          30          /* 11분 이후: 30초 */

/* CASK 왕복 전체(CONFIG 800ms×3 + 할일없음 500ms×3 + SLEEP_NOW 300ms×3 = 4.8s) 대비 여유 —
 * 캠의 CASK_TIMEOUT_MS(15초, 사진목록 등 무거운 할일 포함)보다 짧음(센스는 그런 무거운
 * 할일이 없음) */
#define SENS_CASK_TIMEOUT_MS      8000
/* Live 모드(SLEEP_NOW.sleep_sec==0) 재체크인 페이싱 — 캠의 CASK_LIVE_PACE_MS와 동일 값 */
#define SENS_CASK_LIVE_PACE_MS    1000

static float s_full_mv           = BATT_FULL_MV_DEFAULT;
static int   s_full_mv_persisted = 0;
static adc_oneshot_unit_handle_t s_vin_adc = NULL;

/* 2026-09-05 — 측정값 캐시(딥슬립 경계 넘어 유지) + 측정ID(캠의 s_wake_hub_* RTC_DATA_ATTR
 * 패턴과 동일 이유). 0=한 번도 측정 성공한 적 없음 — 이때만 웨헬이 "빈" 값(chan_ok=false)을
 * 실어보냄(사용자 설계: "캐스크에 측정 값이 없으면 그냥 웨헬 보내고") */
static RTC_DATA_ATTR uint32_t s_measurement_id                    = 0;
static RTC_DATA_ATTR float    s_cached_vals[SENSOR_CHAN_COUNT]     = { 0 };
static RTC_DATA_ATTR uint8_t  s_cached_chan_ok[SENSOR_CHAN_COUNT]  = { 0 };
static RTC_DATA_ATTR uint32_t s_unpaired_backoff_elapsed_sec       = 0;

/* 2026-09-05(사용자 지시) — 전력소모가 큰 센서가 있어서 "깰 때마다"가 아니라 "실제 측정
 * 주기만큼 지났을 때만" 재측정해야 함(응답성<측정주기일 때 측정주기보다 자주 깨는 건
 * 콘의 제어 기회 확보용일 뿐, 그때마다 재측정하면 의미없이 전력만 씀). 이 값은 "마지막
 * 실측정 이후 실제로 몇 초가 지났는지"의 누적치 — 매 사이클 끝에 이번에 실제로 잠들
 * sleep_sec만큼 더해두고, 실측정에 성공하면 0으로 리셋. RTC_DATA_ATTR로 딥슬립 경계
 * 너머 유지해야 다음 부팅(=다음 웨이크) 때도 정확함 */
static RTC_DATA_ATTR uint32_t s_seconds_since_last_measurement    = 0;

/* 2026-09-05 — CAM의 cam_node.c 이벤트기반 대기 루프 이식(s_wake_recheck_sem/s_sweep_completed/
 * s_sleep_now_requested). esp_now_node_cask.c(연결 상태 전담, esp_now_cam.c와 대칭)가
 * esp_now_node_signal_recheck()/esp_now_node_note_sleep_now_requested()/
 * esp_now_node_note_scan_restarted()(esp_now_node.h 선언)로 이 상태를 갱신함 */
static SemaphoreHandle_t s_wake_recheck_sem  = NULL;
static volatile bool     s_sweep_completed   = false;
static volatile bool     s_sleep_now_requested = false;

void esp_now_node_signal_recheck(void)
{
    if (s_wake_recheck_sem) xSemaphoreGive(s_wake_recheck_sem);
}

void esp_now_node_note_sleep_now_requested(uint32_t sleep_sec)
{
    (void)sleep_sec;  /* 실제 딥슬립 시간엔 안 씀(esp_now_node.h의 get_sample_interval_sec
                         주석 참고) — 여기선 "SLEEP_NOW 왔다"는 신호만 필요 */
    s_sleep_now_requested = true;
    esp_now_node_signal_recheck();
}

void esp_now_node_reset_sleep_now_state(void)
{
    s_sleep_now_requested = false;
}

void esp_now_node_note_scan_restarted(void)
{
    s_sweep_completed = false;
}

/* 채널스캔 스윕 한 바퀴 완료 훅(esp_now_channelsync_set_event_hooks 참고) — CAM은 스피커
 * 알림용 훅을 통해 간접적으로 이 상태를 세우지만(cam_speaker 의존), 센스는 스피커가 없으니
 * 이 컴포넌트에 직접 등록 */
static void on_scan_sweep_done(void)
{
    s_sweep_completed = true;
    esp_now_node_signal_recheck();
}

static uint32_t next_unpaired_retry_sleep_sec(void)
{
    if (s_unpaired_backoff_elapsed_sec < UNPAIRED_BACKOFF_SHORT_UNTIL_SEC) return ESP_NOW_NODE_UNPAIRED_RETRY_SEC;
    if (s_unpaired_backoff_elapsed_sec < UNPAIRED_BACKOFF_MID_UNTIL_SEC) return UNPAIRED_BACKOFF_MID_SEC;
    return UNPAIRED_BACKOFF_LONG_SEC;
}

static bool vin_indicates_usb(void)
{
    if (!s_vin_adc) return false;
    int raw = 0;
    if (adc_oneshot_read(s_vin_adc, BSP_C3_VIN_ADC_CHANNEL, &raw) != ESP_OK) return false;
    int mv = (int)((float)raw * 3100.0f / 4095.0f);
    return mv >= BSP_C3_VIN_PRESENT_MV;
}

/* sensor_node.c의 maybe_learn_full_mv()와 같은 목적이지만 단순화(2026-09-05) — 예전 버전은
 * "계속실행" 전제로 20샘플 추세밴드를 봤는데, 딥슬립에선 매 부팅이 곧 새 판독 하나뿐이라
 * 그 창 자체가 없음. 이번 판독이 USB 전원 중이고 완충값 부근이면 그대로 학습 */
static void maybe_learn_full_mv(bool powered, int mv)
{
    if (!powered) return;
    if ((float)mv < BATT_PLATEAU_MIN_MV) return;
    if (mv == (int)s_full_mv) return;

    s_full_mv = (float)mv;
    battery_set_full_mv(s_full_mv);
    ESP_LOGI(TAG, "배터리 완충 전압 학습: %d mV", mv);

    if (mv != s_full_mv_persisted) {
        nvs_handle_t h;
        if (nvs_open(NVS_NS_BATTCAL, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_i32(h, NVS_KEY_FULL_MV, mv);
            nvs_commit(h);
            nvs_close(h);
            s_full_mv_persisted = mv;
        }
    }
}

static led_pattern_t batt_pct_to_led_pattern(int pct)
{
    if (pct >= 60) return LED_PATTERN_HEARTBEAT;
    if (pct >= 20) return LED_PATTERN_HEARTBEAT_FAST;
    return LED_PATTERN_HEARTBEAT_URGENT;
}

/* SCD41 single-shot 판독 — 트리거 후 최대 SCD41_MEASURE_TIMEOUT_MS까지 블로킹 폴링.
 * 예전 sensor_node.c는 이걸 여러 esp_timer 틱에 걸쳐 논블로킹으로 했는데(계속실행 전제),
 * 딥슬립은 부팅마다 한 번뿐이라 "다음 틱"이 없음 — CAM의 촬영 대기 패턴과 동일하게
 * 한 부팅 안에서 블로킹으로 끝냄(scd41.c의 trigger/poll API 자체는 안 건드림) */
static bool measure_scd41(float out[SENSOR_CHAN_COUNT])
{
    if (!scd41_trigger_single_shot()) return false;

    uint32_t waited_ms = 0;
    while (waited_ms < SCD41_MEASURE_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(SCD41_MEASURE_POLL_MS));
        waited_ms += SCD41_MEASURE_POLL_MS;
        int co2 = 0;
        float t = 0.0f, h = 0.0f;
        bool ok = false;
        if (scd41_poll_single_shot(&co2, &t, &h, &ok)) {
            if (ok) { out[0] = (float)co2; out[1] = t; out[2] = h; }
            return ok;
        }
    }
    ESP_LOGW(TAG, "SCD41 측정 타임아웃(%ums)", (unsigned)SCD41_MEASURE_TIMEOUT_MS);
    return false;
}

void app_main(void)
{
    /* 최우선 — WiFi/센서 초기화보다 먼저(cam_node.c의 capture_wake_reason() 호출 순서와
     * 동일 이유, esp_now_node_cask.c 참고) */
    esp_now_node_capture_wake_info();

    /* 초기 보수적 예산(첫 부팅엔 아직 CNTL의 실제 샘플주기를 모름) — SENS_CONFIG_SET 수신
     * 시 esp_now_node_cask.c가 실제 값으로 재무장함(cam_node_set_response_interval_sec()과
     * 동일 패턴) */
    rwdt_guard_arm(15 + CONFIG_SENS_DEEPSLEEP_AWAKE_MARGIN_SEC);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    nvs_handle_t batt_nvs;
    if (nvs_open(NVS_NS_BATTCAL, NVS_READONLY, &batt_nvs) == ESP_OK) {
        int32_t learned_mv = 0;
        if (nvs_get_i32(batt_nvs, NVS_KEY_FULL_MV, &learned_mv) == ESP_OK) {
            s_full_mv           = (float)learned_mv;
            s_full_mv_persisted = learned_mv;
        }
        nvs_close(batt_nvs);
    }

    ESP_ERROR_CHECK(bsp_c3_pico_init());

    if (!scd41_init(BSP_C3_I2C_PORT, BSP_C3_I2C_SDA, BSP_C3_I2C_SCL)) {
        ESP_LOGW(TAG, "SCD41 초기화 실패 — 연결 확인 필요(다음 사이클에 재시도)");
    }
    /* scd41_init()은 항상 continuous 모드로 시작 — single-shot 듀티사이클을 쓰려면 꺼야 함 */
    scd41_stop_periodic_measurement();
    /* 2026-09-05 버그수정(실기 로그로 확인) — stop_periodic_measurement 직후 곧바로 single-shot
     * 트리거를 보내면 SCD41이 매번 send_cmd(0x219D) ESP_ERR_INVALID_RESPONSE로 실패함. 정지
     * 명령 처리에 필요한 안정화 시간(scd41.c의 scd41_init() 내부, 자신의 stop_periodic 호출
     * 뒤에 두는 1000ms와 동일 근거)이 필요 — 예전엔 이 사이에 "페어링 대기(최대 20초)"라는
     * 버그가 있어서 우연히 시간이 충분히 벌어져 이 문제가 가려져 있었을 뿐, 그 버그를 고치고
     * 측정을 캐스크보다 앞으로 옮기면서 실제로 드러남 */
    vTaskDelay(pdMS_TO_TICKS(1000));

    /* 2026-09-05(사용자 지시: "캠의 주기촬영과 같은 방법으로 구현해야 되는 건데") — 측정
     * 시도 자체는 콘/캐스크와 완전히 무관하게, 하드웨어 초기화 직후 곧장 함(캠의 주기촬영
     * esp_timer가 WAKE_HELLO 타이밍과 무관하게 독립적으로 도는 것과 동일 원칙). 다만 실제
     * 트리거 여부는 게이팅됨(사용자 지시: "전력소모 큰 센서가 있어서... 측정주기에만
     * 측정해야되") — 최초(한 번도 성공한 적 없음)이거나 마지막 실측정 이후 측정주기만큼
     * 실제 시간이 지났을 때만 SCD41을 건드리고, 아니면 직전 캐시값을 그대로 재사용 */
    uint32_t measure_period_sec = esp_now_node_get_sample_interval_sec();
    bool due_for_measurement = (s_measurement_id == 0) ||
                               (s_seconds_since_last_measurement >= measure_period_sec);
    if (due_for_measurement) {
        float fresh_vals[SENSOR_CHAN_COUNT] = { 0 };
        bool fresh_ok = measure_scd41(fresh_vals);
        if (fresh_ok) {
            memcpy(s_cached_vals, fresh_vals, sizeof(fresh_vals));
            for (int i = 0; i < SENSOR_CHAN_COUNT; i++) s_cached_chan_ok[i] = 1;
            s_measurement_id++;
            s_seconds_since_last_measurement = 0;
        } else {
            ESP_LOGW(TAG, "SCD41 판독 실패 — 직전 캐시값(측정ID=%u) 재사용", (unsigned)s_measurement_id);
        }
    } else {
        ESP_LOGI(TAG, "측정주기(%us) 미도달(경과 %us) — 재측정 생략, 캐시값(측정ID=%u) 재사용",
                 (unsigned)measure_period_sec, (unsigned)s_seconds_since_last_measurement,
                 (unsigned)s_measurement_id);
    }

    int batt_mv = battery_read_mv();
    bool batt_ok = (batt_mv > 0);
    bool powered = vin_indicates_usb();
    if (batt_ok) maybe_learn_full_mv(powered, batt_mv);
    int batt_pct = batt_ok ? battery_mv_to_pct(batt_mv) : 0;

    /* 충전 중(USB 전원)이 최우선 — sensor_node.c와 동일 우선순위. "판독 실패" LED는
     * 이번 부팅 판독이 아니라 "한 번도 측정 성공한 적 없음"(measurement_id==0) 기준 —
     * 캐시된 값이 있으면 이번에 재측정 실패해도 굳이 경보 패턴까지는 안 씀 */
    if (powered) {
        status_led_init(BSP_C3_LED_BLUE);
        status_led_set_pattern(BSP_C3_LED_BLUE, LED_PATTERN_ON);
    } else if (s_measurement_id == 0) {
        status_led_init(BSP_C3_LED_BLUE);
        status_led_set_pattern(BSP_C3_LED_BLUE, LED_PATTERN_BURST_TRIPLE);
    } else if (batt_ok) {
        status_led_init(BSP_C3_LED_BLUE);
        status_led_set_pattern(BSP_C3_LED_BLUE, batt_pct_to_led_pattern(batt_pct));
    }

    uint16_t batt_mv_u16 = (batt_ok && batt_mv < 65536) ? (uint16_t)batt_mv : 0;

    /* 최소 WiFi 브링업(cam_node.c와 완전히 동일 시퀀스) — SSID 접속/HTTP서버 없음,
     * ESP-NOW만을 위한 라디오 초기화 */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    ESP_LOGI(TAG, "WiFi 모뎀슬립 설정: %s", esp_err_to_name(ps_err));

    /* 2026-09-05 — 캠은 스피커 이벤트훅을 통해 간접 연결하지만(cam_speaker 의존), 센스는
     * 스피커가 없으니 채널스캔 스윕완료 훅에 직접 연결 */
    esp_now_channelsync_set_event_hooks(NULL, NULL, NULL, on_scan_sweep_done);
    s_wake_recheck_sem = xSemaphoreCreateBinary();

    esp_now_node_set_status_led(BSP_C3_LED_GREEN);
    esp_now_node_init(SENSOR_KIND_CURRENT, SENSOR_CHAN_COUNT, s_chan_types);

    ESP_LOGI(TAG, "헤드리스 Sens 노드 준비 완료 (kind=%d, channels=%d, 측정ID=%u)",
             SENSOR_KIND_CURRENT, SENSOR_CHAN_COUNT, (unsigned)s_measurement_id);

    /* CAM의 app_main 웨이크 루프와 완전히 동일 구조(2026-09-05 재수정, cam_node.c 참고) —
     * esp_now_node_init() 직후 esp_now_node_report_reading()을 한 번 동기 호출하는 것 자체가
     * 캠의 "esp_now_cam_init() 끝에서 esp_now_cam_reconnect() 호출"과 동일 타이밍(esp_now_
     * node_cask.c 파일 헤더 주석 참고) — 그래서 이 결과(paired_now)가 항상 정확함 */
    bool paired_now = esp_now_node_report_reading(SENSOR_CHAN_COUNT, s_cached_chan_ok, s_cached_vals,
                                                   s_measurement_id, 0, batt_mv_u16);
    uint32_t sleep_sec = ESP_NOW_NODE_UNPAIRED_RETRY_SEC;

    for (;;) {
        if (!paired_now) {
            /* 알려진 허브가 없었거나 패스트패스가 실패해서 지금 폴백 스캔 중(백그라운드 —
             * esp_now_node_report_reading()의 폴백 분기 참고) — PAIR_REQUEST가 비동기로
             * 도착할 때까지 이벤트 기반 대기, 스윕 한 바퀴 다 돌 때까지만 */
            while (!esp_now_node_is_paired() && !s_sweep_completed) {
                xSemaphoreTake(s_wake_recheck_sem, pdMS_TO_TICKS(1000));
            }
            if (!esp_now_node_is_paired()) {
                sleep_sec = next_unpaired_retry_sleep_sec();
                s_unpaired_backoff_elapsed_sec += sleep_sec;
                ESP_LOGW(TAG, "폴백 스윕 완료 — Cntl 못 찾음, %us 후 재시도", (unsigned)sleep_sec);
                break;
            }
        }
        s_unpaired_backoff_elapsed_sec = 0;  /* 페어링 성공 — 백오프 리셋 */

        /* CASK 대기 — CONFIG부터 SLEEP_NOW까지, 이벤트 기반으로 기다리되 SENS_CASK_TIMEOUT_MS
         * 전체 상한을 둠(캠과 동일 원칙 — "WAKE_HELLO 성공 판정을 CASK 전체로 넓힌 것") */
        uint32_t cask_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
        bool cask_timed_out = false;
        while (!s_sleep_now_requested) {
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            if (now_ms - cask_start_ms >= SENS_CASK_TIMEOUT_MS) {
                ESP_LOGW(TAG, "CASK 미완주(%ums 경과, SLEEP_NOW 못 받음) — WAKE_HELLO_SENS부터 재시도",
                         (unsigned)SENS_CASK_TIMEOUT_MS);
                cask_timed_out = true;
                break;
            }
            xSemaphoreTake(s_wake_recheck_sem, pdMS_TO_TICKS(1000));
        }

        if (cask_timed_out) {
            paired_now = esp_now_node_report_reading(SENSOR_CHAN_COUNT, s_cached_chan_ok, s_cached_vals,
                                                       s_measurement_id, 0, batt_mv_u16);
            continue;
        }

        /* 2026-09-05(사용자 지시로 정정) — 딥슬립 실제 시간은 캠과 동일하게 SLEEP_NOW.
         * sleep_sec 그 자체를 그대로 씀(노드 쪽은 특별취급 없음). "센스는 자기 측정주기마다
         * 깨야 한다"는 요구는 콘 쪽(esp_now_hub.c의 send_cask_sleep_now())이 이 노드의
         * 측정주기를 기준값으로 써서 sleep_sec을 계산하는 것으로 충족됨 — 그래서 여기서
         * 받는 값이 곧 측정주기이고, 별도로 로컬 값을 다시 끼워 넣지 않음(그래야 실수로
         * "실제 잠든 시간 > 측정주기"가 되는 경로 자체가 없어짐) */
        if (esp_now_node_get_last_sleep_sec() != 0) {
            sleep_sec = esp_now_node_get_last_sleep_sec();
            break;
        }

        /* Live 루프 — 이번 CASK가 걸린 시간이 페이싱 기준보다 짧으면 나머지를 채워 대기 */
        uint32_t elapsed_ms = (uint32_t)(esp_timer_get_time() / 1000) - cask_start_ms;
        if (elapsed_ms < SENS_CASK_LIVE_PACE_MS) {
            vTaskDelay(pdMS_TO_TICKS(SENS_CASK_LIVE_PACE_MS - elapsed_ms));
        }
        paired_now = esp_now_node_report_reading(SENSOR_CHAN_COUNT, s_cached_chan_ok, s_cached_vals,
                                                   s_measurement_id, 0, batt_mv_u16);
    }

    /* 이번에 실제로 잠들 시간만큼 "마지막 실측정 이후 경과시간"에 더해둠 — 다음 부팅에서
     * 위 due_for_measurement 판단의 기준이 됨(사용자 지시: 측정주기 게이팅) */
    s_seconds_since_last_measurement += sleep_sec;

    esp_now_node_note_sleep_entry();
    ESP_LOGI(TAG, "딥슬립 진입: %us 후 웨이크", (unsigned)sleep_sec);
    esp_sleep_enable_timer_wakeup((uint64_t)sleep_sec * 1000000ULL);
    esp_deep_sleep_start();
}
