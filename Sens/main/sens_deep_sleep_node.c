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
 * (esp_now_node_get_last_sleep_sec()) — 콘 쪽(node_hub.c의 send_cask_sleep_now())이
 * MIN(응답성, 이 센스의 측정주기)을 계산해서 보내므로, 응답성<측정주기일 때는 측정주기보다
 * 자주 깨지만(콘의 제어 기회 확보용) 위 게이팅 덕분에 그 웨이크들에서 재측정하지는 않음.
 * 센서종류/채널구성(sensor_kind/chan_type)은 매 캐스크가 아니라 페어링(PAIR_ACK) 때 1회만
 * 보냄(esp_now_link.h의 esp_now_pair_ack_t 참고) — esp_now_node_init()에 그때 한 번 넘김.
 */

#include <string.h>
#include <stdlib.h>
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_pm.h"
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
#include "rwdt_guard.h"
#include "rgb_status.h"
#include "sensor_wait.h"   /* 2026-10-23 — 센서 타이밍 대기는 모두 sensor_wait_ms */
#include "esp_mac.h"       /* 2026-10-27 — 교차 실험의 보드 구분(esp_efuse_mac_get_default) */
#include "esp_rtc_time.h"  /* 2026-10-27 — 딥슬립을 넘는 시각(esp_rtc_get_time_us) — SCD41 측정 시작 시각 */

/* 2026-09-12(사용자 지시 — "장치 종류가 달라도 프로토콜/루틴은 동일해야 한다") — 이 파일은
 * 이제 SCD41 전용이 아니라 캐스크(딥슬립) 구조를 쓰는 모든 센서 종류의 공용 루틴이다.
 * 센서 종류는 sensor_node.c와 동일한 패턴(#if CONFIG_SENS_SENSOR_*)으로만 갈리고, CASK
 * 왕복/딥슬립/측정주기 게이팅 등 나머지 전부는 완전히 공용이다. */
#if CONFIG_SENS_SENSOR_SCD41
    #include "scd41.h"
    #define SENSOR_KIND_CURRENT  SENSOR_KIND_SCD41
    #define SENSOR_CHAN_COUNT    3
    static const uint8_t s_chan_types[SENSOR_CHAN_COUNT] = {
        SENSOR_CHAN_CO2_PPM, SENSOR_CHAN_TEMP_C, SENSOR_CHAN_HUMI_PCT,
    };
#elif CONFIG_SENS_SENSOR_MQ137
    #include "mq137.h"
    #define SENSOR_KIND_CURRENT  SENSOR_KIND_MQ137
    #define SENSOR_CHAN_COUNT    1
    static const uint8_t s_chan_types[SENSOR_CHAN_COUNT] = {
        SENSOR_CHAN_NH3_PPM,   /* 2026-09-12 — 아직 진짜 ppm 아님, mq137.h 참고 */
    };
#elif CONFIG_SENS_SENSOR_SC05
    #include "sc05.h"
    #define SENSOR_KIND_CURRENT  SENSOR_KIND_SC05
    #define SENSOR_CHAN_COUNT    1
    static const uint8_t s_chan_types[SENSOR_CHAN_COUNT] = {
        SENSOR_CHAN_NH3_PPM,   /* 2026-09-15 — YYS SC05-NH3, 전기화학식, sc05.h 참고 */
    };
#elif CONFIG_SENS_SENSOR_SHT45
    #include "sht4x.h"
    #define SENSOR_KIND_CURRENT  SENSOR_KIND_SHT45
    #define SENSOR_CHAN_COUNT    2
    static const uint8_t s_chan_types[SENSOR_CHAN_COUNT] = {
        SENSOR_CHAN_TEMP_C, SENSOR_CHAN_HUMI_PCT,
    };
#elif CONFIG_SENS_SENSOR_PT100
    #include "max31865.h"
    #define SENSOR_KIND_CURRENT  SENSOR_KIND_PT100
    #define SENSOR_CHAN_COUNT    1
    static const uint8_t s_chan_types[SENSOR_CHAN_COUNT] = {
        SENSOR_CHAN_TEMP_C,    /* 2026-09-30 — Agar 접촉 온도(3선 PT100 + MAX31865) */
    };
#elif CONFIG_SENS_SENSOR_SHT45_DS2
    /* 2026-10-23(사용자 설계 — 고온 보드, 센서 중심 구조) — 센서 3개: SHT45(공기 온습도) + DS18B20 두 개(배지 온도, 핀 하나씩).
     * 보드의 sensor_kind는 옛 콘 호환용 대표값일 뿐 — 실제 구성은 s_sensors(페어링 응답의 센서 목록) */
    #include "sht4x.h"
    #include "ds18b20_sensor.h"
    #define SENSOR_KIND_CURRENT  SENSOR_KIND_SHT45
    #define SENSOR_CHAN_COUNT    4
    static const uint8_t s_chan_types[SENSOR_CHAN_COUNT] = {
        SENSOR_CHAN_TEMP_C, SENSOR_CHAN_HUMI_PCT, SENSOR_CHAN_TEMP_C, SENSOR_CHAN_TEMP_C,
    };
    static const esp_now_sensor_desc_t s_sensors[] = {
        { .kind = SENSOR_KIND_SHT45,   .port = 0, .chan_first = 0, .chan_n = 2 },
        { .kind = SENSOR_KIND_DS18B20, .port = 1, .chan_first = 2, .chan_n = 1 },   /* GPIO4 */
        { .kind = SENSOR_KIND_DS18B20, .port = 2, .chan_first = 3, .chan_n = 1 },   /* GPIO5 */
    };
#else
    #error "sens_deep_sleep_node.c supports only SCD41/MQ137/SC05/SHT45/PT100/SHT45_DS2 - add a branch here to move another sensor to CASK"
#endif

#if !CONFIG_SENS_SENSOR_SHT45_DS2
/* 2026-10-23(센서 중심 구조) — 센서 하나짜리 보드: 포트 0에 그 센서, 채널 전부 */
static const esp_now_sensor_desc_t s_sensors[] = {
    { .kind = SENSOR_KIND_CURRENT, .port = 0, .chan_first = 0, .chan_n = SENSOR_CHAN_COUNT },
};
#endif

static const char *TAG = "sens_deep_sleep_node";

#define BATT_FULL_MV_FIXED      4200.0f   /* 2026-10-23 — 고정(학습은 나중에) */

#if CONFIG_SENS_SENSOR_SCD41
/* SCD41 single-shot 측정 소요시간(~5초) 대비 여유 — 트리거 후 이 안에 완료 안 되면 실패로
 * 처리(cam_node.c류 "무한정 안 기다린다" 원칙과 동일). 센서마다 측정 소요시간이 다름
 * (사용자 지시: 온습도~100ms, CO2~1초, 암모니아~3초) — 이 값은 SCD41 전용, MQ137은 AO/DO를
 * 그냥 즉시 읽으면 되므로(트리거+폴링 자체가 없음) 이 타임아웃이 필요 없음 */
/* 2026-10-05(사용자 결정 — SCD41 개선 1단계) — Sensirion 공식 드라이버(measureAndReadSingleShot)처럼 5초 뒤부터 100ms마다,
 * 준비될 때까지 기다림. 공식은 제한 없음 — 여기선 RWDT 예산·전력 때문에 상한 15초(measure_scd41 안). 예전 6초에 포기 → 다음 측정이
 * 남은 결과를 읽던 패턴 */
/* 2026-09-14(사용자 지적 "SCD41 값 못읽은 거 왜 안 찾았냐" 재조사 — Sensirion SCD4x
 * 공식 매뉴얼 v1.7 확인) — measure_single_shot(0x219d)의 실행시간은 정확히 5000ms
 * (Table 9)이고, Section 3.4: "실행시간이 명시된 명령은 그 실행시간 동안 추가 명령을
 * 보내면 안 된다." 아래 measure_scd41()이 트리거 직후부터 200ms 간격으로
 * GET_DATA_READY_STATUS를 계속 보내던 것은 이 5초 구간 동안 명령을 보내지 말라는
 * 규정 위반 — Section 3.11(싱글샷) 공식 절차도 "트리거 -> 실행시간만큼 대기 -> 곧장
 * read_measurement"이지 폴링이 아님(get_data_ready_status는 언급조차 없음, periodic
 * 모드 패턴을 그대로 잘못 가져다 쓴 것). 5000ms를 먼저 그대로 흘려보내고 그 이후에만
 * 폴링하도록 수정 */
#define SCD41_MEASURE_MIN_WAIT_MS 5000
#endif

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

static adc_oneshot_unit_handle_t s_adc_unit = NULL;   /* 배터리 ADC 유닛 — MQ137 AO가 같이 씀 */

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

/* 2026-09-29(사용자 지시 — SCD41이 밤새 측정을 못 했는데 원인을 알 수 없었음) — 마지막 측정 시도 결과
 * (sensor_fault_t, 0=성공)와 연속 실패 횟수. 딥슬립을 넘어 유지되고, 다음 WAKE_HELLO_SENS에 실려 콘으로 감 */
static RTC_DATA_ATTR uint8_t  s_sensor_fault                      = 0;
static RTC_DATA_ATTR uint16_t s_sensor_fail_streak                = 0;
/* 연속 실패가 이 배수가 될 때마다 SCD41 재초기화(stop -> reinit, 데이터시트 3.9.5) */
#define SENS_REINIT_EVERY_FAILS 2  /* 2026-10-06(사용자 결정) — 2회 연속 실패마다 reinit(예전 3회) */
/* measure_sensor()가 실패했을 때 그 원인 — attempt_one_measurement()가 채움 */
static uint8_t s_attempt_fault = 0;
/* 2026-10-23(사용자 결정 — 센서 중 하나라도 성공하면 측정 성공, 실패한 센서는 따로 보고) — 센서별(s_sensors 순서) 측정 상태.
 * 위 s_sensor_fault/streak는 보드 전체(모든 센서가 실패했을 때만 실패) */
#define SENS_SENSOR_N  (sizeof(s_sensors) / sizeof(s_sensors[0]))
static RTC_DATA_ATTR uint8_t  s_sen_fault[ESP_NOW_MAX_SENSORS];
static RTC_DATA_ATTR uint16_t s_sen_streak[ESP_NOW_MAX_SENSORS];
static uint8_t s_attempt_sen_fault[ESP_NOW_MAX_SENSORS];   /* 이번 시도의 센서별 실패 원인 */

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

/* 2026-10-23(사용자 결정) — 외부 보드(LED 두 개, VIN 분압)를 모든 센스에서 뗌: USB 전원 감지(vin_indicates_usb)와
 * 그걸 전제로 한 만충 전압 학습(maybe_learn_full_mv), 파랑 LED 배터리 패턴, WS2812 시험 코드를 지움. 표시는 rgb_status
 * (내장 RGB, 단색). 만충 전압은 일단 고정(BATT_FULL_MV_FIXED) — 학습은 나중에(사용자 "시간되면") */
/* 2026-09-06(사용자 지시) — CONFIG_PM_ENABLE=y + FREERTOS_USE_TICKLESS_IDLE=y(자동
 * 라이트슬립)가 켜져 있어서, SCD41 측정 폴링 중(vTaskDelay 사이사이) 라이트슬립에 들어갔다
 * 나왔다 하면서 I2C 상태가 깨질 가능성 — 측정 시작~완료까지는 라이트슬립 자체를 못 하게
 * 락을 잡음(이 락은 esp_deep_sleep_start()의 진짜 딥슬립과는 무관, 자동 라이트슬립만 막음) */
/* 2026-10-01(할 일 E) — 09-27 sdkconfig를 defaults 기준으로 바꾸며 CONFIG_PM_ENABLE이 꺼졌음. 그 빌드에선 자동 라이트슬립
 * 자체가 없어 막을 게 없고, esp_pm_lock_create()가 매번 실패해 측정마다 경고만 찍혔음 → PM이 켜진 빌드에서만 락을 씀 */
#if CONFIG_PM_ENABLE
static esp_pm_lock_handle_t s_no_light_sleep_lock = NULL;

static void pm_lock_no_light_sleep_acquire(void)
{
    if (!s_no_light_sleep_lock) {
        if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "sens_measure", &s_no_light_sleep_lock) != ESP_OK) {
            ESP_LOGW(TAG, "PM lock create failed - measuring without light-sleep guard");
            return;
        }
    }
    esp_pm_lock_acquire(s_no_light_sleep_lock);
}

static void pm_lock_no_light_sleep_release(void)
{
    if (s_no_light_sleep_lock) esp_pm_lock_release(s_no_light_sleep_lock);
}
#else
static void pm_lock_no_light_sleep_acquire(void) {}
static void pm_lock_no_light_sleep_release(void) {}
#endif

#if CONFIG_SENS_SENSOR_SCD41
/* SCD41 single-shot 판독 — 트리거 후 최대 15초까지 블로킹 폴링.
 * 예전 sensor_node.c는 이걸 여러 esp_timer 틱에 걸쳐 논블로킹으로 했는데(계속실행 전제),
 * 딥슬립은 부팅마다 한 번뿐이라 "다음 틱"이 없음 — CAM의 촬영 대기 패턴과 동일하게
 * 한 부팅 안에서 블로킹으로 끝냄(scd41.c의 trigger/poll API 자체는 안 건드림) */
static bool measure_scd41(float out[SENSOR_CHAN_COUNT])
{
    /* 2026-10-05(실기 — 플래시 직후 첫 명령 NACK) — 이 보드는 SCD41 전원이 3.3V 직결이라 ESP만 리셋되면 센서는 그대로.
     * 리셋 직전에 단발 측정을 시작했다면 그 실행 시간(5초) 동안은 명령을 안 받음 → 이 부팅의 첫 명령이 NACK이면
     * 그만큼 기다렸다 한 번만 더(딥슬립 웨이크는 이전 측정이 이미 끝나 있어 해당 없음) */
    static bool s_first_cmd_this_boot = true;
    scd41_wake_up();  /* 2026-10-06(시험 — 공식 loop() 첫 줄 wakeUp()) */
    scd41_clear_fault();
    bool trig_ok = scd41_trigger_single_shot();
    if (!trig_ok && s_first_cmd_this_boot) {
        ESP_LOGW(TAG, "SCD41 first command NACK - previous measurement may still be running, retry in %ums",
                 (unsigned)SCD41_MEASURE_MIN_WAIT_MS);
        sensor_wait_ms(SCD41_MEASURE_MIN_WAIT_MS);
        scd41_clear_fault();
        trig_ok = scd41_trigger_single_shot();
    }
    s_first_cmd_this_boot = false;
    if (!trig_ok) {
        s_attempt_fault = SENSOR_FAULT_CMD_NACK;
        return false;
    }

    /* 2026-10-06(사용자 결정 — Sensirion 단발 예제 그대로 시험) — exampleScd41SingleShot.ino loop():
     * wakeUp() → measureSingleShot()(5초, 결과 버림) → measureAndReadSingleShot()(트리거 → 5초 → 100ms마다 data-ready, 준비되면 읽기).
     * wake_up은 이 함수 맨 앞, 위의 트리거가 "버리는 측정". 공식은 무한정 기다림 — RWDT 예산 때문에 두 번째 트리거 뒤 15초 상한(시험, 임시) */
    sensor_wait_ms(SCD41_MEASURE_MIN_WAIT_MS);  /* 첫 측정(버림) — 공식 measureSingleShot() 안의 delay(5000) */
    scd41_clear_fault();
    bool ok = false;
    uint32_t waited_ms = 0;
    if (!scd41_trigger_single_shot()) {
        s_attempt_fault = SENSOR_FAULT_CMD_NACK;
    } else {
        sensor_wait_ms(SCD41_MEASURE_MIN_WAIT_MS);
        waited_ms = SCD41_MEASURE_MIN_WAIT_MS;
        bool done = false;
        while (!done && waited_ms <= 15000) {
            int co2 = 0;
            float t = 0.0f, h = 0.0f;
            bool read_ok = false;
            if (scd41_poll_single_shot(&co2, &t, &h, &read_ok)) {
                done = true;
                ok = read_ok;
                if (ok) { out[0] = (float)co2; out[1] = t; out[2] = h; }
                else s_attempt_fault = scd41_last_fault() ? (uint8_t)scd41_last_fault() : SENSOR_FAULT_READ;
            } else {
                sensor_wait_ms(100);
                waited_ms += 100;
            }
        }
        if (!done) s_attempt_fault = scd41_last_fault() ? (uint8_t)scd41_last_fault() : SENSOR_FAULT_NOT_READY;
    }
    return ok;
}
#endif

/* 2026-10-13(사용자 지시 — 펌웨어 다시 빌드 없이 SCD41 보정 이력 되돌리기) — SENS_CALIB_RESET 처리. 수신 콜백에서 불리므로
 * 실행(약 2.5초)은 별도 태스크로, 끝나면 결과 응답. 같은 seq 재시도면 다시 하지 않고 저장해 둔 결과로 응답(RTC — 딥슬립에도 유지).
 * 측정·딥슬립 전에 calib_wait_idle()로 끝나기를 기다림(I2C를 같이 쓰지 않게, 실행 중에 잠들지 않게) */
static volatile bool s_calib_busy = false;
static RTC_DATA_ATTR bool     s_calib_done_valid = false;
static RTC_DATA_ATTR uint16_t s_calib_done_seq = 0;
static RTC_DATA_ATTR uint8_t  s_calib_done_result = 0, s_calib_done_asc = 0xFF;
static uint16_t s_calib_run_seq = 0;

#if CONFIG_SENS_SENSOR_SCD41
static void calib_task(void *arg)
{
    (void)arg;
    uint8_t asc = 0xFF;
    bool ok = scd41_factory_reset(&asc);
    scd41_power_down();   /* 2026-10-27 — 측정할 때만 깨움(진행 중이던 측정은 초기화가 멈췄으므로 그 회차는 실패로 남음) */
    s_calib_done_seq = s_calib_run_seq;
    s_calib_done_result = ok ? SENS_CALIB_RESULT_OK : SENS_CALIB_RESULT_CMD_FAIL;
    s_calib_done_asc = asc;
    s_calib_done_valid = true;
    s_calib_busy = false;
    ESP_LOGW(TAG, "Calibration reset seq=%u result=%u asc=%u", (unsigned)s_calib_done_seq, (unsigned)s_calib_done_result, (unsigned)asc);
    esp_now_node_send_calib_ack(s_calib_done_seq, s_calib_done_result, s_calib_done_asc);
    vTaskDelete(NULL);
}
#endif

void sens_node_calib_reset_request(uint16_t seq)
{
    if (s_calib_done_valid && seq == s_calib_done_seq) {   /* 재시도 — 이미 실행함 */
        esp_now_node_send_calib_ack(seq, s_calib_done_result, s_calib_done_asc);
        return;
    }
    if (s_calib_busy) return;   /* 실행 중 재시도 — 끝나면 응답 */
#if CONFIG_SENS_SENSOR_SCD41
    s_calib_busy = true;
    s_calib_run_seq = seq;
    if (xTaskCreate(calib_task, "calib", 3072, NULL, 5, NULL) != pdPASS) {
        s_calib_busy = false;
        esp_now_node_send_calib_ack(seq, SENS_CALIB_RESULT_BUSY, 0xFF);
    }
#else
    esp_now_node_send_calib_ack(seq, SENS_CALIB_RESULT_UNSUPPORTED, 0xFF);
#endif
}

static void calib_wait_idle(void)
{
    for (int i = 0; i < 60 && s_calib_busy; i++) vTaskDelay(pdMS_TO_TICKS(100));   /* 최대 6초(실행 약 2.5초) */
}

/* 2026-09-12(사용자 지시 — "같은 루틴, 변수만 다르게") — 센서 종류별 실제 판독 하나로
 * 모음. SCD41은 트리거+블로킹폴링(위), MQ137은 이미 히터가 외부전원으로 항상 예열돼
 * 있어서(배터리가 아니라 상시전원 전제) 트리거/대기 없이 즉시 읽으면 됨 */
/* 2026-10-23 — 채널마다 성공 여부(ok[])를 채움. 반환 = 모든 채널 성공. 센서 하나짜리는 전부 같은 결과 */
#if !CONFIG_SENS_SENSOR_SHT45_DS2
static bool measure_sensor_one(float out[SENSOR_CHAN_COUNT]);
#endif
static bool measure_sensor(float out[SENSOR_CHAN_COUNT], uint8_t ok[SENSOR_CHAN_COUNT])
{
    calib_wait_idle();   /* 2026-10-13 */
#if CONFIG_SENS_SENSOR_SHT45_DS2
    bool a = sht4x_read(&out[0], &out[1]);
    ok[0] = ok[1] = a;
    if (!a) s_attempt_fault = s_attempt_sen_fault[0] = SENSOR_FAULT_READ;
    const gpio_num_t pins[2] = { BSP_C3_DS18B20_PIN_A, BSP_C3_DS18B20_PIN_B };
    bool all = a;
    for (int i = 0; i < 2; i++) {
        ds18b20_sensor_result_t r = ds18b20_sensor_read_pin(pins[i], &out[2 + i]);
        ok[2 + i] = (r == DS18B20_SENSOR_OK);
        if (r != DS18B20_SENSOR_OK) {
            all = false;
            s_attempt_fault = (r == DS18B20_SENSOR_ERR_NO_DEVICE) ? SENSOR_FAULT_CMD_NACK
                            : (r == DS18B20_SENSOR_ERR_CRC) ? SENSOR_FAULT_CRC : SENSOR_FAULT_READ;
            s_attempt_sen_fault[1 + i] = s_attempt_fault;
        }
    }
    return all;
#else
    bool r = measure_sensor_one(out);
    for (int i = 0; i < SENSOR_CHAN_COUNT; i++) ok[i] = r;
    if (!r) s_attempt_sen_fault[0] = s_attempt_fault;
    return r;
#endif
}

#if !CONFIG_SENS_SENSOR_SHT45_DS2
static bool measure_sensor_one(float out[SENSOR_CHAN_COUNT])
{
#if CONFIG_SENS_SENSOR_SCD41
    return measure_scd41(out);
#elif CONFIG_SENS_SENSOR_MQ137
    bool do_alarm = false;
    bool ok = mq137_read(&out[0], &do_alarm);
    if (ok && do_alarm) ESP_LOGW(TAG, "MQ137 DO threshold exceeded");
    return ok;
#elif CONFIG_SENS_SENSOR_SC05
    /* Auto 모드(공장 기본값)라 트리거 없이 그냥 다음 프레임을 기다려서 잡음 —
     * 1초마다 쏘므로 2500ms면 최소 2번의 기회를 보장(sc05.h 파일 설명 참고) */
    return sc05_read(&out[0], 2500);
#elif CONFIG_SENS_SENSOR_SHT45
    /* sht4x_read()는 명령전송→10ms 대기→수신까지 자체 블로킹으로 끝남(트리거/폴링 분리 없음,
     * sht4x.c 참고) — SCD41처럼 별도 대기/폴링 루프가 필요 없음 */
    return sht4x_read(&out[0], &out[1]);
#elif CONFIG_SENS_SENSOR_PT100
    /* 1-shot 변환까지 블로킹 약 80ms(max31865.c). 칩 무응답 = 명령 NACK, RTD 고장 = 선 끊김/단락/범위 밖 */
    uint8_t fault_status = 0;
    max31865_result_t r = max31865_read(&out[0], &fault_status);
    if (r == MAX31865_OK) return true;
    s_attempt_fault = (r == MAX31865_ERR_FAULT) ? SENSOR_FAULT_RTD
                    : (r == MAX31865_ERR_NO_CHIP) ? SENSOR_FAULT_CMD_NACK : SENSOR_FAULT_READ;
    if (r == MAX31865_ERR_FAULT) ESP_LOGW(TAG, "MAX31865 RTD fault status 0x%02X", fault_status);
    return false;
#endif
}
#endif

/* 2026-09-06(사용자 지시) — 측정 시도 한 번(노랑 시작 표시 -> 실제 판독 -> 청록/보라
 * 결과 표시)을 통째로 감싼 헬퍼. 파워사이클 후 첫 워밍업 측정도 이 함수로 똑같이
 * 표시하고(사용자 지시: 구분 없이 LED 표시), 실측정도 이 함수로 함 — 한 사이클 안에서
 * 최대 두 번(워밍업+실측정) 불릴 수 있어서 elapsed는 호출부가 계속 누적하도록
 * 포인터로 받음 */
/* 2026-10-23 — 센서별 결과 반영(센서의 채널이 모두 성공 = 그 센서 성공). 콘에는 다음 상태 메시지에 실려 감 */
static void note_sensor_results(const uint8_t ok_ch[SENSOR_CHAN_COUNT])
{
    for (int i = 0; i < (int)SENS_SENSOR_N && i < ESP_NOW_MAX_SENSORS; i++) {
        bool ok = true;
        for (int c = s_sensors[i].chan_first; c < s_sensors[i].chan_first + s_sensors[i].chan_n && c < SENSOR_CHAN_COUNT; c++) {
            if (!ok_ch[c]) ok = false;
        }
        if (ok) {
            if (s_sen_streak[i] > 0) {
                ESP_LOGI(TAG, "Sensor port%u recovered after %u failed measurements", (unsigned)s_sensors[i].port,
                         (unsigned)s_sen_streak[i]);
            }
            s_sen_fault[i] = SENSOR_FAULT_NONE;
            s_sen_streak[i] = 0;
        } else {
            s_sen_fault[i] = s_attempt_sen_fault[i] ? s_attempt_sen_fault[i] : SENSOR_FAULT_OTHER;
            if (s_sen_streak[i] < UINT16_MAX) s_sen_streak[i]++;
            ESP_LOGW(TAG, "Sensor port%u read failed (fault=%u, streak=%u)", (unsigned)s_sensors[i].port,
                     (unsigned)s_sen_fault[i], (unsigned)s_sen_streak[i]);
        }
    }
    esp_now_node_set_sensor_status(s_sen_fault, s_sen_streak, (uint8_t)SENS_SENSOR_N);
}

static bool attempt_one_measurement(float out[SENSOR_CHAN_COUNT], uint8_t ok_ch[SENSOR_CHAN_COUNT], uint32_t *accum_elapsed_ms)
{
    memset(s_attempt_sen_fault, 0, sizeof(s_attempt_sen_fault));
    rgb_status_blink(RGB_STATUS_GREEN, 1);   /* 2026-10-23 — 측정 시작 G 1번 */
    uint32_t start_ms = (uint32_t)(esp_timer_get_time() / 1000);
    pm_lock_no_light_sleep_acquire();
    s_attempt_fault = SENSOR_FAULT_OTHER;  /* 원인을 구분하는 센서(SCD41)는 measure_* 안에서 덮어씀 */
    bool ok = measure_sensor(out, ok_ch);
    pm_lock_no_light_sleep_release();
    *accum_elapsed_ms += (uint32_t)(esp_timer_get_time() / 1000) - start_ms;
    note_sensor_results(ok_ch);
    bool any_ok = false;
    for (int i = 0; i < SENSOR_CHAN_COUNT; i++) any_ok |= ok_ch[i];
    /* 성공 G 2번 / 일부 성공 노랑 2번(사용자 결정 10-23) / 실패 R 1번 */
    if (ok) rgb_status_blink(RGB_STATUS_GREEN, 2);
    else if (any_ok) rgb_status_blink(RGB_STATUS_YELLOW, 2);
    else rgb_status_blink(RGB_STATUS_RED, 1);
    return ok;
}

/* 2026-09-06(사용자 지시 정정: "연결 전에 측정하지 말아", "광고를 하는 상황에서는
 * 측정하지 않는다") — 예전엔 캠의 "캐스크 타이밍과 무관하게 측정"을 센스에도 그대로
 * 확장해서 페어링 여부와 무관하게 부팅 직후 곧장 측정했는데, 이건 잘못된 확장이었음.
 * 캠은 로컬저장이 있어 연결 안 돼도 촬영 자체는 가치가 있지만, 센스 값은 보낼 곳(콘)이
 * 없으면 측정 자체가 무의미 — 그래서 이 함수는 반드시 "실제로 페어링된 게 확인된 뒤"에만
 * app_main에서 호출해야 함(광고/스캔 중엔 호출 안 함). 측정주기 게이팅(due_for_measurement)
 * 자체는 그대로 유지 */
/* 2026-10-05(실기 — SCD41을 붙였는데 측정이 계속 안 됨) — 측정 실패·회복 처리. 보통 경로와 Live 경로가 같이 씀.
 * 예전엔 Live 경로(응답성 "즉시")가 이걸 안 해서 콘에 실패가 보고되지 않았고, 3번마다 하는 SCD41 재초기화도 없어서
 * 한 번 측정이 안 되는 상태에 빠지면 Live 동안 6초마다 같은 실패만 반복했음 */
static void note_measure_recovered(void)
{
    if (s_sensor_fail_streak > 0) {
        ESP_LOGI(TAG, "Sensor recovered after %u failed measurements (last fault=%u)",
                 (unsigned)s_sensor_fail_streak, (unsigned)s_sensor_fault);
    }
    s_sensor_fault = SENSOR_FAULT_NONE;
    s_sensor_fail_streak = 0;
}

static void note_measure_failed(void)
{
    s_sensor_fault = s_attempt_fault;
    if (s_sensor_fail_streak < UINT16_MAX) s_sensor_fail_streak++;
    ESP_LOGW(TAG, "MEASMARK read failed (fault=%u, streak=%u) - reusing cached value (measID=%u)",
             (unsigned)s_sensor_fault, (unsigned)s_sensor_fail_streak, (unsigned)s_measurement_id);
#if CONFIG_SENS_SENSOR_SCD41
    /* 2026-09-29(Sensirion 데이터시트 3.9.5) — 연속 실패가 이어지면 센서가 갇힌 상태로 보고 재초기화.
     * 이 보드는 SCD41 전원을 끊을 수 없어서(3.3V 직결) 데이터시트의 마지막 단계(전원 껐다 켜기)는 못 함 */
    if (s_sensor_fail_streak % SENS_REINIT_EVERY_FAILS == 0) scd41_reinit();
#endif
}

#if CONFIG_SENS_SENSOR_SCD41
/* 2026-10-27(사용자 설계 — Docs/설계_센스측정운용_2026-10-27.md) — SCD41 측정 흐름.
 *  측정할 때가 되면 센서를 깨워 연속 측정을 시작만 하고(기다리지 않음) 평소처럼 통신·잠듦. 센서는 혼자 5초마다 잼.
 *  시작 후 SCD_SETTLE_US가 지난 뒤 깨는 회차에서 최신 값(8번째 무렵)을 읽고, 연속 측정을 멈추고 센서를 재움(power_down).
 *  응답 주기가 이보다 짧으면 그 뒤 응답 회차에서 읽고, 길면 그 시각에 맞춰 무선 없이 따로 깨서 읽음(s_scd_readwake).
 *  근거: 깨운 뒤 연속 측정 8번째 값이 최종값에서 최대 7ppm, 단발은 몇 번을 재도 흔들림(실험 10-26~27). 단발 측정은 안 씀 */
#define SCD_SETTLE_US  (41LL * 1000000)   /* 시작 명령 뒤 첫 값 약 1+5초, 8번째 약 41초 */
static RTC_DATA_ATTR bool     s_scd_pending = false;       /* 연속 측정 중(아직 안 읽음) */
static RTC_DATA_ATTR uint64_t s_scd_start_rtc_us = 0;      /* 시작 시각(RTC — 딥슬립을 넘어 이어짐) */
static RTC_DATA_ATTR bool     s_scd_readwake = false;      /* 다음 깨어남은 읽기 전용(무선 없음) */
static RTC_DATA_ATTR uint64_t s_scd_resp_due_rtc_us = 0;   /* 읽기 전용으로 깬 뒤 돌아갈 응답 깨어남 시각 */

static int64_t scd_settle_left_us(void)
{
    return (int64_t)(s_scd_start_rtc_us + SCD_SETTLE_US) - (int64_t)esp_rtc_get_time_us();
}

static void scd_finish(void);

/* 측정 시작 — 기다리지 않음. 실패하면 바로 실패로 처리 */
static bool scd_start(void)
{
    scd41_wake_up();
    if (!scd41_start_periodic()) {
        ESP_LOGW(TAG, "SCD41 periodic start failed");
        s_scd_pending = true;   /* scd_finish가 실패로 정리(읽기 실패 → 센서 재움 시도) */
        s_scd_start_rtc_us = esp_rtc_get_time_us() - SCD_SETTLE_US;
        scd_finish();
        return false;
    }
    s_scd_pending = true;
    s_scd_start_rtc_us = esp_rtc_get_time_us();
    rgb_status_blink(RGB_STATUS_GREEN, 1);   /* 측정 시작 G 1번 */
    ESP_LOGI(TAG, "SCD41 periodic started - read after %llds", (long long)(SCD_SETTLE_US / 1000000));
    return true;
}

/* 안정된 뒤 최신 값 읽기 → 멈춤 → 재움 → 결과 반영 */
static void scd_finish(void)
{
    float v[SENSOR_CHAN_COUNT] = { 0 };
    uint8_t ok[SENSOR_CHAN_COUNT] = { 0 };
    memset(s_attempt_sen_fault, 0, sizeof(s_attempt_sen_fault));
    int co2 = 0;
    float t = 0.0f, h = 0.0f;
    bool rd = false;
    scd41_clear_fault();
    for (int k = 0; k < 60 && !rd; k++) {   /* 다음 값이 준비될 때까지 최대 6초 */
        if (scd41_read(&co2, &t, &h)) rd = true; else sensor_wait_ms(100);
    }
    if (!rd) s_attempt_fault = scd41_last_fault() ? (uint8_t)scd41_last_fault() : SENSOR_FAULT_NOT_READY;
    scd41_stop_periodic_measurement();
    sensor_wait_ms(500);   /* stop 뒤 500ms는 명령을 안 받음(데이터시트) */
    if (s_measurement_id % 40 == 0) scd41_log_calibration("periodic");   /* 멈춘 뒤에만 읽을 수 있음 */
    bool pd = scd41_power_down();
    s_scd_pending = false;
    if (rd) {
        v[0] = (float)co2; v[1] = t; v[2] = h;
        for (int i = 0; i < SENSOR_CHAN_COUNT; i++) ok[i] = 1;
    } else {
        s_attempt_sen_fault[0] = s_attempt_fault;
    }
    note_sensor_results(ok);
    if (rd) {
        rgb_status_blink(RGB_STATUS_GREEN, 2);   /* 성공 G 2번 */
        note_measure_recovered();
        memcpy(s_cached_vals, v, sizeof(v));
        for (int i = 0; i < SENSOR_CHAN_COUNT; i++) s_cached_chan_ok[i] = ok[i];
        s_measurement_id++;
        s_seconds_since_last_measurement = 0;
        int t10 = (int)(t * 10.0f + 0.5f), h10 = (int)(h * 10.0f + 0.5f);
        ESP_LOGI(TAG, "MEASMARK measurement ok - measID=%u co2=%d temp=%d.%d humi=%d.%d (periodic, %llds after start) pd=%d",
                 (unsigned)s_measurement_id, co2, t10 / 10, t10 % 10, h10 / 10, h10 % 10,
                 (long long)((esp_rtc_get_time_us() - s_scd_start_rtc_us) / 1000000), (int)pd);
    } else {
        rgb_status_blink(RGB_STATUS_RED, 1);     /* 실패 R 1번 */
        note_measure_failed();
    }
}
#endif

static void do_gated_measurement_once(uint32_t *measurement_elapsed_ms)
{
#if CONFIG_SENS_SENSOR_SCD41
    /* 2026-10-27 — SCD41은 위 흐름(시작만 하고 나중에 읽음) */
    (void)measurement_elapsed_ms;
    if (s_scd_pending) {
        if (scd_settle_left_us() <= 0) scd_finish();
        return;
    }
    if (s_measurement_id == 0 || s_seconds_since_last_measurement >= esp_now_node_get_sample_interval_sec()) {
        scd_start();
    } else {
        ESP_LOGI(TAG, "Measurement period (%us) not reached (elapsed %us) - skipped (measID=%u)",
                 (unsigned)esp_now_node_get_sample_interval_sec(), (unsigned)s_seconds_since_last_measurement,
                 (unsigned)s_measurement_id);
    }
    return;
#endif
    uint32_t measure_period_sec = esp_now_node_get_sample_interval_sec();
    bool due_for_measurement = (s_measurement_id == 0) ||
                               (s_seconds_since_last_measurement >= measure_period_sec);
    if (due_for_measurement) {
        bool is_first_ever = (s_measurement_id == 0);
        float fresh_vals[SENSOR_CHAN_COUNT] = { 0 };
        uint8_t fresh_chok[SENSOR_CHAN_COUNT] = { 0 };
        bool fresh_ok = attempt_one_measurement(fresh_vals, fresh_chok, measurement_elapsed_ms);

#if CONFIG_SENS_SENSOR_SCD41
        if (fresh_ok && is_first_ever) {
            /* 2026-09-06(Sensirion 공식 문서: "파워사이클 후 첫 싱글샷 결과는 항상 버려야
             * 안정화됨") — 진짜 최초(측정ID==0)일 때만 해당, 딥슬립 웨이크는 센서 자체
             * 전원이 안 끊기므로 매번 적용 안 함. 워밍업 결과는 버리고 곧장 한 번 더
             * 측정해서 그 결과를 진짜 첫 값(측정ID=1)으로 씀 — 사용자 지시로 이 워밍업도
             * 진짜 측정과 동일하게 LED로 표시(구분 없음). MQ137은 히터가 외부전원으로 항상
             * 예열돼 있어서(배터리 아님) 이 워밍업-버림이 필요 없음 */
            ESP_LOGI(TAG, "SCD41 warm-up measurement done (discarded, first value after power cycle) - measuring again");
            fresh_ok = attempt_one_measurement(fresh_vals, fresh_chok, measurement_elapsed_ms);
        }
#else
        (void)is_first_ever;
#endif

        bool any_ok = false;
        for (int i = 0; i < SENSOR_CHAN_COUNT; i++) any_ok |= fresh_chok[i];
        if (any_ok) {
            /* 2026-10-23 — 센서가 여럿이면 성공한 채널만 새 값, 실패한 채널은 "값 없음"(콘이 그 센싱만 비움).
             * 하나라도 성공하면 보드는 측정 성공(사용자 결정) — 실패한 센서는 센서별 상태로(note_sensor_results) */
            note_measure_recovered();
            memcpy(s_cached_vals, fresh_vals, sizeof(fresh_vals));
            for (int i = 0; i < SENSOR_CHAN_COUNT; i++) s_cached_chan_ok[i] = fresh_chok[i];
            s_measurement_id++;
            s_seconds_since_last_measurement = 0;
            /* %f 안 씀(newlib-nano 미지원 — feedback_lvgl_no_percent_f 관례) — 정수부/소수부
             * 수동 분리(format_battery_display()와 동일 패턴). 로그 포맷은 채널 구성이
             * 센서마다 달라서(SCD41=3채널, MQ137=1채널) 그대로 센서별로 남겨둠(값 자체 —
             * 측정/전송 루틴은 위에서 이미 공용화됨) */
#if CONFIG_SENS_SENSOR_SCD41
            int temp_x10 = (int)(s_cached_vals[1] * 10.0f + 0.5f);
            int humi_x10 = (int)(s_cached_vals[2] * 10.0f + 0.5f);
            ESP_LOGI(TAG, "MEASMARK measurement ok - measID=%u co2=%d temp=%d.%d humi=%d.%d",
                     (unsigned)s_measurement_id, (int)s_cached_vals[0],
                     temp_x10 / 10, temp_x10 % 10, humi_x10 / 10, humi_x10 % 10);
            /* 2026-10-23(보정 의심) — 측정 40번마다(15초 주기면 10분) 보정 설정도 다시 읽음: 딥슬립 중에 바뀌는지 */
            if (s_measurement_id % 40 == 0) scd41_log_calibration("periodic");
#elif CONFIG_SENS_SENSOR_MQ137
            int ppm_x10 = (int)(s_cached_vals[0] * 10.0f + 0.5f);
            ESP_LOGI(TAG, "MEASMARK measurement ok - measID=%u NH3=%d.%dppm (provisional factor)",
                     (unsigned)s_measurement_id, ppm_x10 / 10, ppm_x10 % 10);
#elif CONFIG_SENS_SENSOR_SC05
            int sc05_ppm_x100 = (int)(s_cached_vals[0] * 100.0f + 0.5f);
            ESP_LOGI(TAG, "MEASMARK measurement ok - measID=%u NH3=%d.%02dppm",
                     (unsigned)s_measurement_id, sc05_ppm_x100 / 100, sc05_ppm_x100 % 100);
#elif CONFIG_SENS_SENSOR_SHT45
            int sht_temp_x10 = (int)(s_cached_vals[0] * 10.0f + 0.5f);
            int sht_humi_x10 = (int)(s_cached_vals[1] * 10.0f + 0.5f);
            ESP_LOGI(TAG, "MEASMARK measurement ok - measID=%u temp=%d.%d humi=%d.%d",
                     (unsigned)s_measurement_id, sht_temp_x10 / 10, sht_temp_x10 % 10,
                     sht_humi_x10 / 10, sht_humi_x10 % 10);
#elif CONFIG_SENS_SENSOR_SHT45_DS2
            int t0 = (int)(s_cached_vals[0] * 10.0f + 0.5f), h0 = (int)(s_cached_vals[1] * 10.0f + 0.5f);
            int a1 = (int)(s_cached_vals[2] * 100.0f + 0.5f), a2 = (int)(s_cached_vals[3] * 100.0f + 0.5f);
            ESP_LOGI(TAG, "MEASMARK measurement %s - measID=%u temp=%d.%d humi=%d.%d sub1=%s%d.%02d sub2=%s%d.%02d",
                     fresh_ok ? "ok" : "partial", (unsigned)s_measurement_id, t0 / 10, t0 % 10, h0 / 10, h0 % 10,
                     s_cached_chan_ok[2] ? "" : "(none) ", abs(a1) / 100, abs(a1) % 100,
                     s_cached_chan_ok[3] ? "" : "(none) ", abs(a2) / 100, abs(a2) % 100);
#elif CONFIG_SENS_SENSOR_PT100
            int pt_temp_x100 = (int)(s_cached_vals[0] * 100.0f + (s_cached_vals[0] < 0 ? -0.5f : 0.5f));
            ESP_LOGI(TAG, "MEASMARK measurement ok - measID=%u agar_temp=%s%d.%02d",
                     (unsigned)s_measurement_id, pt_temp_x100 < 0 ? "-" : "",
                     abs(pt_temp_x100) / 100, abs(pt_temp_x100) % 100);
#endif
        } else {
            note_measure_failed();
        }
    } else {
        ESP_LOGI(TAG, "Measurement period (%us) not reached (elapsed %us) - skipped, reusing cached value (measID=%u)",
                 (unsigned)measure_period_sec, (unsigned)s_seconds_since_last_measurement,
                 (unsigned)s_measurement_id);
    }
    ESP_LOGW(TAG, "MEASCHK due=%d id=%u elapsed=%us period=%us reset_reason=%d",
             (int)due_for_measurement, (unsigned)s_measurement_id,
             (unsigned)s_seconds_since_last_measurement, (unsigned)measure_period_sec,
             (int)esp_reset_reason());
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

    ESP_ERROR_CHECK(bsp_c3_pico_init());

    /* 2026-09-06(사용자 지시로 재확인) — single-shot만 쓰는 경로는 continuous(periodic)
     * 모드를 아예 시작하지도 않으므로, "혹시 periodic이 켜져있을까봐 방어적으로 끄는" 절차
     * 자체가 필요 없음(Sensirion 공식 문서: measure_single_shot은 start_periodic_measurement의
     * 대안이지 같이 쓰는 게 아니고, wake_up도 power_down을 실제로 건 적이 있을 때만 필요한데
     * 이 경로는 power_down을 안 씀). STOP_PERIODIC 자체는 없앴지만, 안정화 지연은 그 명령과
     * 무관하게 별도로 필요함 — Sensirion 공식 데이터시트: "전원 인가 후 idle 상태 진입까지
     * 1000ms 필요, 그래야 명령을 받을 준비가 됨"(싱글샷이든 뭐든 첫 명령 자체에 적용되는
     * 일반 요구사항). 예전엔 컨티뉴어스 왕복(약 7~9초)이 우연히 이 요구사항을 넘겨서
     * 가려주고 있었을 뿐 — 그 왕복을 없앤 지금은 이 지연을 그 목적 그대로 명시적으로 둠.
     * I2C 버스만 지금 준비해두고, 실제 측정은 아래에서 페어링 확인 후로 미룸(사용자 지시:
     * "연결 전에 측정하지 말아" — 캠의 "캐스크 타이밍과 무관"을 센스에 잘못 그대로 확장 적용한
     * 실수를 정정. 캠은 로컬저장이 있어 연결 여부와 무관하게 촬영 가치가 있지만, 센스 값은
     * 보낼 곳이 없으면 측정 자체가 무의미) */
#if CONFIG_SENS_SENSOR_SCD41
    if (!scd41_init_single_shot(BSP_C3_I2C_PORT, BSP_C3_I2C_SDA, BSP_C3_I2C_SCL)) {
        ESP_LOGW(TAG, "SCD41 init failed - check wiring (retry next cycle)");
    }
    /* 2026-10-27 — 전원 인가 뒤 1초(센서가 명령을 받을 준비) — 센서 전원은 딥슬립 동안에도 들어가 있으므로 전원 켤 때만 */
    if (esp_reset_reason() != ESP_RST_DEEPSLEEP) sensor_wait_ms(1000);
    /* 2026-10-05(사용자 결정 — SCD41 개선 2단계) — Sensirion 단발 예제처럼 시작할 때 센서를 깨끗한 상태로:
     * stop_periodic(500ms) → reinit(30ms). 09-06에 "단발엔 필요 없다"며 뺐던 것을 되돌림.
     * 딥슬립에서 깰 때마다 할지 전원 켤 때만 할지는 Live에서 효과 확인 뒤 결정(사용자) — 지금은 부팅마다 */
    /* 2026-10-06(사용자 결정) — Sensirion 단발 예제 setup()처럼 wakeUp → stop → reinit, ASC 끔 확인.
     * 딥슬립에서 깰 때는 하지 않음(센서 전원은 그대로라 상태가 이어짐) — 전원 켬·리셋 부팅 때만 */
    if (esp_reset_reason() != ESP_RST_DEEPSLEEP) {
        scd41_wake_up();
        scd41_reinit();
        scd41_read_identity();   /* 2026-10-18(시험) — 예전 순서 그대로(scd41.c 주석) */
        scd41_ensure_asc_off();  /* 2026-10-05(SCD41 개선 3단계) */
        scd41_log_calibration("power-on");   /* 2026-10-23(보정 의심) — 전원 부팅마다 */
#if !CONFIG_SENS_SCD41_PERIODIC_TEST && !CONFIG_SENS_SCD41_SINGLESHOT_LOOP_TEST && !CONFIG_SENS_SCD41_SLEEP_EXPERIMENT && \
    !CONFIG_SENS_SCD41_CROSSOVER_TEST && !CONFIG_SENS_SCD41_OUTDOOR_TEST
        s_scd_pending = false;
        s_scd_readwake = false;
        ESP_LOGI(TAG, "SCD41 power_down until first measurement: %d", (int)scd41_power_down());   /* 2026-10-27 — 측정할 때만 깨움 */
#endif
    }
#if !CONFIG_SENS_SCD41_PERIODIC_TEST && !CONFIG_SENS_SCD41_SINGLESHOT_LOOP_TEST && !CONFIG_SENS_SCD41_SLEEP_EXPERIMENT && \
    !CONFIG_SENS_SCD41_CROSSOVER_TEST && !CONFIG_SENS_SCD41_OUTDOOR_TEST
    /* 2026-10-27 — 읽기 전용 깨어남: 무선을 켜지 않고 값만 읽고 센서를 재운 뒤, 원래 응답 깨어남 시각까지 다시 잠 */
    if (esp_reset_reason() == ESP_RST_DEEPSLEEP && s_scd_readwake) {
        s_scd_readwake = false;
        int64_t left = scd_settle_left_us();
        if (left > 0) sensor_wait_ms((uint32_t)(left / 1000) + 1);
        if (s_scd_pending) scd_finish();
        int64_t to_us = (int64_t)s_scd_resp_due_rtc_us - (int64_t)esp_rtc_get_time_us();
        if (to_us < 1000000) to_us = 1000000;
        rgb_status_flush(1500);
        ESP_LOGI(TAG, "SCD41 read wake done - sleep %llds until response wake", (long long)(to_us / 1000000));
        esp_sleep_enable_timer_wakeup((uint64_t)to_us);
        esp_deep_sleep_start();
    }
#endif
#if CONFIG_SENS_SCD41_OUTDOOR_TEST
    {
        static uint16_t s_out[720];   /* 5초마다 1시간 */
        static uint32_t s_out_t[720];
        int n = 0;
        /* 2026-10-27(사용자 지시) — 새 보드 센서 공장 초기화(FRC·ASC 이력 지움) 뒤 실외 측정. 초기화가 ASC를 다시 켜므로 안에서 끄고 저장 */
        uint8_t asc_after = 0xFF;
        bool fr = scd41_factory_reset(&asc_after);
        ESP_LOGW(TAG, "OUT factory reset=%d asc_after=%u", (int)fr, (unsigned)asc_after);
        scd41_log_calibration("after-factory-reset");
        scd41_wake_up();
        ESP_LOGW(TAG, "OUT start periodic=%d", (int)scd41_start_periodic());
        int64_t last_dump = esp_timer_get_time();
        for (;;) {
            rwdt_guard_arm(60);
            sensor_wait_ms(500);
            int co2 = 0;
            float t = 0.0f, h = 0.0f;
            if (scd41_read(&co2, &t, &h)) {
                if (n < 720) { s_out[n] = (uint16_t)co2; s_out_t[n] = (uint32_t)(esp_timer_get_time() / 1000000); n++; }
                int t10 = (int)(t * 10.0f + 0.5f), h10 = (int)(h * 10.0f + 0.5f);
                ESP_LOGI(TAG, "OUT t=%lus co2=%d temp=%d.%d humi=%d.%d", (unsigned long)(esp_timer_get_time() / 1000000), co2,
                         t10 / 10, t10 % 10, h10 / 10, h10 % 10);
                rgb_status_blink(RGB_STATUS_GREEN, 1);
            }
            if (esp_timer_get_time() - last_dump > 30LL * 1000000) {   /* 쌓인 값 전부 다시(USB를 다시 꽂으면 받을 수 있게) */
                last_dump = esp_timer_get_time();
                for (int i = 0; i < n; i += 12) {
                    char line[160];
                    int k = snprintf(line, sizeof(line), "OUTDUMP %d:", i);
                    for (int j = i; j < n && j < i + 12 && k < (int)sizeof(line) - 12; j++)
                        k += snprintf(line + k, sizeof(line) - k, " %lu=%u", (unsigned long)s_out_t[j], s_out[j]);
                    ESP_LOGI(TAG, "%s", line);
                }
            }
        }
    }
#endif
#if CONFIG_SENS_SCD41_CROSSOVER_TEST
    {
        uint8_t mac[6] = { 0 };
        esp_efuse_mac_get_default(mac);
        int role = (mac[5] == 0x04) ? 0 : 1;   /* 0 = 첫 1시간 단발 */
        int64_t t_start = esp_timer_get_time();
        ESP_LOGW(TAG, "XO start mac ..%02X role=%d (hour1 %s, hour2 %s)", mac[5], role, role == 0 ? "single" : "periodic",
                 role == 0 ? "periodic" : "single");
        for (int cyc = 1; cyc <= 36; cyc++) {   /* 2026-10-27 — 3시간(18회)씩 교대(사용자) */
            int64_t due = t_start + (int64_t)(cyc - 1) * 600 * 1000000LL;
            while (esp_timer_get_time() < due) {
                rwdt_guard_arm(60);
                int64_t l = (due - esp_timer_get_time()) / 1000;
                sensor_wait_ms((uint32_t)(l > 30000 ? 30000 : (l > 0 ? l : 1)));
            }
            int hour = cyc <= 18 ? 1 : 2;   /* 1 = 첫 3시간, 2 = 다음 3시간 */
            bool single = (hour == 1) == (role == 0);
            rwdt_guard_arm(60);
            rgb_status_blink(RGB_STATUS_GREEN, 1);   /* 평소와 같게 — 측정 시작 G 1번(성공 G 2번 / 실패 R 1번은 아래) */
            int xo_fail = 0;
            scd41_wake_up();
            if (single) {
                for (int i = 1; i <= 13; i++) {   /* 2026-10-27 — 연속과 같은 13번(사용자) */
                    rwdt_guard_arm(60);
                    int co2 = 0;
                    float t = 0.0f, h = 0.0f;
                    bool rd = false, ok = false;
                    if (scd41_trigger_single_shot()) {
                        sensor_wait_ms(SCD41_MEASURE_MIN_WAIT_MS);
                        for (int k = 0; k < 50 && !rd; k++) {
                            if (scd41_poll_single_shot(&co2, &t, &h, &ok)) rd = true; else sensor_wait_ms(100);
                        }
                    }
                    if (rd && ok) ESP_LOGI(TAG, "XO h=%d mode=S cyc=%d i=%d co2=%d", hour, cyc, i, co2);
                    else { ESP_LOGW(TAG, "XO h=%d mode=S cyc=%d i=%d read failed", hour, cyc, i); xo_fail++; }
                }
            } else {
                scd41_start_periodic();
                int got = 0;
                int64_t tb = esp_timer_get_time();
                while (got < 13 && esp_timer_get_time() - tb < 120LL * 1000000) {
                    rwdt_guard_arm(60);
                    sensor_wait_ms(500);
                    int co2 = 0;
                    float t = 0.0f, h = 0.0f;
                    if (scd41_read(&co2, &t, &h)) {
                        got++;
                        ESP_LOGI(TAG, "XO h=%d mode=P cyc=%d i=%d co2=%d", hour, cyc, got, co2);
                    }
                }
                if (got < 13) xo_fail++;
                scd41_stop_periodic_measurement();
                sensor_wait_ms(500);
            }
            if (xo_fail) rgb_status_blink(RGB_STATUS_RED, 1); else rgb_status_blink(RGB_STATUS_GREEN, 2);
            ESP_LOGW(TAG, "XO cyc=%d done pd=%d fail=%d", cyc, (int)scd41_power_down(), xo_fail);
        }
        ESP_LOGW(TAG, "XO all done - idle (sensor asleep)");
        for (;;) { rwdt_guard_arm(60); sensor_wait_ms(30000); }
    }
#endif
#if CONFIG_SENS_SCD41_SLEEP_EXPERIMENT
    /* 2026-10-26(실험) — Kconfig 설명 참고. 기다림은 RWDT 예산(60초) 안에서 나눔 */
    {
        int64_t t_start = esp_timer_get_time();
        #define EXP_MIN(m) (t_start + (int64_t)(m) * 60 * 1000000LL)
        #define EXP_WAIT_UNTIL(t) do { while (esp_timer_get_time() < (t)) { rwdt_guard_arm(60); \
            int64_t _l = ((t) - esp_timer_get_time()) / 1000; sensor_wait_ms((uint32_t)(_l > 30000 ? 30000 : (_l > 0 ? _l : 1))); } } while (0)
        ESP_LOGW(TAG, "EXP start (1: 30min single+sleep, 2A: 60min periodic burst/10min, 2B: 60min 5 single/10min)");
        /* 1) 약 24초 주기 단발 + 재움 — measure_scd41()은 깨우기 → 버리는 측정 → 측정 */
        while (esp_timer_get_time() < EXP_MIN(30)) {
            int64_t t0 = esp_timer_get_time();
            rwdt_guard_arm(60);
            float v[SENSOR_CHAN_COUNT] = { 0 };
            pm_lock_no_light_sleep_acquire();
            bool ok = measure_scd41(v);
            bool pd = scd41_power_down();
            pm_lock_no_light_sleep_release();
            if (ok) ESP_LOGI(TAG, "EXP1 co2=%d pd=%d", (int)v[0], (int)pd);
            else ESP_LOGW(TAG, "EXP1 read failed (fault=%u)", (unsigned)s_attempt_fault);
            EXP_WAIT_UNTIL(t0 + 23600LL * 1000);
        }
        /* 2A) 10분마다: 깨우기 → 연속 측정 → 5초마다 13개 → 멈춤 → 재움 */
        for (int cyc = 1; cyc <= 6; cyc++) {
            EXP_WAIT_UNTIL(EXP_MIN(30 + (cyc - 1) * 10));
            rwdt_guard_arm(60);
            scd41_wake_up();
            bool st = scd41_start_periodic();
            int got = 0;
            int64_t tb = esp_timer_get_time();
            while (got < 13 && esp_timer_get_time() - tb < 120LL * 1000000) {
                rwdt_guard_arm(60);
                sensor_wait_ms(500);
                int co2 = 0;
                float t = 0.0f, h = 0.0f;
                if (scd41_read(&co2, &t, &h)) {
                    got++;
                    ESP_LOGI(TAG, "EXP2A cyc=%d i=%d co2=%d st=%d", cyc, got, co2, (int)st);
                }
            }
            scd41_stop_periodic_measurement();
            sensor_wait_ms(500);
            ESP_LOGW(TAG, "EXP2A cyc=%d done (%d values) pd=%d", cyc, got, (int)scd41_power_down());
        }
        /* 2B) 10분마다: 깨우기 → 단발 5번(각 5초, 버리지 않고 모두 기록) → 재움 */
        for (int cyc = 1; cyc <= 6; cyc++) {
            EXP_WAIT_UNTIL(EXP_MIN(90 + (cyc - 1) * 10));
            rwdt_guard_arm(60);
            scd41_wake_up();
            for (int i = 1; i <= 5; i++) {
                rwdt_guard_arm(60);
                int co2 = 0;
                float t = 0.0f, h = 0.0f;
                bool rd = false, ok = false;
                if (scd41_trigger_single_shot()) {
                    sensor_wait_ms(SCD41_MEASURE_MIN_WAIT_MS);
                    for (int k = 0; k < 50 && !rd; k++) {
                        if (scd41_poll_single_shot(&co2, &t, &h, &ok)) rd = true; else sensor_wait_ms(100);
                    }
                }
                if (rd && ok) ESP_LOGI(TAG, "EXP2B cyc=%d i=%d co2=%d", cyc, i, co2);
                else ESP_LOGW(TAG, "EXP2B cyc=%d i=%d read failed", cyc, i);
            }
            ESP_LOGW(TAG, "EXP2B cyc=%d done pd=%d", cyc, (int)scd41_power_down());
        }
        ESP_LOGW(TAG, "EXP all done - idle (sensor asleep)");
        for (;;) { rwdt_guard_arm(60); sensor_wait_ms(30000); }
    }
#endif
#if CONFIG_SENS_SCD41_SINGLESHOT_LOOP_TEST
    /* 2026-10-26(시험 A) — 보통 측정과 같은 measure_scd41()을 같은 간격(보통 경로 실측 약 23.6초)으로, 통신·딥슬립 없이.
     * 라이트슬립 막기도 보통 경로(attempt_one_measurement)와 같게 */
    ESP_LOGW(TAG, "SCD41 SINGLESHOT LOOP TEST: start");
    for (;;) {
        int64_t t0 = esp_timer_get_time();
        rwdt_guard_arm(60);
        float v[SENSOR_CHAN_COUNT] = { 0 };
        pm_lock_no_light_sleep_acquire();
        bool ok = measure_scd41(v);
        pm_lock_no_light_sleep_release();
        if (ok) {
            int t10 = (int)(v[1] * 10.0f + 0.5f), h10 = (int)(v[2] * 10.0f + 0.5f);
            ESP_LOGI(TAG, "SSLOOP co2=%d temp=%d.%d humi=%d.%d", (int)v[0], t10 / 10, t10 % 10, h10 / 10, h10 % 10);
        } else {
            ESP_LOGW(TAG, "SSLOOP read failed (fault=%u)", (unsigned)s_attempt_fault);
        }
        int64_t used_ms = (esp_timer_get_time() - t0) / 1000;
        int64_t left_ms = CONFIG_SENS_SCD41_SSLOOP_INTERVAL_MS - used_ms;
        while (left_ms > 0) {   /* 긴 간격(시험 B 5분)은 RWDT 예산(60초) 안에서 나눠 기다림 */
            rwdt_guard_arm(60);
            uint32_t w = left_ms > 30000 ? 30000 : (uint32_t)left_ms;
            sensor_wait_ms(w);
            left_ms -= w;
        }
    }
#endif
#if CONFIG_SENS_SCD41_PERIODIC_TEST
    /* 2026-10-25(시험) — 연속 측정 모드로만 돌며 값을 로그(통신·딥슬립 없음). 측정 방식 하나만 바꿈 */
#if CONFIG_SENS_SCD41_PERIODIC_LOWPOWER
    ESP_LOGW(TAG, "SCD41 PERIODIC TEST (low power 30s): start=%s", scd41_start_low_power_periodic() ? "ok" : "FAIL");
#else
    ESP_LOGW(TAG, "SCD41 PERIODIC TEST: start=%s", scd41_start_periodic() ? "ok" : "FAIL");
#endif
    for (;;) {
        rwdt_guard_arm(60);   /* 딥슬립 없이 계속 깨어 있으므로 예산을 계속 다시 잡음 */
        sensor_wait_ms(1000);
        int co2 = 0;
        float t = 0.0f, h = 0.0f;
        if (scd41_read(&co2, &t, &h)) {
            int t10 = (int)(t * 10.0f + 0.5f), h10 = (int)(h * 10.0f + 0.5f);
            ESP_LOGI(TAG, "PERIODIC co2=%d temp=%d.%d humi=%d.%d", co2, t10 / 10, t10 % 10, h10 / 10, h10 % 10);
        }
    }
#endif
#elif CONFIG_SENS_SENSOR_SHT45 || CONFIG_SENS_SENSOR_SHT45_DS2
    /* SHT4x는 SCD41과 달리 전원안정화 지연 요구사항이 문서화돼있지 않음(예전 sensor_node.c도
     * 지연 없이 바로 init) — I2C 버스 공유(BSP_C3_I2C_*)는 SCD41과 동일 자리 */
    if (!sht4x_init(BSP_C3_I2C_PORT, BSP_C3_I2C_SDA, BSP_C3_I2C_SCL)) {
        ESP_LOGW(TAG, "SHT45 init failed - check wiring (retry next cycle)");
    }
#elif CONFIG_SENS_SENSOR_PT100
    if (!max31865_init(BSP_C3_PT100_SPI_HOST, BSP_C3_PT100_SPI_CLK, BSP_C3_PT100_SPI_SDI,
                       BSP_C3_PT100_SPI_SDO, BSP_C3_PT100_SPI_CS, true)) {
        ESP_LOGW(TAG, "MAX31865 init failed - check wiring (retry next cycle)");
    }
#endif
    /* 2026-09-12(MQ137 추가) — AO 채널 설정에 공유 ADC 유닛 핸들이 필요한데, 그 핸들은
     * battery_init() 이후에나 생기므로 여기서는 못 함(아래 battery_init()+VIN 채널 설정
     * 직후로 미룸) — SCD41(I2C)은 그런 의존성이 없어서 그대로 여기(초기)에 둠 */

    uint32_t measurement_elapsed_ms = 0;  /* 2026-09-06(사용자 지시) — 이번 사이클에 측정으로
                                            * 쓴 실제 시간, 나중에 딥슬립 시간에서 뺄 기준.
                                            * 실제 측정은 페어링 확인 후(아래) 일어남 */

    /* 2026-09-06 버그수정(사용자 보고: "배터리 값은 0.00 V 로 나와") — CASK 재작성(2026-09-05)
     * 때 이 초기화 블록 자체를 실수로 빠뜨림. battery_read_mv()는 battery_init()이 세팅하는
     * 내부 ADC 핸들(s_adc)이 없으면 그냥 0을 반환하므로(battery.c), 호출은 계속 됐지만 항상
     * 0이 나왔던 것 — 매번 "0.00 V"로 보인 원인 */
    battery_config_t batt_cfg = {
        .adc_unit    = BSP_C3_BATTERY_ADC_UNIT,
        .adc_channel = BSP_C3_BATTERY_ADC_CHANNEL,
        .atten       = BSP_C3_BATTERY_ADC_ATTEN,
        .divider     = BSP_C3_BATTERY_DIV,
        .full_mv     = BATT_FULL_MV_FIXED,
        .empty_mv    = 3300.0f,
        .ctrl_gpio   = GPIO_NUM_NC,
    };
    battery_init(&batt_cfg);

    s_adc_unit = battery_get_adc_handle();

#if CONFIG_SENS_SENSOR_MQ137
    if (!mq137_init(s_adc_unit, BSP_C3_MQ137_AO_ADC_CHANNEL, BSP_C3_MQ137_AO_ADC_ATTEN,
                    BSP_C3_MQ137_DO_PIN)) {
        ESP_LOGW(TAG, "MQ137 init failed - check wiring (retry next cycle)");
    }
#elif CONFIG_SENS_SENSOR_SC05
    /* 2026-09-15 — 공유 ADC 핸들 의존성 없음(UART), MQ137과 달리 battery_init() 이후로
     * 미룰 필요는 없지만 위치 일관성을 위해 같은 자리에 둠 */
    if (!sc05_init(BSP_C3_SC05_UART_PORT, BSP_C3_SC05_UART_RX, BSP_C3_SC05_UART_TX,
                   BSP_C3_SC05_UART_BAUD)) {
        ESP_LOGW(TAG, "SC05 init failed - check wiring (retry next cycle)");
    }
#endif

    int batt_mv = battery_read_mv();
    bool batt_ok = (batt_mv > 0);
    int batt_pct = batt_ok ? battery_mv_to_pct(batt_mv) : 0;
    if (batt_ok && batt_pct < 20) rgb_status_blink(RGB_STATUS_RED, 3);   /* 2026-10-23 — 배터리 20% 미만 R 3번 */

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
    ESP_LOGI(TAG, "WiFi modem sleep set: %s", esp_err_to_name(ps_err));

    /* 2026-09-05 — 캠은 스피커 이벤트훅을 통해 간접 연결하지만(cam_speaker 의존), 센스는
     * 스피커가 없으니 채널스캔 스윕완료 훅에 직접 연결 */
    esp_now_channelsync_set_event_hooks(NULL, NULL, NULL, on_scan_sweep_done);
    s_wake_recheck_sem = xSemaphoreCreateBinary();

    esp_now_node_set_sensors(s_sensors, sizeof(s_sensors) / sizeof(s_sensors[0]));   /* 2026-10-23 — 센서 목록 */
    esp_now_node_set_sensor_status(s_sen_fault, s_sen_streak, (uint8_t)SENS_SENSOR_N);   /* 지난 측정의 센서별 상태(RTC) */
    esp_now_node_init(SENSOR_KIND_CURRENT, SENSOR_CHAN_COUNT, s_chan_types);

    ESP_LOGI(TAG, "Headless Sens node ready (kind=%d, channels=%d, measID=%u)",
             SENSOR_KIND_CURRENT, SENSOR_CHAN_COUNT, (unsigned)s_measurement_id);

    /* CAM의 app_main 웨이크 루프와 완전히 동일 구조(2026-09-05 재수정, cam_node.c 참고) —
     * esp_now_node_init() 직후 esp_now_node_report_reading()을 한 번 동기 호출하는 것 자체가
     * 캠의 "esp_now_cam_init() 끝에서 esp_now_cam_reconnect() 호출"과 동일 타이밍(esp_now_
     * node_cask.c 파일 헤더 주석 참고) — 그래서 이 결과(paired_now)가 항상 정확함 */
    bool paired_now = esp_now_node_report_reading(SENSOR_CHAN_COUNT, s_cached_chan_ok, s_cached_vals,
                                                   s_measurement_id, 0, batt_mv_u16,
                                                   s_sensor_fault, s_sensor_fail_streak);
    uint32_t sleep_sec = ESP_NOW_NODE_UNPAIRED_RETRY_SEC;
    /* 2026-09-06(사용자 지시) — Live 모드(sleep_sec==0)로 오래 깨있는 동안에도 측정주기가
     * 되면 재측정해야 함(캠의 독립적 주기촬영과 동일 원칙) — 이 기준점을 실제로 측정할
     * 때마다(워밍업 포함, do_gated_measurement_once 안에서) 갱신하고, 여기서부터는 Live
     * 루프 안에서 "이 기준점 이후 실제로 깨있던 시간"을 재보고 판단. 최초값은 아래에서
     * 실제로 측정이 일어난 시점에 다시 잡음(그 전엔 의미 없는 기준점) */
    uint32_t live_awake_baseline_ms = (uint32_t)(esp_timer_get_time() / 1000);
    bool measured_this_boot = false;  /* 2026-09-06(사용자 지시) — 페어링 확인 전엔 측정 안 함,
                                          확인되면 딱 한 번만 이 부팅의 최초 측정을 함 */
    /* 2026-10-05 — 측정이 실패하면 이 시각 전엔 다시 시도하지 않음(측정 주기만큼 뒤). 재초기화 간격도 그만큼 벌어짐 */
    uint32_t live_retry_after_ms = 0;

    if (paired_now) {
        do_gated_measurement_once(&measurement_elapsed_ms);
        measured_this_boot = true;
        live_awake_baseline_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (s_sensor_fail_streak > 0) {
            live_retry_after_ms = live_awake_baseline_ms + esp_now_node_get_sample_interval_sec() * 1000u;
        }
    }

    for (;;) {
        if (!paired_now) {
            /* 알려진 허브가 없었거나 패스트패스가 실패해서 지금 폴백 스캔 중(백그라운드 —
             * esp_now_node_report_reading()의 폴백 분기 참고) — PAIR_REQUEST가 비동기로
             * 도착할 때까지 이벤트 기반 대기, 스윕 한 바퀴 다 돌 때까지만. 이 대기 동안은
             * 광고/스캔 상태라 측정 안 함(사용자 지시) */
            while (!esp_now_node_is_paired() && !s_sweep_completed) {
                xSemaphoreTake(s_wake_recheck_sem, pdMS_TO_TICKS(1000));
            }
            if (!esp_now_node_is_paired()) {
                sleep_sec = next_unpaired_retry_sleep_sec();
                s_unpaired_backoff_elapsed_sec += sleep_sec;
                ESP_LOGW(TAG, "Fallback sweep done - Cntl not found, retry in %us", (unsigned)sleep_sec);
                break;
            }
        }
        s_unpaired_backoff_elapsed_sec = 0;  /* 페어링 성공 — 백오프 리셋 */

        if (!measured_this_boot) {
            /* 폴백 스캔으로 방금 막 페어링됨 — 이제서야 이번 부팅의 최초 측정 수행 */
            do_gated_measurement_once(&measurement_elapsed_ms);
            measured_this_boot = true;
            live_awake_baseline_ms = (uint32_t)(esp_timer_get_time() / 1000);
        }

        /* CASK 대기 — CONFIG부터 SLEEP_NOW까지, 이벤트 기반으로 기다리되 SENS_CASK_TIMEOUT_MS
         * 전체 상한을 둠(캠과 동일 원칙 — "WAKE_HELLO 성공 판정을 CASK 전체로 넓힌 것") */
        uint32_t cask_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
        bool cask_timed_out = false;
        while (!s_sleep_now_requested) {
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
            if (now_ms - cask_start_ms >= SENS_CASK_TIMEOUT_MS) {
                ESP_LOGW(TAG, "CASK incomplete (%ums, no SLEEP_NOW) - retrying from WAKE_HELLO_SENS",
                         (unsigned)SENS_CASK_TIMEOUT_MS);
                cask_timed_out = true;
                break;
            }
            xSemaphoreTake(s_wake_recheck_sem, pdMS_TO_TICKS(1000));
        }

        if (cask_timed_out) {
            paired_now = esp_now_node_report_reading(SENSOR_CHAN_COUNT, s_cached_chan_ok, s_cached_vals,
                                                       s_measurement_id, 0, batt_mv_u16,
                                                   s_sensor_fault, s_sensor_fail_streak);
            continue;
        }

        /* 2026-09-05(사용자 지시로 정정) — 딥슬립 실제 시간은 캠과 동일하게 SLEEP_NOW.
         * sleep_sec 그 자체를 그대로 씀(노드 쪽은 특별취급 없음). "센스는 자기 측정주기마다
         * 깨야 한다"는 요구는 콘 쪽(node_hub.c의 send_cask_sleep_now())이 이 노드의
         * 측정주기를 기준값으로 써서 sleep_sec을 계산하는 것으로 충족됨 — 그래서 여기서
         * 받는 값이 곧 측정주기이고, 별도로 로컬 값을 다시 끼워 넣지 않음(그래야 실수로
         * "실제 잠든 시간 > 측정주기"가 되는 경로 자체가 없어짐) */
        if (esp_now_node_get_last_sleep_sec() != 0) {
            sleep_sec = esp_now_node_get_last_sleep_sec();
            break;
        }

        /* 2026-09-06(사용자 지시) — Live로 오래 깨있는 동안 측정주기가 되면 재측정. "이번
         * 부팅 들어와서 실제로 깨있던 시간"(live_awake_baseline_ms 기준)을 마지막 실측정
         * 이후 경과시간에 더해서 판단 — 딥슬립을 안 거치므로 s_seconds_since_last_measurement
         * 자체는 그대로 두고(다음에 진짜 잘 때를 위해), 여기서는 로컬 변수로만 따짐 */
        uint32_t live_now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        uint32_t live_awake_elapsed_sec = (live_now_ms - live_awake_baseline_ms) / 1000;
        uint32_t live_measure_period_sec = esp_now_node_get_sample_interval_sec();
#if CONFIG_SENS_SENSOR_SCD41
        /* 2026-10-27 — SCD41: 시작만 하고 안정되면 읽음(위 흐름과 같음) */
        if (s_scd_pending) {
            if (scd_settle_left_us() <= 0) {
                scd_finish();
                live_awake_baseline_ms = (uint32_t)(esp_timer_get_time() / 1000);
                if (s_sensor_fail_streak > 0) live_retry_after_ms = live_awake_baseline_ms + live_measure_period_sec * 1000u;
            }
        } else if (s_seconds_since_last_measurement + live_awake_elapsed_sec >= live_measure_period_sec &&
                   (int32_t)(live_now_ms - live_retry_after_ms) >= 0) {
            if (!scd_start()) live_retry_after_ms = (uint32_t)(esp_timer_get_time() / 1000) + live_measure_period_sec * 1000u;
        }
        if (false) {
#else
        if (s_seconds_since_last_measurement + live_awake_elapsed_sec >= live_measure_period_sec &&
            (int32_t)(live_now_ms - live_retry_after_ms) >= 0) {
#endif
            float fresh_vals[SENSOR_CHAN_COUNT] = { 0 };
            uint8_t fresh_chok[SENSOR_CHAN_COUNT] = { 0 };
            bool all_ok = attempt_one_measurement(fresh_vals, fresh_chok, &measurement_elapsed_ms);
            bool any_ok = false;
            for (int i = 0; i < SENSOR_CHAN_COUNT; i++) any_ok |= fresh_chok[i];
            if (any_ok) {   /* 2026-10-23 — 센서가 여럿이면 성공한 채널만(보통 경로와 같음) */
                (void)all_ok;
                note_measure_recovered();   /* 하나라도 성공하면 보드는 성공(센서별은 note_sensor_results) */
                memcpy(s_cached_vals, fresh_vals, sizeof(fresh_vals));
                for (int i = 0; i < SENSOR_CHAN_COUNT; i++) s_cached_chan_ok[i] = fresh_chok[i];
                s_measurement_id++;
                s_seconds_since_last_measurement = 0;
                live_awake_baseline_ms = (uint32_t)(esp_timer_get_time() / 1000);
            } else {
                ESP_LOGW(TAG, "Sensor read failed (Live re-measure) - retry in %us", (unsigned)live_measure_period_sec);
                note_measure_failed();  /* 2026-10-05 — 보통 경로와 같게: 콘에 실패 보고 + 3번마다 SCD41 재초기화 */
                live_retry_after_ms = (uint32_t)(esp_timer_get_time() / 1000) + live_measure_period_sec * 1000u;
            }
        }

        /* Live 루프 — 이번 CASK가 걸린 시간이 페이싱 기준보다 짧으면 나머지를 채워 대기 */
        uint32_t elapsed_ms = (uint32_t)(esp_timer_get_time() / 1000) - cask_start_ms;
        if (elapsed_ms < SENS_CASK_LIVE_PACE_MS) {
            vTaskDelay(pdMS_TO_TICKS(SENS_CASK_LIVE_PACE_MS - elapsed_ms));
        }
        paired_now = esp_now_node_report_reading(SENSOR_CHAN_COUNT, s_cached_chan_ok, s_cached_vals,
                                                   s_measurement_id, 0, batt_mv_u16,
                                                   s_sensor_fault, s_sensor_fail_streak);
    }

    /* 이번에 실제로 잠들 시간만큼 "마지막 실측정 이후 경과시간"에 더해둠 — 다음 부팅에서
     * 위 due_for_measurement 판단의 기준이 됨(사용자 지시: 측정주기 게이팅). 이 누적은
     * CNTL이 준 "원래 자야 할 시간"(sleep_sec) 기준 그대로 — 아래 실제 딥슬립 시간 보정과는
     * 별개(측정주기 게이팅은 "예산 하나를 다 썼다"는 의미라 원래 예산으로 유지) */
    s_seconds_since_last_measurement += sleep_sec;

    /* 2026-09-06(사용자 지시) — "총 잠들어야 하는 시간에서 측정에 소요된 시간을 뺀 시간만큼만
     * 잠들도록" — 이 센서는 측정 자체가 오래 걸려서(수 초), 측정시간을 안 빼면 전체 주기가
     * (측정시간 + sleep_sec)로 밀림. 측정을 안 한 사이클(due_for_measurement==false)은
     * measurement_elapsed_ms==0이라 자연히 그대로 sleep_sec만큼 잠 */
    uint32_t measurement_elapsed_sec = measurement_elapsed_ms / 1000;
    uint32_t actual_sleep_sec = (sleep_sec > measurement_elapsed_sec)
                                     ? sleep_sec - measurement_elapsed_sec : 0;

    calib_wait_idle();   /* 2026-10-13 — 보정 초기화 중엔 잠들지 않음 */
    esp_now_node_persist_sensor_ids();   /* 2026-10-23 — 콘이 붙여 준 센서 ID 저장 */
    rgb_status_flush(1500);   /* 2026-10-23 — 남은 깜빡임을 보여 주고 끔 */
    esp_now_node_note_sleep_entry();
    ESP_LOGI(TAG, "Deep sleep: wake in %us (requested %us, %us spent measuring)",
             (unsigned)actual_sleep_sec, (unsigned)sleep_sec, (unsigned)measurement_elapsed_sec);
    uint64_t sleep_us = (uint64_t)actual_sleep_sec * 1000000ULL;
#if CONFIG_SENS_SENSOR_SCD41
    if (s_scd_pending) {   /* 2026-10-27 — 다음 응답 깨어남보다 읽을 때가 먼저면 그때 무선 없이 깸 */
        int64_t left = scd_settle_left_us();
        if (left < (int64_t)sleep_us) {
            s_scd_resp_due_rtc_us = esp_rtc_get_time_us() + sleep_us;
            s_scd_readwake = true;
            sleep_us = (left > 100000) ? (uint64_t)left : 100000;
            ESP_LOGI(TAG, "SCD41 read wake in %llums (response wake in %us)", (unsigned long long)(sleep_us / 1000),
                     (unsigned)actual_sleep_sec);
        }
    }
#endif
    esp_sleep_enable_timer_wakeup(sleep_us);
    esp_now_node_wait_replies_sent(100);  /* 2026-10-08 — SLEEP_NOW 응답이 나가기 전에 잠들지 않게 */
    esp_deep_sleep_start();
}
