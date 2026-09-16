#include "power_relay.h"
#include "ui_main.h"
#include "fs.h"

#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "power_relay";

/* 2026-09-16 — 실제 배선 전까지의 자리표시 핀. SSR-DK25DA 제어입력(3-32VDC, 2선)을 직접
 * 구동하기엔 3.3V 로직레벨 전류가 부족할 수 있어 실제 배선 시 트랜지스터/옵토 드라이버
 * 단계가 필요할 수 있음 — 물리 배선 전에 반드시 재확인할 것(TODO, 확정 아님) */
static const gpio_num_t s_relay_gpio[POWER_RELAY_COUNT] = { GPIO_NUM_NC, GPIO_NUM_NC };

#define POWER_RELAY_FILE_PATH   FS_MOUNT_POINT "/power_relay.bin"
#define POWER_RELAY_FILE_VERSION 1

/* 판정 주기(ms) — 응답성 설정과 무관한 이 모듈만의 독립 주기. 짧을수록 반응이 빠르지만
 * stats_agg 집계 버킷(1H 스케일=60초 폭)보다 훨씬 짧게 돌려봤자 대부분 같은 값을 다시
 * 읽는 것뿐이라 15초로 정함(그래프 갱신주기 재조정과 같은 논리, 2026-09-15) */
#define POWER_RELAY_EVAL_INTERVAL_MS 15000

/* 소스 값이 이 시간(ms) 이상 안 들어오면 정지(Off) — "짧은 끊김은 무시, 길면 멈춤"
 * (2026-09-15 사용자 설계). 릴레이별 실제 동작주기에 맞춰 나중에 조정 가능하도록 남겨둠 */
#define POWER_RELAY_STALE_TIMEOUT_MS (5 * 60 * 1000)

/* 추세(기울기) 계산용 최근 샘플 링버퍼 — 판정 주기(15초)로 채워짐. 2026-09-16(사용자 설계
 * 재정리 — "판단기간" 사용자 입력값) 용량은 넉넉히(40개=최대 10분 창) 잡아두고, 실제
 * 회귀에는 evaluate_relay()가 cfg->trend_window_sec 안에 드는 샘플만 골라서 씀 */
#define POWER_TREND_SAMPLES 40

typedef struct {
    uint32_t t_ms[POWER_TREND_SAMPLES];
    float    v[POWER_TREND_SAMPLES];
    int      count;   /* 0..POWER_TREND_SAMPLES, 아직 안 채워졌으면 이만큼만 */
    int      next;    /* 다음에 쓸 슬롯(원형) */
} power_trend_buf_t;

typedef struct __attribute__((packed)) {
    uint32_t version;
    power_relay_config_t relays[POWER_RELAY_COUNT];
} power_relay_file_t;

static power_relay_config_t s_relay_cfg[POWER_RELAY_COUNT];
static bool     s_commanded_on[POWER_RELAY_COUNT];
static uint32_t s_last_transition_ms[POWER_RELAY_COUNT];
static uint32_t s_last_data_ms[POWER_RELAY_COUNT];
static power_trend_buf_t s_trend[POWER_RELAY_COUNT];

static void power_relay_save(void)
{
    FILE *f = fopen(POWER_RELAY_FILE_PATH, "wb");
    if (!f) {
        ESP_LOGW(TAG, "저장 실패(fopen): %s", POWER_RELAY_FILE_PATH);
        return;
    }
    power_relay_file_t s = { .version = POWER_RELAY_FILE_VERSION };
    memcpy(s.relays, s_relay_cfg, sizeof(s.relays));
    fwrite(&s, sizeof(s), 1, f);
    fclose(f);
}

void power_relay_load(void)
{
    memset(s_relay_cfg, 0, sizeof(s_relay_cfg));
    for (int i = 0; i < POWER_RELAY_COUNT; i++) {
        s_relay_cfg[i].min_hold_sec = 60;          /* 기본 1분 — 사용자가 팝업에서 조정 */
        s_relay_cfg[i].trend_window_sec = 60;
        s_relay_cfg[i].ai_mode = true;  /* 2026-09-16 — 기본은 AI On(간단한 화면) */
    }

    FILE *f = fopen(POWER_RELAY_FILE_PATH, "rb");
    if (!f) return;
    power_relay_file_t s = { 0 };
    bool ok = (fread(&s, sizeof(s), 1, f) == 1) && s.version == POWER_RELAY_FILE_VERSION;
    fclose(f);
    if (!ok) {
        ESP_LOGW(TAG, "설정 파일 형식 불일치 — 기본값 유지");
        return;
    }
    for (int i = 0; i < POWER_RELAY_COUNT; i++) {
        s.relays[i].alias[sizeof(s.relays[i].alias) - 1] = '\0';
    }
    memcpy(s_relay_cfg, s.relays, sizeof(s_relay_cfg));
    ESP_LOGI(TAG, "설정 복원 완료(relay0.configured=%d relay1.configured=%d)",
             (int)s_relay_cfg[0].configured, (int)s_relay_cfg[1].configured);
}

const power_relay_config_t *power_relay_get_config(int idx)
{
    if (idx < 0 || idx >= POWER_RELAY_COUNT) return NULL;
    return &s_relay_cfg[idx];
}

/* 2026-09-16(실기에서 발견된 잘못 — "Alias만 줬는데 재부팅") — 예전엔 여기서 무조건
 * configured=true로 만들었음. Alias 전용 Apply(cb_relay_alias_apply_clicked)도 내부적으로
 * 이 함수를 호출하는데, 그러면 채널/임계값 등 나머지 필드가 전부 기본값(0)인 채로 판정
 * 루프(evaluate_relay)가 그 릴레이를 "설정 완료"로 착각하고 평가를 시작해버림 — 실제로
 * 재부팅 크래시가 이 상태에서 재현됨. 이제 configured는 호출부가 명시한 값을 그대로
 * 존중(cfg->configured) — 본 설정 Apply(cb_relay_apply_clicked)만 true로 세팅해서 넘기고,
 * Alias 전용 Apply는 기존 값을 그대로 들고 있는 cfg를 넘기므로 자연히 안 바뀜 */
void power_relay_set_config(int idx, const power_relay_config_t *cfg)
{
    if (idx < 0 || idx >= POWER_RELAY_COUNT || !cfg) return;
    s_relay_cfg[idx] = *cfg;
    s_relay_cfg[idx].alias[sizeof(s_relay_cfg[idx].alias) - 1] = '\0';
    /* 설정이 바뀌면 추세 이력/최근전환시각을 리셋 — 새 소스/방향/임계값 기준으로 처음부터
     * 다시 판단해야지, 이전 소스 기준으로 쌓인 샘플을 섞어 쓰면 안 됨 */
    memset(&s_trend[idx], 0, sizeof(s_trend[idx]));
    s_last_transition_ms[idx] = 0;
    power_relay_save();
}

bool power_relay_get_commanded_on(int idx)
{
    if (idx < 0 || idx >= POWER_RELAY_COUNT) return false;
    return s_commanded_on[idx];
}

static void relay_set_gpio(int idx, bool on)
{
    if (s_relay_gpio[idx] == GPIO_NUM_NC) {
        ESP_LOGW(TAG, "릴레이%d: GPIO 미배정(자리표시) — 명령상태만 갱신, 실제 핀 출력 없음", idx);
        return;
    }
    gpio_set_level(s_relay_gpio[idx], on ? 1 : 0);
}

static void power_relay_command(int idx, bool on, uint32_t now_ms)
{
    if (s_commanded_on[idx] == on) return;
    s_commanded_on[idx] = on;
    s_last_transition_ms[idx] = now_ms;
    relay_set_gpio(idx, on);
    ESP_LOGI(TAG, "릴레이%d(%s) -> %s", idx, s_relay_cfg[idx].alias, on ? "On" : "Off");
}

/* 최소제곱 1차회귀 기울기(값/ms) — 추세(방향+속도)만 필요하므로 1차식으로 충분(그래프의
 * 2차 국소회귀와 달리 여기선 "부드러운 곡선"이 아니라 "지금 어느 쪽으로 얼마나 빠르게
 * 가는지"만 필요). 2026-09-16 — window_ms보다 오래된 샘플은 회귀에서 제외("판단기간"을
 * 사용자가 직접 지정) */
static float trend_slope_per_ms(const power_trend_buf_t *buf, uint32_t now_ms, uint32_t window_ms)
{
    double sum_t = 0, sum_v = 0, sum_tt = 0, sum_tv = 0;
    int n_used = 0;
    for (int i = 0; i < buf->count; i++) {
        if ((now_ms - buf->t_ms[i]) > window_ms) continue;
        double t = (double)(buf->t_ms[i] - now_ms);  /* now_ms 기준 상대시각(항상 <=0) —
                                                          정밀도 손실 방지 + t0 슬라이딩 불필요 */
        double v = (double)buf->v[i];
        sum_t += t; sum_v += v; sum_tt += t * t; sum_tv += t * v;
        n_used++;
    }
    if (n_used < 2) return 0.0f;
    double n = (double)n_used;
    double denom = n * sum_tt - sum_t * sum_t;
    if (fabs(denom) < 1e-6) return 0.0f;
    return (float)((n * sum_tv - sum_t * sum_v) / denom);
}

static void trend_push(power_trend_buf_t *buf, uint32_t now_ms, float value)
{
    buf->t_ms[buf->next] = now_ms;
    buf->v[buf->next] = value;
    buf->next = (buf->next + 1) % POWER_TREND_SAMPLES;
    if (buf->count < POWER_TREND_SAMPLES) buf->count++;
}

/* 2026-09-16(사용자 설계 대화 정리) — 히스테리시스+최소유지시간+추세("Puzzy") 판정.
 *
 * 추세 규칙: raw=지금 값, predicted=raw+기울기*lookahead.
 *  - On으로 갈 때는 raw/predicted 중 "On 쪽으로 더 유리한 값"을 씀(ON_ABOVE면 max, ON_BELOW면
 *    min) — 그래서 raw가 실제로 임계값을 넘으면 추세와 무관하게 반드시 켜지고(억제되지 않음),
 *    추세가 그 방향이면 predicted가 먼저 넘어가서 "미리 켠다".
 *  - Off로 갈 때는 predicted만 씀 — 추세가 아직 On 쪽이면 predicted가 raw보다 유리한 값에
 *    머물러 있어 Off 임계값을 늦게 넘음(오래 유지), 추세가 이미 Off 쪽이면 predicted가 raw
 *    보다 먼저 넘어가서 더 빨리 끈다(사용자 설명한 "반대"쌍 그대로) */
static void evaluate_relay(int idx, uint32_t now_ms)
{
    power_relay_config_t *cfg = &s_relay_cfg[idx];
    if (!cfg->configured) return;
    /* 방어적 가드(2026-09-16) — Alias만 저장된 채 configured가 잘못 true였던 예전 상태가
     * 저장 파일에 이미 남아있을 수 있음(재부팅해도 그대로 불러와짐). chan_type이 NONE이면
     * 실제로 한 번도 본 설정을 Apply한 적 없는 상태이므로 평가 자체를 건너뜀 */
    if (cfg->chan_type == SENSOR_CHAN_NONE) return;

    float value;
    bool have = ui_main_query_power_source_value(cfg, &value);
    if (!have) {
        if (s_last_data_ms[idx] != 0 && (now_ms - s_last_data_ms[idx]) > POWER_RELAY_STALE_TIMEOUT_MS) {
            /* 2026-09-15(사용자 설계) — 기준 센서가 오래 끊기면 기본은 정지 */
            power_relay_command(idx, false, now_ms);
        }
        return;  /* 짧은 끊김은 이번 주기 판정만 건너뜀(무시) */
    }
    s_last_data_ms[idx] = now_ms;
    trend_push(&s_trend[idx], now_ms, value);

    float predicted = value;
    if (cfg->trend_enable) {
        uint32_t window_ms = cfg->trend_window_sec * 1000;
        float slope_per_ms = trend_slope_per_ms(&s_trend[idx], now_ms, window_ms);
        /* 2026-09-16(사용자 정리) — "판단기간"을 그대로 예측폭으로도 씀(본 만큼만 내다봄) */
        predicted = value + slope_per_ms * (float)window_ms;
    }

    bool currently_on = s_commanded_on[idx];
    bool want_on = currently_on;
    if (cfg->direction == POWER_DIR_ON_ABOVE) {
        float on_eval  = fmaxf(value, predicted);
        float off_eval = predicted;
        if (!currently_on && on_eval >= cfg->on_threshold) want_on = true;
        else if (currently_on && off_eval <= cfg->off_threshold) want_on = false;
    } else {
        float on_eval  = fminf(value, predicted);
        float off_eval = predicted;
        if (!currently_on && on_eval <= cfg->on_threshold) want_on = true;
        else if (currently_on && off_eval >= cfg->off_threshold) want_on = false;
    }

    if (want_on == currently_on) return;

    /* 채터링 방지 — 직전 전환 후 최소유지시간이 안 지났으면 이번 전환은 보류(다음 주기에
     * 다시 판단, 값이 그때도 같은 방향이면 그때 반영됨) */
    if (s_last_transition_ms[idx] != 0 &&
        (now_ms - s_last_transition_ms[idx]) < (uint32_t)cfg->min_hold_sec * 1000) {
        return;
    }
    power_relay_command(idx, want_on, now_ms);
}

static void power_relay_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        for (int i = 0; i < POWER_RELAY_COUNT; i++) {
            evaluate_relay(i, now_ms);
        }
        vTaskDelay(pdMS_TO_TICKS(POWER_RELAY_EVAL_INTERVAL_MS));
    }
}

void power_relay_start(void)
{
    /* 2026-09-16(사용자 지적 — "메모리 35K까지 줄었어, 위험해") — 태스크 생성이 실제로
     * 내부RAM을 얼마나 쓰는지 추측 대신 실측(기존 MEMDIAG 관례와 동일) */
    size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    for (int i = 0; i < POWER_RELAY_COUNT; i++) {
        if (s_relay_gpio[i] == GPIO_NUM_NC) continue;
        gpio_config_t io_conf = {
            .pin_bit_mask = 1ULL << s_relay_gpio[i],
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
        gpio_set_level(s_relay_gpio[i], 0);
    }
    /* 2026-09-09 CNTL 태스크 우선순위 체계(project_cntl_task_priority_scheme) — 통신17/
     * SR제어15/파일처리10 중 SR제어 자리를 여기서 처음 실사용.
     * 2026-09-16(실기 크래시 조사) — evaluate_relay()가 ui_main_query_power_source_value()를
     * 거쳐 esp_now_hub_get_nodes(esp_now_hub_node_t[ESP_NOW_HUB_MAX_NODES] 지역배열, 노드
     * 구조체 자체가 꽤 큼)까지 내려가는 깊은 호출체인 — main.c의 httpd 핸들러가 똑같은
     * 호출(esp_now_hub_get_nodes)을 스택에서 하려고 기본 스택이 빠듯해서 8192로 올렸던
     * 전례(web_dashboard_start() 주석 참고)와 동일 소지가 있어 8192로 잡음.
     * 2026-09-16(사용자 지적 — "메모리 35K까지 줄었어, 위험해") — plain xTaskCreate()는
     * 스택을 내부RAM에서 할당함(esp_lv_adapter의 stack_in_psram=true와 같은 이유로 위험).
     * 이 프로젝트는 SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY+FREERTOS_TASK_CREATE_ALLOW_EXT_MEM이
     * 이미 켜져있어(sdkconfig 확인) xTaskCreateStatic()에 PSRAM 버퍼를 직접 주면 스택
     * 자체를 PSRAM에 둘 수 있음 — TCB(제어블록)만 내부RAM(작고 고정폭이라 무시 가능) */
    static StaticTask_t s_power_relay_tcb;
    StackType_t *stack_buf = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (stack_buf) {
        xTaskCreateStatic(power_relay_task, "power_relay", 8192, NULL, 15, stack_buf, &s_power_relay_tcb);
    } else {
        ESP_LOGE(TAG, "SR 태스크 스택 PSRAM 할당 실패 — 내부 RAM으로 폴백");
        xTaskCreate(power_relay_task, "power_relay", 8192, NULL, 15, NULL);
    }
    size_t after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGW(TAG, "MEMDIAG power_relay_start 비용: internal %u -> %u (소모 %d bytes)",
             (unsigned)before, (unsigned)after, (int)before - (int)after);
}
