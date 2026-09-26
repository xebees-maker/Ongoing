/**
 * @file    status_led.c
 * @brief   GPIO 상태 LED 패턴 드라이버
 */

#include "status_led.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "status_led";

#define STATUS_LED_MAX_COUNT     4
#define BLINK_SLOW_PERIOD_MS     1000
#define BLINK_FAST_PERIOD_MS     100

/* BURST_TRIPLE/HEARTBEAT는 on/off 고정 주기가 아니라 위상마다 다른 지속시간이 필요해서
 * esp_timer_start_periodic()로는 못 만들고, 위상마다 start_once로 스스로 재무장하는
 * 방식(phase sequence)으로 구현한다. */
typedef struct {
    const uint32_t *ms;
    const bool     *level;
    uint8_t         count;
} led_phase_seq_t;

/* 0.1s on/off를 3번, 0.5s 쉬고 반복 — "주의" 신호 */
#define BURST_PHASE_COUNT 7
static const uint32_t s_burst_ms[BURST_PHASE_COUNT]    = { 100, 100, 100, 100, 100, 100, 500 };
static const bool     s_burst_level[BURST_PHASE_COUNT] = { true, false, true, false, true, false, false };
static const led_phase_seq_t s_burst_seq = { s_burst_ms, s_burst_level, BURST_PHASE_COUNT };

/* 50ms on / 2.95s off — "정상 동작 중" 저전력 heartbeat */
#define HEARTBEAT_PHASE_COUNT 2
static const uint32_t s_heartbeat_ms[HEARTBEAT_PHASE_COUNT]    = { 50, 2950 };
static const bool     s_heartbeat_level[HEARTBEAT_PHASE_COUNT] = { true, false };
static const led_phase_seq_t s_heartbeat_seq = { s_heartbeat_ms, s_heartbeat_level, HEARTBEAT_PHASE_COUNT };

/* 50ms on / 1s off — HEARTBEAT보다 빠른 주기의 "주의" 신호 */
#define HEARTBEAT_FAST_PHASE_COUNT 2
static const uint32_t s_heartbeat_fast_ms[HEARTBEAT_FAST_PHASE_COUNT]    = { 50, 1000 };
static const bool     s_heartbeat_fast_level[HEARTBEAT_FAST_PHASE_COUNT] = { true, false };
static const led_phase_seq_t s_heartbeat_fast_seq = {
    s_heartbeat_fast_ms, s_heartbeat_fast_level, HEARTBEAT_FAST_PHASE_COUNT,
};

/* 50ms on / 300ms off — 더 급한 경고 (배터리 0~20%) */
#define HEARTBEAT_URGENT_PHASE_COUNT 2
static const uint32_t s_heartbeat_urgent_ms[HEARTBEAT_URGENT_PHASE_COUNT]    = { 50, 300 };
static const bool     s_heartbeat_urgent_level[HEARTBEAT_URGENT_PHASE_COUNT] = { true, false };
static const led_phase_seq_t s_heartbeat_urgent_seq = {
    s_heartbeat_urgent_ms, s_heartbeat_urgent_level, HEARTBEAT_URGENT_PHASE_COUNT,
};

typedef struct {
    bool                   in_use;
    gpio_num_t             pin;
    led_pattern_t          pattern;
    bool                   level;
    uint8_t                phase;
    const led_phase_seq_t *active_seq;   /* pattern이 BURST_TRIPLE/HEARTBEAT일 때만 사용 */
    status_led_write_fn_t  write_fn;     /* NULL이면 GPIO, 아니면 status_led_init_custom()의 출력 함수 */
    esp_timer_handle_t     blink_timer;
} status_led_slot_t;

static status_led_slot_t s_slots[STATUS_LED_MAX_COUNT];

static status_led_slot_t *find_slot(gpio_num_t pin)
{
    for (int i = 0; i < STATUS_LED_MAX_COUNT; i++) {
        if (s_slots[i].in_use && s_slots[i].pin == pin) return &s_slots[i];
    }
    return NULL;
}

static void led_write(status_led_slot_t *slot, bool on)
{
    if (slot->write_fn) {
        slot->write_fn(on);
    } else {
        gpio_set_level(slot->pin, on ? 1 : 0);
    }
}

static void arm_next_phase(status_led_slot_t *slot)
{
    const led_phase_seq_t *seq = slot->active_seq;
    slot->level = seq->level[slot->phase];
    led_write(slot, slot->level);
    esp_timer_start_once(slot->blink_timer, (uint64_t)seq->ms[slot->phase] * 1000);
    slot->phase = (slot->phase + 1) % seq->count;
}

static void blink_timer_cb(void *arg)
{
    status_led_slot_t *slot = (status_led_slot_t *)arg;
    if (slot->active_seq) {
        arm_next_phase(slot);
        return;
    }
    slot->level = !slot->level;
    led_write(slot, slot->level);
}

static status_led_slot_t *alloc_slot(void)
{
    for (int i = 0; i < STATUS_LED_MAX_COUNT; i++) {
        if (!s_slots[i].in_use) return &s_slots[i];
    }
    ESP_LOGE(TAG, "슬롯 부족 (최대 %d개)", STATUS_LED_MAX_COUNT);
    return NULL;
}

/* 출력 쪽 준비(GPIO 설정 또는 write_fn 등록)가 끝난 슬롯에 타이머를 붙이고 사용 중으로 표시 */
static bool finish_slot(status_led_slot_t *slot, gpio_num_t pin, status_led_write_fn_t write_fn)
{
    const esp_timer_create_args_t timer_args = {
        .callback = blink_timer_cb,
        .arg      = slot,
        .name     = "status_led",
    };
    if (esp_timer_create(&timer_args, &slot->blink_timer) != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create 실패 (pin=%d)", pin);
        return false;
    }

    slot->pin        = pin;
    slot->write_fn   = write_fn;
    slot->pattern    = LED_PATTERN_OFF;
    slot->level      = false;
    slot->active_seq = NULL;
    slot->in_use     = true;
    return true;
}

bool status_led_init(gpio_num_t pin)
{
    if (find_slot(pin)) return true;  /* 이미 초기화됨 */
    if (pin < 0 || pin >= GPIO_NUM_MAX) {
        ESP_LOGE(TAG, "잘못된 GPIO (pin=%d)", pin);
        return false;
    }

    status_led_slot_t *slot = alloc_slot();
    if (!slot) return false;

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode         = GPIO_MODE_OUTPUT,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config 실패 (pin=%d)", pin);
        return false;
    }
    gpio_set_level(pin, 0);
    return finish_slot(slot, pin, NULL);
}

bool status_led_init_custom(gpio_num_t id, status_led_write_fn_t write_fn)
{
    if (find_slot(id)) return true;  /* 이미 초기화됨 */
    if (id < GPIO_NUM_MAX || !write_fn) {
        ESP_LOGE(TAG, "init_custom: id=%d는 GPIO_NUM_MAX(%d) 이상이어야 하고 write_fn 필요", id, GPIO_NUM_MAX);
        return false;
    }

    status_led_slot_t *slot = alloc_slot();
    if (!slot) return false;

    write_fn(false);
    return finish_slot(slot, id, write_fn);
}

void status_led_set_pattern(gpio_num_t pin, led_pattern_t pattern)
{
    status_led_slot_t *slot = find_slot(pin);
    if (!slot) {
        ESP_LOGW(TAG, "status_led_init() 안 된 pin=%d", pin);
        return;
    }
    if (slot->pattern == pattern) return;

    esp_timer_stop(slot->blink_timer);  /* 실행 중이 아니면 무해하게 실패 */
    slot->pattern    = pattern;
    slot->active_seq = NULL;  /* 아래에서 BURST_TRIPLE/HEARTBEAT면 다시 채움 */

    switch (pattern) {
        case LED_PATTERN_OFF:
            slot->level = false;
            led_write(slot, false);
            break;
        case LED_PATTERN_ON:
            slot->level = true;
            led_write(slot, true);
            break;
        case LED_PATTERN_BLINK_SLOW:
            slot->level = true;
            led_write(slot, true);
            esp_timer_start_periodic(slot->blink_timer, BLINK_SLOW_PERIOD_MS * 1000);
            break;
        case LED_PATTERN_BLINK_FAST:
            slot->level = true;
            led_write(slot, true);
            esp_timer_start_periodic(slot->blink_timer, BLINK_FAST_PERIOD_MS * 1000);
            break;
        case LED_PATTERN_BURST_TRIPLE:
            slot->active_seq = &s_burst_seq;
            slot->phase      = 0;
            arm_next_phase(slot);
            break;
        case LED_PATTERN_HEARTBEAT:
            slot->active_seq = &s_heartbeat_seq;
            slot->phase      = 0;
            arm_next_phase(slot);
            break;
        case LED_PATTERN_HEARTBEAT_FAST:
            slot->active_seq = &s_heartbeat_fast_seq;
            slot->phase      = 0;
            arm_next_phase(slot);
            break;
        case LED_PATTERN_HEARTBEAT_URGENT:
            slot->active_seq = &s_heartbeat_urgent_seq;
            slot->phase      = 0;
            arm_next_phase(slot);
            break;
    }
}
