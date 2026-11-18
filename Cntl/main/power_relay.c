#include "power_relay.h"
#include "sd_health.h"
#include "memdiag.h"
#include "ui_main.h"
#include "fs.h"
#include "romanize.h"
#include "esp_heap_caps.h"
#include "alarm.h"
#include "rtc_sync.h"

#include <string.h>
#include <stddef.h>
#include <math.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "ch422g.h"
#include "node_hub.h"
#include "sensing_state.h"
#include "sensor_registry.h"
#include "stats_store.h"
#include "sd_storage.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "RELAY";

/* 2026-10-02(SSR 연결) — 릴레이 출력 = 보드 절연 출력 DOUT0/DOUT1(CH422G OC0/OC1 → 포토커플러 → NPN 오픈컬렉터).
 * 예전 자리표시(GPIO_NUM_NC)를 대체. 배선(SSR마다): SSR 입력 + → 전압원, SSR 입력 − → DOUT. 출력은 GND로 당기기만
 * 함(Waveshare 사양 5~36V, 450mA). CH422G OC가 Low일 때 켜짐(ch422g_set_do 주석) */
static const uint8_t s_relay_do_bit[POWER_RELAY_LOCAL_COUNT] = { CH422G_OD_DO0, CH422G_OD_DO1 };

#define POWER_RELAY_FILE_PATH   FS_MOUNT_POINT "/power_relay.bin"
/* 2026-09-18(실기에서 발견된 버그 — "저장된 값이 아니라 오버라이드 문구가 뜬다") —
 * power_relay_config_t는 packed가 아니라 컴파일러가 필드 사이에 정렬 패딩을 넣는데, 오늘
 * manual_override/manual_override_on을 기존 필드(configured/ai_mode) 사이의 패딩 자리에
 * 끼워넣으면서 우연히 sizeof()가 안 바뀌었음 — 버전을 여태 안 올려서 예전 파일이 형식
 * 불일치로 안 걸러지고, 그 구버전 byte들이(예: 예전 ai_mode 값) 새 필드 자리로 그대로
 * 재해석돼 엉뚱한 값(Override=true)이 됐음. 구조체를 바꿀 때마다(오늘 이미 y_rises/
 * z_turns_on도 안 올렸었음) 반드시 버전을 올려서 이 클래스의 버그를 원천 차단 */
#define POWER_RELAY_FILE_VERSION 5   /* 2026-11-18 — 릴레이 6개(원격 자리 4), 끊김 시간은 power_remote.bin(자리마다). 4(11-12 active, 릴레이 3개 + 끊김 시간)·3·2는 옮김 */
#define POWER_RELAY_OLD_COUNT    3   /* 버전 3·4의 릴레이 수(로컬 2 + 원격 1 — 원격은 지금의 첫 원격 자리로) */
/* 버전 3까지의 설정 구조체 크기 = active 앞까지(active가 맨 끝, trend_sample_count(uint32) 바로 뒤라 옛 크기와 같음) */
#define POWER_RELAY_CFG_V3_SIZE  offsetof(power_relay_config_t, active)
_Static_assert(offsetof(power_relay_config_t, active) == offsetof(power_relay_config_t, trend_sample_count) + sizeof(uint32_t),
               "active must directly follow trend_sample_count (v3 migration)");
#define POWER_RELAY_FILE_V4_SIZE (sizeof(uint32_t) + sizeof(power_relay_config_t) * POWER_RELAY_OLD_COUNT + sizeof(uint16_t))
#define POWER_RELAY_FILE_V3_SIZE (sizeof(uint32_t) + POWER_RELAY_CFG_V3_SIZE * POWER_RELAY_OLD_COUNT + sizeof(uint16_t))
#define POWER_RELAY_FILE_V2_SIZE (sizeof(uint32_t) + POWER_RELAY_CFG_V3_SIZE * POWER_RELAY_LOCAL_COUNT)
#define POWER_REMOTE_FAILSAFE_DEFAULT_MIN 10

/* 판정 주기(ms) — 응답성 설정과 무관한 이 모듈만의 독립 주기. 짧을수록 반응이 빠르지만
 * stats_agg 집계 버킷(1H 스케일=60초 폭)보다 훨씬 짧게 돌려봤자 대부분 같은 값을 다시
 * 읽는 것뿐이라 15초로 정함(그래프 갱신주기 재조정과 같은 논리, 2026-09-15) */
#define POWER_RELAY_EVAL_INTERVAL_MS 15000

/* 2026-09-15(사용자 설계 "짧은 끊김은 무시, 길면 멈춤") — 2026-11-12부터 기준 상태의 판단 대기: 이 시간까지 출력 유지, 지나면 끔 */
#define POWER_RELAY_WAIT_HOLD_MS (5 * 60 * 1000)

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

/* ── 2026-11-18 원격 자리(power_relay.h 주석) ── */
#define POWER_REMOTE_PATH    FS_MOUNT_POINT "/power_remote.bin"
#define POWER_REMOTE_VERSION 1
typedef struct __attribute__((packed)) {
    uint8_t  owner;          /* 주인 기기가 정해짐 */
    uint8_t  linked;         /* 사용자가 연결함(끊으면 0 — 자리 숨김, 설정·주인은 남음) */
    uint8_t  mac[6];
    uint16_t failsafe_min;   /* 통신이 끊기면 이만큼 뒤 기기가 스스로 끔 */
} remote_slot_t;
typedef struct __attribute__((packed)) {
    uint32_t version;
    remote_slot_t slot[POWER_RELAY_REMOTE_MAX];
} remote_file_t;
static remote_slot_t s_rs[POWER_RELAY_REMOTE_MAX];
static portMUX_TYPE s_rs_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_rs_dirty;          /* 판정 태스크가 저장 */
#define RS(idx) (&s_rs[(idx) - POWER_RELAY_REMOTE_FIRST])
static bool failsafe_valid(uint16_t m) { return m == 1 || m == 10 || m == 30 || m == 60; }

static power_relay_config_t s_relay_cfg[POWER_RELAY_COUNT];

/* ── 2026-10-09 릴레이 타이머(할 일 AG, power_relay.h 참고) ── */
#define POWER_RELAY_TIMER_PATH    FS_MOUNT_POINT "/power_relay_timer.bin"
#define POWER_RELAY_TIMER_VERSION 3   /* 2026-11-18 — 타이머 6개(원격 자리 4). 2(10-18, 3개)·1(2개)은 옮김 */
#define POWER_RELAY_TIMER_VERSION_V2 2
#define CLOCK_VALID_MIN_UNIX      1704067200u   /* 2024-01-01 — 이보다 작으면 콘 시계가 아직 안 맞은 것 */
#define UPTIME_UNTIL_FLAG         0x80000000u
#define POWER_RELAY_TIMER_VERSION_V1 1   /* 10-17까지 — 타이머 2개 */
typedef struct __attribute__((packed)) {
    uint32_t version;
    power_relay_timer_t timers[POWER_RELAY_COUNT];
} power_relay_timer_file_t;
typedef struct __attribute__((packed)) {
    uint32_t version;
    power_relay_timer_t timers[POWER_RELAY_LOCAL_COUNT];
} power_relay_timer_file_v1_t;
static power_relay_timer_t s_timer[POWER_RELAY_COUNT];

static void timer_defaults(power_relay_timer_t *t)
{
    memset(t, 0, sizeof(*t));
    t->on_sec = 15 * 60;
    t->off_sec = 45 * 60;
    t->day_start_min = 6 * 60;
    t->night_start_min = 18 * 60;
    t->night_on_sec = 15 * 60;
    t->night_off_sec = 45 * 60;
}

static void timer_save(void)
{
    FILE *f = fopen(POWER_RELAY_TIMER_PATH, "wb");
    if (!f) { ESP_LOGW(TAG, "Timer save failed (fopen)"); return; }
    power_relay_timer_file_t s = { .version = POWER_RELAY_TIMER_VERSION };
    memcpy(s.timers, s_timer, sizeof(s.timers));
    fwrite(&s, sizeof(s), 1, f);
    fclose(f);
}

static void timer_load(void)
{
    for (int i = 0; i < POWER_RELAY_COUNT; i++) timer_defaults(&s_timer[i]);
    FILE *f = fopen(POWER_RELAY_TIMER_PATH, "rb");
    if (!f) return;
    power_relay_timer_file_t s = { 0 };
    size_t got = fread(&s, 1, sizeof(s), f);
    fclose(f);
    if (got == sizeof(s) && s.version == POWER_RELAY_TIMER_VERSION) {
        memcpy(s_timer, s.timers, sizeof(s_timer));
    } else if (got == sizeof(uint32_t) + sizeof(power_relay_timer_t) * POWER_RELAY_OLD_COUNT && s.version == POWER_RELAY_TIMER_VERSION_V2) {
        memcpy(s_timer, s.timers, sizeof(power_relay_timer_t) * POWER_RELAY_OLD_COUNT);   /* 2026-11-18 — 원격 1개 → 첫 원격 자리 */
        ESP_LOGI(TAG, "Timer file v2 (3 relays) migrated");
        timer_save();
    } else if (got == sizeof(power_relay_timer_file_v1_t) && s.version == POWER_RELAY_TIMER_VERSION_V1) {
        memcpy(s_timer, s.timers, sizeof(power_relay_timer_t) * POWER_RELAY_LOCAL_COUNT);   /* 2026-10-18 — 앞 두 칸 그대로 */
        ESP_LOGI(TAG, "Timer file v1 (2 relays) migrated");
        timer_save();
    } else {
        ESP_LOGW(TAG, "Timer file format mismatch - keeping defaults");
    }
}

static bool clock_valid(uint32_t *out_now)
{
    uint32_t t = rtc_sync_get_unix_time();
    if (out_now) *out_now = t;
    return t >= CLOCK_VALID_MIN_UNIX;
}

/* 구간 안에서 켜짐? — elapsed: 구간 시작부터 지난 초. next: 다음 전환까지 남은 초 */
static bool cycle_state(uint32_t elapsed, uint32_t on_sec, uint32_t off_sec, uint32_t *next)
{
    uint32_t cyc = on_sec + off_sec;
    if (on_sec == 0) { if (next) *next = UINT32_MAX; return false; }
    if (off_sec == 0) { if (next) *next = UINT32_MAX; return true; }
    uint32_t pos = elapsed % cyc;
    if (pos < on_sec) { if (next) *next = on_sec - pos; return true; }
    if (next) *next = cyc - pos;
    return false;
}

bool power_relay_timer_state(const power_relay_timer_t *t, uint32_t *out_next_sec)
{
    uint32_t now = 0, next = UINT32_MAX;
    bool on;
    if (!clock_valid(&now)) {
        /* 시계가 아직 — 부팅부터 센 시간으로, 하루 종일(낮) 값 */
        uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
        on = cycle_state(up, t->on_sec, t->off_sec, &next);
    } else {
        uint32_t sod = now % 86400u;   /* 콘 시각은 현지 시각을 초로 셈(rtc_sync) */
        if (!t->day_night) {
            on = cycle_state(sod, t->on_sec, t->off_sec, &next);
            uint32_t to_midnight = 86400u - sod;
            if (to_midnight < next) next = to_midnight;
        } else {
            uint32_t ds = (uint32_t)t->day_start_min * 60u, ns = (uint32_t)t->night_start_min * 60u;
            bool in_day = (ds <= ns) ? (sod >= ds && sod < ns) : (sod >= ds || sod < ns);
            uint32_t seg_start = in_day ? ds : ns, seg_end = in_day ? ns : ds;
            uint32_t elapsed = (sod + 86400u - seg_start) % 86400u;
            uint32_t to_end = (seg_end + 86400u - sod) % 86400u;
            if (to_end == 0) to_end = 86400u;
            on = in_day ? cycle_state(elapsed, t->on_sec, t->off_sec, &next)
                        : cycle_state(elapsed, t->night_on_sec, t->night_off_sec, &next);
            if (to_end < next) next = to_end;
        }
    }
    if (out_next_sec) *out_next_sec = next;
    return on;
}

const power_relay_timer_t *power_relay_get_timer(int idx)
{
    if (idx < 0 || idx >= POWER_RELAY_COUNT) return NULL;
    return &s_timer[idx];
}

void power_relay_set_timer(int idx, const power_relay_timer_t *t)
{
    if (idx < 0 || idx >= POWER_RELAY_COUNT || !t) return;
    uint32_t keep_release = s_timer[idx].manual_release_sec, keep_until = s_timer[idx].manual_until;
    s_timer[idx] = *t;
    s_timer[idx].manual_release_sec = keep_release;   /* 수동 자동해제는 power_relay_set_manual_release로만 */
    s_timer[idx].manual_until = keep_until;
    if (s_timer[idx].day_start_min > 1439) s_timer[idx].day_start_min = 1439;
    if (s_timer[idx].night_start_min > 1439) s_timer[idx].night_start_min = 1439;
    timer_save();
    power_relay_kick();
}

void power_relay_set_manual_release(int idx, uint32_t sec)
{
    if (idx < 0 || idx >= POWER_RELAY_COUNT) return;
    s_timer[idx].manual_release_sec = sec;
    uint32_t now = 0;
    if (sec == 0) s_timer[idx].manual_until = 0;
    else if (clock_valid(&now)) s_timer[idx].manual_until = now + sec;
    else s_timer[idx].manual_until = UPTIME_UNTIL_FLAG | ((uint32_t)(esp_timer_get_time() / 1000000) + sec);
    timer_save();
}

bool power_relay_manual_remaining_sec(int idx, uint32_t *out_sec)
{
    if (idx < 0 || idx >= POWER_RELAY_COUNT || !s_relay_cfg[idx].manual_override) return false;
    uint32_t u = s_timer[idx].manual_until, now = 0;
    if (u == 0) return false;
    if (u & UPTIME_UNTIL_FLAG) now = (uint32_t)(esp_timer_get_time() / 1000000), u &= ~UPTIME_UNTIL_FLAG;
    else if (!clock_valid(&now)) return false;
    if (out_sec) *out_sec = (u > now) ? u - now : 0;
    return true;
}

/* 수동 강제의 자동 해제 시각이 지났나 */
static bool manual_release_due(int idx)
{
    uint32_t u = s_timer[idx].manual_until;
    if (u == 0) return false;
    if (u & UPTIME_UNTIL_FLAG) return (uint32_t)(esp_timer_get_time() / 1000000) >= (u & ~UPTIME_UNTIL_FLAG);
    uint32_t now = 0;
    return clock_valid(&now) && now >= u;
}

/* 2026-10-05 — 릴레이마다 로마자 별명(PSRAM, 저장 안 함) */
static char (*s_relay_alias_disp)[POWER_RELAY_ALIAS_DISP_LEN] = NULL;

static void relay_alias_disp_refresh(int i)
{
    if (!s_relay_alias_disp) s_relay_alias_disp = heap_caps_calloc(POWER_RELAY_COUNT, POWER_RELAY_ALIAS_DISP_LEN, MALLOC_CAP_SPIRAM);
    if (!s_relay_alias_disp || i < 0 || i >= POWER_RELAY_COUNT) return;
    romanize_hangul(s_relay_cfg[i].alias, s_relay_alias_disp[i], POWER_RELAY_ALIAS_DISP_LEN);
}
static bool     s_commanded_on[POWER_RELAY_COUNT];
static uint32_t s_last_transition_ms[POWER_RELAY_COUNT];
/* 2026-11-12(기준 측정기 상태) — 릴레이마다 기준 상태, 판단 대기 시작 시각, 기준값을 낸 쪽(0 없음, 1 주, 2 대신), 추세를 기록으로 다시 채울지 */
static power_relay_ref_t s_ref[POWER_RELAY_COUNT];
static uint32_t s_wait_since_ms[POWER_RELAY_COUNT];
static uint8_t  s_ref_side[POWER_RELAY_COUNT];
static uint32_t s_ref_ids_sig[POWER_RELAY_COUNT];   /* 2026-11-13 — 기준 쪽 센싱 목록 요약(바뀌면 이력 끊김) */
static bool     s_trend_refill[POWER_RELAY_COUNT];   /* 부팅 때 한 번(power_relay_load가 모두 true로) */
static power_trend_buf_t s_trend[POWER_RELAY_COUNT];

/* 2026-09-18(사용자 설계 — "부팅 때에는... 설정이 되있더라도 오프로 시작... 설정에 의해
 * 온 되는 시간이 필요") — 원인(자동규칙/Override) 무관하게 부팅 후 최초 1회 On 전환만
 * 이 유예시간만큼 지연. GPIO 자체는 이미 power_relay_start()에서 물리적으로 Low로
 * 초기화됨 — 이건 그 이후 "언제 처음 켜도 되는가"에 대한 소프트웨어 유예 */
#define POWER_RELAY_BOOT_ON_DELAY_MS (60 * 1000)
static uint32_t s_boot_ms_ref;
static bool     s_ever_on_since_boot[POWER_RELAY_COUNT];
/* 2026-09-17 — "진짜 새 측정값"만 추세 샘플로 인정하기 위한 직전값 캐시(측정횟수 기준
 * 재설계, power_relay.h의 trend_sample_count 주석 참고) */
static float    s_trend_last_value[POWER_RELAY_COUNT];
static bool     s_trend_has_last_value[POWER_RELAY_COUNT];

static void power_relay_save(void)
{
    FILE *f = fopen(POWER_RELAY_FILE_PATH, "wb");
    if (!f) {
        ESP_LOGW(TAG, "Save failed (fopen): %s", POWER_RELAY_FILE_PATH);
        return;
    }
    power_relay_file_t *s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_SPIRAM);
    if (!s) { fclose(f); return; }
    s->version = POWER_RELAY_FILE_VERSION;
    memcpy(s->relays, s_relay_cfg, sizeof(s->relays));
    fwrite(s, sizeof(*s), 1, f);
    fclose(f);
    heap_caps_free(s);
}

static void remote_save(void)
{
    remote_file_t r = { .version = POWER_REMOTE_VERSION };
    taskENTER_CRITICAL(&s_rs_lock);
    memcpy(r.slot, s_rs, sizeof(r.slot));
    s_rs_dirty = false;
    taskEXIT_CRITICAL(&s_rs_lock);
    FILE *f = fopen(POWER_REMOTE_PATH, "wb");
    if (!f) { ESP_LOGW(TAG, "Save failed (fopen): %s", POWER_REMOTE_PATH); return; }
    fwrite(&r, sizeof(r), 1, f);
    fclose(f);
}

/* migrated_fs_min: 옛 설정 파일(버전 3·4)의 원격릴레이 끊김 시간 — 원격 자리 파일이 없을 때 첫 자리에 */
static void remote_load(uint16_t migrated_fs_min)
{
    memset(s_rs, 0, sizeof(s_rs));
    for (int i = 0; i < POWER_RELAY_REMOTE_MAX; i++) s_rs[i].failsafe_min = POWER_REMOTE_FAILSAFE_DEFAULT_MIN;
    FILE *f = fopen(POWER_REMOTE_PATH, "rb");
    if (!f) {
        if (failsafe_valid(migrated_fs_min)) s_rs[0].failsafe_min = migrated_fs_min;
        return;
    }
    remote_file_t r;
    size_t got = fread(&r, 1, sizeof(r), f);
    fclose(f);
    if (got != sizeof(r) || r.version != POWER_REMOTE_VERSION) { ESP_LOGW(TAG, "power_remote.bin format mismatch - ignored"); return; }
    memcpy(s_rs, r.slot, sizeof(s_rs));
    for (int i = 0; i < POWER_RELAY_REMOTE_MAX; i++) {
        if (!failsafe_valid(s_rs[i].failsafe_min)) s_rs[i].failsafe_min = POWER_REMOTE_FAILSAFE_DEFAULT_MIN;
        if (s_rs[i].owner) ESP_LOGI(TAG, "Remote %d = %02X%02X%02X%02X%02X%02X%s", i + 1, s_rs[i].mac[0], s_rs[i].mac[1], s_rs[i].mac[2],
                                    s_rs[i].mac[3], s_rs[i].mac[4], s_rs[i].mac[5], s_rs[i].linked ? "" : " (not linked)");
    }
}

static void relay_defaults(int i)
{
    memset(&s_relay_cfg[i], 0, sizeof(s_relay_cfg[i]));
    s_relay_cfg[i].min_hold_sec = 60;          /* 기본 1분 — 사용자가 팝업에서 조정 */
    s_relay_cfg[i].trend_sample_count = 5;
    s_relay_cfg[i].ai_mode = true;  /* 2026-09-16 — 기본은 AI On(간단한 화면) */
    s_relay_cfg[i].y_rises = true;     /* 기본 표시: 도달(Up to) */
    s_relay_cfg[i].z_turns_on = true;  /* 기본 표시: 켜짐(On) — direction 기본값(ON_ABOVE=0)과 일치 */
    /* 2026-09-17(사용자 지시 — "OnOff=On, 계열=온도, Up/Below=Up, 온도값 20도, 온도오차
     * 2도") — 채널/중심/오차 디폴트. ui_main.c의 relay_default_center_margin_for_channel()과
     * 같은 숫자(20±2, 온도) — 이 파일은 UI 헬퍼를 모르므로 그대로 재기술 */
    s_relay_cfg[i].chan_type = SENSOR_CHAN_TEMP_C;
    s_relay_cfg[i].on_threshold = 22.0f;   /* center(20) + margin(2), direction=ON_ABOVE */
    s_relay_cfg[i].off_threshold = 18.0f;  /* center(20) - margin(2) */
}

void power_relay_load(void)
{
    for (int i = 0; i < POWER_RELAY_COUNT; i++) {
        relay_defaults(i);
        s_trend_refill[i] = true;
    }
    timer_load();   /* 2026-10-09 — 타이머 설정(따로 저장) */
    uint16_t fs_min = 0;
    bool migrated = false;
    FILE *f = fopen(POWER_RELAY_FILE_PATH, "rb");
    if (f) {
        power_relay_file_t *s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_SPIRAM);   /* 스택에 두지 않음 */
        if (s) {
            size_t got = fread(s, 1, sizeof(*s), f);
            const uint8_t *p = (const uint8_t *)s + sizeof(uint32_t);
            if (got == sizeof(*s) && s->version == POWER_RELAY_FILE_VERSION) {
                memcpy(s_relay_cfg, s->relays, sizeof(s_relay_cfg));
            } else if (got == POWER_RELAY_FILE_V4_SIZE && s->version == 4) {
                /* 2026-11-18 — 버전 4(릴레이 3개 + 끊김 시간): 원격릴레이 설정은 첫 원격 자리(주인 없음 — 처음 연결되는 기기가 이어받음) */
                memcpy(s_relay_cfg, p, sizeof(power_relay_config_t) * POWER_RELAY_OLD_COUNT);
                memcpy(&fs_min, p + sizeof(power_relay_config_t) * POWER_RELAY_OLD_COUNT, sizeof(fs_min));
                migrated = true;
            } else if ((got == POWER_RELAY_FILE_V3_SIZE && s->version == 3) || (got == POWER_RELAY_FILE_V2_SIZE && s->version == 2)) {
                /* 2026-11-12 — 옛 파일(설정 구조체에 active 없음): 칸마다 앞부분을 그대로 옮기고, 설정됨 = 센서 조건 적용 또는 타이머
                 * (타이머는 위 timer_load()에서 이미 읽음). 버전 2(릴레이 2개, 10-17까지)는 원격릴레이 칸이 기본값 */
                int n = (s->version == 3) ? POWER_RELAY_OLD_COUNT : POWER_RELAY_LOCAL_COUNT;
                for (int i = 0; i < n; i++) memcpy(&s_relay_cfg[i], p + (size_t)i * POWER_RELAY_CFG_V3_SIZE, POWER_RELAY_CFG_V3_SIZE);
                for (int i = 0; i < n; i++) s_relay_cfg[i].active = s_relay_cfg[i].configured || s_timer[i].timer_mode;
                if (s->version == 3) memcpy(&fs_min, p + (size_t)POWER_RELAY_OLD_COUNT * POWER_RELAY_CFG_V3_SIZE, sizeof(fs_min));
                migrated = true;
            } else {
                ESP_LOGW(TAG, "Config file format mismatch - keeping defaults");
            }
            if (migrated) ESP_LOGI(TAG, "Config file v%u migrated to v%d", (unsigned)s->version, POWER_RELAY_FILE_VERSION);
            heap_caps_free(s);
        }
        fclose(f);
    }
    remote_load(fs_min);
    for (int i = 0; i < POWER_RELAY_COUNT; i++) {
        s_relay_cfg[i].alias[sizeof(s_relay_cfg[i].alias) - 1] = '\0';
        relay_alias_disp_refresh(i);
    }
    if (migrated) {
        power_relay_save();
        remote_save();
    }
    ESP_LOGI(TAG, "Config restored (active: %d %d, remote slots linked: %d %d %d %d)", (int)s_relay_cfg[0].active, (int)s_relay_cfg[1].active,
             s_rs[0].linked, s_rs[1].linked, s_rs[2].linked, s_rs[3].linked);
}

const char *power_relay_get_alias_display(int idx)
{
    if (idx < 0 || idx >= POWER_RELAY_COUNT || !s_relay_alias_disp) return "";
    return s_relay_alias_disp[idx];
}

const power_relay_config_t *power_relay_get_config(int idx)
{
    if (idx < 0 || idx >= POWER_RELAY_COUNT) return NULL;
    return &s_relay_cfg[idx];
}

/* relay_set_output()/power_relay_command() 본문보다 앞에서 쓰여서 fwd 필요 */
static void power_relay_apply_override_immediate(int idx);
void power_relay_kick(void);

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
    if (!cfg->manual_override && s_timer[idx].manual_until) {   /* 2026-10-09 — 강제를 끄면 자동 해제 예약도 없앰 */
        s_timer[idx].manual_until = 0;
        timer_save();
    }
    relay_alias_disp_refresh(idx);
    /* 설정이 바뀌면 추세 이력/최근전환시각을 리셋 — 새 소스/방향/임계값 기준으로 처음부터
     * 다시 판단해야지, 이전 소스 기준으로 쌓인 샘플을 섞어 쓰면 안 됨 */
    memset(&s_trend[idx], 0, sizeof(s_trend[idx]));
    s_trend_has_last_value[idx] = false;
    s_trend_refill[idx] = true;   /* 2026-11-12 — 비우기만 하지 않고 다음 판정 때 기록으로 다시 채움 */
    s_last_transition_ms[idx] = 0;
    power_relay_save();
    /* 2026-09-18(Manual Override, 사용자 설계) — Override를 켠 순간(팝업 OK) 다음 15초
     * 판정주기까지 안 기다리고 바로 반영 */
    power_relay_apply_override_immediate(idx);
    power_relay_kick();   /* 2026-10-09 — 타이머·조건도 바로 다시 판정 */
}

bool power_relay_get_commanded_on(int idx)
{
    if (idx < 0 || idx >= POWER_RELAY_COUNT) return false;
    return s_commanded_on[idx];
}

/* 2026-10-18 — 원격릴레이는 출력이 무선이라 여기선 명령만 바꾸고 판정 태스크를 깨움 — 실제 전송·확인(보릴의 SET_ACK)은 remote_sync()가
 * 판정 태스크에서 함(신뢰 전송이 최대 약 2초 걸려서, 화면 태스크에서 부르면 화면이 멈춤) */
static void relay_set_output(int idx, bool on)
{
    if (idx >= POWER_RELAY_LOCAL_COUNT) {
        power_relay_kick();
        return;
    }
    esp_err_t err = ch422g_set_do(s_relay_do_bit[idx], !on);  /* Low = 켜짐 */
    sd_health_mark(SDH_MARK_RELAY, (uint32_t)idx * 10 + (on ? 1 : 0));   /* 2026-10-21 — SD 명령 기록에 릴레이 바뀜 */
    if (err != ESP_OK) ESP_LOGE(TAG, "Relay%d: DO write failed (%s)", idx, esp_err_to_name(err));
}

bool power_relay_is_remote(int idx) { return idx >= POWER_RELAY_REMOTE_FIRST && idx < POWER_RELAY_COUNT; }

bool power_relay_is_shown(int idx)
{
    if (idx < 0 || idx >= POWER_RELAY_COUNT) return false;
    if (!power_relay_is_remote(idx)) return true;
    return RS(idx)->owner && RS(idx)->linked;
}

bool power_relay_remote_mac(int idx, uint8_t mac_out[6])
{
    if (!power_relay_is_remote(idx)) return false;
    taskENTER_CRITICAL(&s_rs_lock);
    bool v = RS(idx)->owner;
    if (v) memcpy(mac_out, RS(idx)->mac, 6);
    taskEXIT_CRITICAL(&s_rs_lock);
    return v;
}

static int remote_of_locked(const uint8_t *mac)
{
    for (int i = 0; i < POWER_RELAY_REMOTE_MAX; i++) if (s_rs[i].owner && memcmp(s_rs[i].mac, mac, 6) == 0) return POWER_RELAY_REMOTE_FIRST + i;
    return -1;
}

int power_relay_remote_of(const uint8_t *mac)
{
    taskENTER_CRITICAL(&s_rs_lock);
    int r = remote_of_locked(mac);
    taskEXIT_CRITICAL(&s_rs_lock);
    return r;
}

bool power_relay_remote_linked(const uint8_t *mac)
{
    taskENTER_CRITICAL(&s_rs_lock);
    int r = remote_of_locked(mac);
    bool v = r >= 0 && RS(r)->linked;
    taskEXIT_CRITICAL(&s_rs_lock);
    return v;
}

/* 새 기기에 줄 자리: 주인 없는 자리(설정 그대로 — 처음이면 기본값, 옛 원격릴레이 설정을 옮긴 자리면 그 설정), 없으면 주인이 끊긴 자리
 * (새 기기라 설정 초기화). 다 연결돼 있으면 -1. *reset = 설정을 지워야 함 */
static int remote_free_locked(bool *reset)
{
    *reset = false;
    for (int i = 0; i < POWER_RELAY_REMOTE_MAX; i++) if (!s_rs[i].owner) return POWER_RELAY_REMOTE_FIRST + i;
    for (int i = 0; i < POWER_RELAY_REMOTE_MAX; i++) if (!s_rs[i].linked) { *reset = true; return POWER_RELAY_REMOTE_FIRST + i; }
    return -1;
}

bool power_relay_remote_can_take(const uint8_t *mac)
{
    bool reset;
    taskENTER_CRITICAL(&s_rs_lock);
    bool ok = remote_of_locked(mac) >= 0 || remote_free_locked(&reset) >= 0;
    taskEXIT_CRITICAL(&s_rs_lock);
    return ok;
}

static bool s_remote_adopted[POWER_RELAY_REMOTE_MAX];   /* 그 자리 기기가 알려 준 상태를 명령으로 받아들였음 — 콘 재부팅으로 보일러가 꺼지지 않게 */
static uint32_t s_remote_last_try_ms[POWER_RELAY_REMOTE_MAX];
static volatile bool s_remote_reset_req[POWER_RELAY_REMOTE_MAX];   /* 새 기기가 끊긴 주인의 자리를 씀 — 판정 태스크가 설정 초기화 */
static volatile bool s_remote_unforce_req[POWER_RELAY_REMOTE_MAX]; /* 사용자가 연결을 끊음 — 판정 태스크가 강제를 풀고 저장 */

void power_relay_remote_link(const uint8_t *mac)
{
    bool reset = false;
    taskENTER_CRITICAL(&s_rs_lock);
    int r = remote_of_locked(mac);
    if (r < 0) r = remote_free_locked(&reset);
    bool changed = false;
    if (r >= 0) {
        remote_slot_t *sl = RS(r);
        changed = !sl->owner || !sl->linked || memcmp(sl->mac, mac, 6) != 0;
        sl->owner = 1;
        sl->linked = 1;
        memcpy(sl->mac, mac, 6);
        if (changed) { s_rs_dirty = true; s_remote_adopted[r - POWER_RELAY_REMOTE_FIRST] = false; }
        if (reset) s_remote_reset_req[r - POWER_RELAY_REMOTE_FIRST] = true;
    }
    taskEXIT_CRITICAL(&s_rs_lock);
    if (r < 0) { ESP_LOGW(TAG, "Remote %02X%02X%02X: no free remote slot", mac[3], mac[4], mac[5]); return; }
    if (changed) ESP_LOGI(TAG, "Remote %02X%02X%02X linked to relay %d%s", mac[3], mac[4], mac[5], r, reset ? " (slot reused - settings reset)" : "");
    power_relay_kick();
}

void power_relay_remote_unlink(const uint8_t *mac)
{
    taskENTER_CRITICAL(&s_rs_lock);
    int r = remote_of_locked(mac);
    if (r >= 0 && RS(r)->linked) {
        RS(r)->linked = 0;
        s_rs_dirty = true;
        s_remote_adopted[r - POWER_RELAY_REMOTE_FIRST] = false;
    } else {
        r = -1;
    }
    taskEXIT_CRITICAL(&s_rs_lock);
    if (r < 0) return;
    s_commanded_on[r] = false;   /* 연결을 끊은 자리 — 다시 연결되면 그 기기 상태를 다시 받아들임. 기기는 해제를 받으면 스스로 끔(보릴 11-18) */
    s_remote_unforce_req[r - POWER_RELAY_REMOTE_FIRST] = true;   /* 2026-11-18(사용자 결정) — 사용자가 끊으면 강제(ON/OFF)만 풀고 나머지 설정은 남김. 통신 끊김은 해당 없음 */
    ESP_LOGI(TAG, "Remote %02X%02X%02X unlinked from relay %d (settings kept)", mac[3], mac[4], mac[5], r);
    power_relay_kick();
}

uint16_t power_relay_get_remote_failsafe_min(int idx)
{
    return power_relay_is_remote(idx) ? RS(idx)->failsafe_min : POWER_REMOTE_FAILSAFE_DEFAULT_MIN;
}

uint16_t power_relay_remote_failsafe_for_mac(const uint8_t *mac)
{
    int r = power_relay_remote_of(mac);
    return r >= 0 ? RS(r)->failsafe_min : POWER_REMOTE_FAILSAFE_DEFAULT_MIN;
}

void power_relay_set_remote_failsafe_min(int idx, uint16_t min)
{
    if (!power_relay_is_remote(idx) || !failsafe_valid(min) || RS(idx)->failsafe_min == min) return;
    RS(idx)->failsafe_min = min;
    remote_save();
    ESP_LOGI(TAG, "Relay %d failsafe %u min", idx, (unsigned)min);   /* 기기는 다음 STATUS_ACK(10초 안)로 받음 */
}

/* 원격 자리 상태 — 그 자리 기기의 노드 정보(node_hub)와 명령을 견줌 */
void power_relay_get_remote_status(int idx, power_remote_status_t *out)
{
    memset(out, 0, sizeof(*out));
    uint8_t mac[6];
    if (!power_relay_is_shown(idx) || !power_relay_remote_mac(idx, mac)) return;
    node_hub_node_t *n = heap_caps_malloc(sizeof(*n), MALLOC_CAP_SPIRAM);
    if (!n) return;
    if (node_hub_get_boril(mac, n) && n->boril_has_status) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        uint32_t timeout_ms = node_hub_node_timeout_ms(n);
        uint32_t elapsed = now_ms - n->last_seen_ms;
        out->actual_on = n->boril_relay_on;
        out->has_rssi = n->has_rssi;
        out->rssi = n->rssi;
        out->near_orphan = (uint64_t)elapsed * 10 >= (uint64_t)timeout_ms * 8;
        out->state = (n->boril_relay_on == s_commanded_on[idx] || !s_remote_adopted[idx - POWER_RELAY_REMOTE_FIRST]) ? POWER_REMOTE_OK
                                                                                                                    : POWER_REMOTE_PENDING;
    }
    heap_caps_free(n);
}

/* 판정 태스크에서 — 자리마다 명령과 기기의 실제 상태가 다르면 보냄(기기 재부팅으로 꺼진 채 돌아온 경우도 맞춰짐). 5초에 한 번까지 */
static void remote_sync(uint32_t now_ms)
{
    node_hub_node_t *n = heap_caps_malloc(sizeof(*n), MALLOC_CAP_SPIRAM);
    if (!n) return;
    for (int r = POWER_RELAY_REMOTE_FIRST; r < POWER_RELAY_COUNT; r++) {
        int k = r - POWER_RELAY_REMOTE_FIRST;
        uint8_t mac[6];
        if (!power_relay_is_shown(r) || !power_relay_remote_mac(r, mac)) continue;
        if (!node_hub_get_boril(mac, n) || !n->boril_has_status) continue;
        if (!s_remote_adopted[k]) {
            s_remote_adopted[k] = true;
            s_commanded_on[r] = n->boril_relay_on;
            s_ever_on_since_boot[r] = n->boril_relay_on;
            s_last_transition_ms[r] = now_ms;
            ESP_LOGI(TAG, "Relay %d state adopted from remote %02X%02X%02X: %s", r, mac[3], mac[4], mac[5], n->boril_relay_on ? "On" : "Off");
        }
        if (n->boril_relay_on == s_commanded_on[r] || (s_remote_last_try_ms[k] && now_ms - s_remote_last_try_ms[k] < 5000)) continue;
        s_remote_last_try_ms[k] = now_ms;
        esp_err_t err = node_hub_boril_set(mac, s_commanded_on[r]);
        if (err != ESP_OK) ESP_LOGW(TAG, "Relay %d %s not confirmed (%s) - retrying", r, s_commanded_on[r] ? "On" : "Off", esp_err_to_name(err));
    }
    heap_caps_free(n);
}

static void power_relay_command_ex(int idx, bool on, uint32_t now_ms, bool by_timer)
{
    if (s_commanded_on[idx] == on) return;
    s_commanded_on[idx] = on;
    s_last_transition_ms[idx] = now_ms;
    relay_set_output(idx, on);
    ESP_LOGI(TAG, "Relay%d(%s) -> %s%s", idx, s_relay_cfg[idx].alias, on ? "On" : "Off", by_timer ? " (timer)" : "");
    /* 2026-10-09 — 타이머 전환은 주기가 짧으면 알림이 많아져 기록만(푸시 없음) */
    void (*post)(notify_type_t, const char *, const char *, ...) = by_timer ? alarm_record : alarm_post;
    if (s_relay_cfg[idx].alias[0]) post(NOTIFY_TYPE_RELAY, on ? "Relay ON" : "Relay OFF", "%s %s%s", s_relay_cfg[idx].alias, on ? "ON" : "OFF", by_timer ? " (timer)" : "");
    else if (power_relay_is_remote(idx)) post(NOTIFY_TYPE_RELAY, on ? "Relay ON" : "Relay OFF", "Remote relay %d %s%s", idx - POWER_RELAY_REMOTE_FIRST + 1,
                                              on ? "ON" : "OFF", by_timer ? " (timer)" : "");
    else post(NOTIFY_TYPE_RELAY, on ? "Relay ON" : "Relay OFF", "Relay %d %s%s", idx + 1, on ? "ON" : "OFF", by_timer ? " (timer)" : "");
}

static void power_relay_command(int idx, bool on, uint32_t now_ms)
{
    power_relay_command_ex(idx, on, now_ms, false);
}

static void power_relay_apply_override_immediate(int idx)
{
    power_relay_config_t *cfg = &s_relay_cfg[idx];
    if (!cfg->manual_override) return;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    bool want_on = cfg->manual_override_on;
    if (want_on == s_commanded_on[idx]) return;
    /* Override라도 부팅 유예는 그대로 존중(원인 무관 규칙, 사용자 지시) */
    if (want_on && !s_ever_on_since_boot[idx] &&
        (now_ms - s_boot_ms_ref) < POWER_RELAY_BOOT_ON_DELAY_MS) {
        return;
    }
    power_relay_command(idx, want_on, now_ms);
    if (want_on) s_ever_on_since_boot[idx] = true;
}

/* 최소제곱 1차회귀 기울기(값/ms) — 추세(방향+속도)만 필요하므로 1차식으로 충분(그래프의
 * 2차 국소회귀와 달리 여기선 "부드러운 곡선"이 아니라 "지금 어느 쪽으로 얼마나 빠르게
 * 가는지"만 필요).
 * 2026-09-17(사용자 지적 — "측정 주기가 길면 60초래봐야 하나도 없을 수 있는데? 측정
 * 횟수 아니야?") — 시간 창 대신, 링버퍼에 실제로 쌓인 것 중 가장 최근 sample_count개를
 * 그대로 씀(trend_push가 "값이 바뀐 진짜 새 측정값"만 넣으므로, 여기 있는 건 전부 유효한
 * 서로 다른 샘플). 링버퍼가 원형이라 buf->next 바로 앞(가장 최근)부터 거꾸로 훑음.
 * out_span_ms에 실제로 쓴 샘플들이 걸친 시간폭을 같이 반환(예측폭으로 재사용) */
static float trend_slope_per_ms(const power_trend_buf_t *buf, uint32_t now_ms, int sample_count,
                                 uint32_t *out_span_ms)
{
    if (out_span_ms) *out_span_ms = 0;
    int n = (sample_count < buf->count) ? sample_count : buf->count;
    if (n < 2) return 0.0f;

    double sum_t = 0, sum_v = 0, sum_tt = 0, sum_tv = 0;
    int idx = (buf->next - 1 + POWER_TREND_SAMPLES) % POWER_TREND_SAMPLES;
    uint32_t oldest_used_t = buf->t_ms[idx];
    for (int i = 0; i < n; i++) {
        double t = (double)(buf->t_ms[idx] - now_ms);  /* now_ms 기준 상대시각(항상 <=0) —
                                                            정밀도 손실 방지 */
        double v = (double)buf->v[idx];
        sum_t += t; sum_v += v; sum_tt += t * t; sum_tv += t * v;
        oldest_used_t = buf->t_ms[idx];
        idx = (idx - 1 + POWER_TREND_SAMPLES) % POWER_TREND_SAMPLES;
    }
    if (out_span_ms) *out_span_ms = now_ms - oldest_used_t;
    double dn = (double)n;
    double denom = dn * sum_tt - sum_t * sum_t;
    if (fabs(denom) < 1e-6) return 0.0f;
    return (float)((dn * sum_tv - sum_t * sum_v) / denom);
}

static void trend_push(power_trend_buf_t *buf, uint32_t now_ms, float value)
{
    buf->t_ms[buf->next] = now_ms;
    buf->v[buf->next] = value;
    buf->next = (buf->next + 1) % POWER_TREND_SAMPLES;
    if (buf->count < POWER_TREND_SAMPLES) buf->count++;
}

/* ════════════════════════════════════════════════════════════
 * 2026-11-12(사용자와 대화로 확정 — Docs/한일_2026-09-28.txt 11-12절 6·7번) — 기준 측정기 상태
 *  - 센서 AI(공기 온습도): 정밀이 "사용 가능"이면 정밀, 아니면 간이 — Apply 때 고정하지 않고 판정할 때마다 정함.
 *  - 센서 수동·그 밖의 값 종류: 고른 쪽만(대신하지 않음).
 *  - 기준값 = 쓰는 쪽의 사용 가능한 측정기들의 마지막 측정값(평균/최대/최소). 예전의 최근 5분 통계 창은 쓰지 않음.
 * ════════════════════════════════════════════════════════════ */
typedef struct {
    sensing_st_t st;                      /* 쪽 전체의 상태(사용 가능 > 확인 중 > 끊김 > 고장 > 미등록) */
    int      n;                           /* 속한 센싱 수 */
    uint16_t ids[SREG_MAX_SENSINGS];
    int      n_usable;
    float    v_sum, v_min, v_max;
} ref_side_t;

static sreg_sensing_t *s_ref_list = NULL;   /* 판정 태스크 전용 작업 칸(PSRAM) */

static bool ref_member(const power_relay_config_t *cfg, const sreg_sensing_t *g, bool precise)
{
    if (g->what != (uint8_t)cfg->chan_type || g->what == SREG_WHAT_CAMERA) return false;
    if (cfg->source_kind == POWER_SRC_SINGLE_DEVICE) {
        uint8_t mac[6];
        return sreg_sensing_board(g->id, mac) && memcmp(mac, cfg->device_mac, 6) == 0;
    }
    switch (cfg->group) {
        case POWER_GROUP_AIR:  return g->where == SENSING_WHERE_AIR && (g->precise != 0) == precise;
        case POWER_GROUP_AGAR: return g->where == SENSING_WHERE_SUBSTRATE;
        default:               return g->where == SENSING_WHERE_AIR;
    }
}

static int st_rank(sensing_st_t st)
{
    switch (st) {
        case SENSING_ST_USABLE:   return 4;
        case SENSING_ST_CHECKING: return 3;
        case SENSING_ST_LOST:     return 2;
        case SENSING_ST_FAULT:    return 1;
        default:                  return 0;
    }
}

static void ref_side_eval(const power_relay_config_t *cfg, const sreg_sensing_t *list, int nl, bool precise, ref_side_t *out)
{
    memset(out, 0, sizeof(*out));
    out->st = SENSING_ST_UNREG;
    for (int i = 0; i < nl; i++) {
        if (!ref_member(cfg, &list[i], precise)) continue;
        out->ids[out->n++] = list[i].id;
        float v = 0.0f;
        sensing_st_t st = sensing_state_get(list[i].id, &v);
        if (st_rank(st) > st_rank(out->st)) out->st = st;
        if (st != SENSING_ST_USABLE) continue;
        if (out->n_usable == 0) { out->v_min = out->v_max = v; }
        else { if (v < out->v_min) out->v_min = v; if (v > out->v_max) out->v_max = v; }
        out->v_sum += v;
        out->n_usable++;
    }
}

static float ref_side_value(const ref_side_t *sd, power_source_stat_t stat)
{
    switch (stat) {
        case POWER_STAT_MAX: return sd->v_max;
        case POWER_STAT_MIN: return sd->v_min;
        default:             return sd->v_sum / (float)sd->n_usable;
    }
}

/* 표 2 — 주 쪽(정밀, 또는 고른 쪽) 상태 × 대신 쪽(간이, 없으면 미등록) 상태 → 기준 상태. *out_side = 값을 낼 쪽(0 없음, 1 주, 2 대신) */
static power_relay_ref_t ref_from_sides(sensing_st_t p, sensing_st_t b, uint8_t *out_side)
{
    bool bu = (b == SENSING_ST_USABLE);
    *out_side = 0;
    switch (p) {
        case SENSING_ST_USABLE:
            *out_side = 1;
            return POWER_REF_MAIN;
        case SENSING_ST_CHECKING:
            if (bu) { *out_side = 2; return POWER_REF_BASIC_MAIN_CHECKING; }
            return POWER_REF_CHECKING;
        case SENSING_ST_LOST:
            if (bu) { *out_side = 2; return POWER_REF_SUB_LOST; }
            return POWER_REF_WAIT;
        case SENSING_ST_FAULT:
            if (bu) { *out_side = 2; return POWER_REF_SUB_FAULT; }
            return (b == SENSING_ST_CHECKING || b == SENSING_ST_LOST) ? POWER_REF_WAIT : POWER_REF_FAIL_FAULT;
        default:
            switch (b) {
                case SENSING_ST_USABLE:   *out_side = 2; return POWER_REF_BASIC;
                case SENSING_ST_CHECKING: return POWER_REF_CHECKING;
                case SENSING_ST_LOST:     return POWER_REF_WAIT;
                case SENSING_ST_FAULT:    return POWER_REF_FAIL_FAULT;
                default:                  return POWER_REF_FAIL_NONE;
            }
    }
}

/* 릴레이 하나의 기준 상태와 기준값. 값을 낼 쪽이 있으면 out_value·out_side(1·2)·out_ids/out_n(그 쪽의 센싱) 채움 */
static power_relay_ref_t ref_compute(const power_relay_config_t *cfg, float *out_value, uint8_t *out_side,
                                     uint16_t *out_ids, int *out_n)
{
    *out_side = 0;
    if (!s_ref_list) s_ref_list = heap_caps_malloc(sizeof(sreg_sensing_t) * SREG_MAX_SENSINGS, MALLOC_CAP_SPIRAM);
    if (!s_ref_list) return POWER_REF_CHECKING;
    int nl = sreg_list_sensings(s_ref_list, SREG_MAX_SENSINGS);
    static ref_side_t s_p, s_b;   /* 판정 태스크에서만 — 스택에 안 둠 */
    bool fallback = cfg->ai_mode && cfg->source_kind == POWER_SRC_GROUP_STAT && cfg->group == POWER_GROUP_AIR;
    bool main_precise = fallback ? true : cfg->precise;
    ref_side_eval(cfg, s_ref_list, nl, main_precise, &s_p);
    if (fallback) ref_side_eval(cfg, s_ref_list, nl, false, &s_b);
    else { memset(&s_b, 0, sizeof(s_b)); s_b.st = SENSING_ST_UNREG; }
    power_relay_ref_t r = ref_from_sides(s_p.st, s_b.st, out_side);
    const ref_side_t *use = (*out_side == 1) ? &s_p : (*out_side == 2) ? &s_b : NULL;
    if (use && use->n_usable > 0) {
        *out_value = ref_side_value(use, cfg->stat);
        memcpy(out_ids, use->ids, sizeof(uint16_t) * use->n);
        *out_n = use->n;
    } else {
        *out_side = 0;
    }
    return r;
}

/* 2026-11-12(7번) — 추세를 통계 기록(최근 30분)으로 다시 채움. 같은 쪽 센싱들의 기록을 시간순으로 훑으며, 기록마다 그때까지의
 * 센싱별 마지막 값으로 기준값(평균/최대/최소)을 만들어 값이 바뀔 때만 넣음(판정 때와 같은 규칙) */
#define POWER_TREND_REFILL_SEC   (30 * 60)
#define POWER_TREND_REFILL_CAP   512
static stats_record_t *s_refill_buf = NULL;

/* 돌려주는 값 = 최근 30분 안의 기준 측정기 기록 수(빠른 재측정 판단). 시계가 안 맞으면 0(기록을 못 봄) */
static int trend_refill(int idx, const power_relay_config_t *cfg, const uint16_t *ids, int n, uint32_t now_ms)
{
    memset(&s_trend[idx], 0, sizeof(s_trend[idx]));
    s_trend_has_last_value[idx] = false;
    uint32_t now_unix = rtc_sync_get_unix_time();
    if (now_unix < 1700000000u || n <= 0) return 0;   /* 시계가 안 맞음 — 기록 시각을 견줄 수 없음 */
    if (!s_refill_buf) s_refill_buf = heap_caps_malloc(sizeof(stats_record_t) * POWER_TREND_REFILL_CAP, MALLOC_CAP_SPIRAM);
    if (!s_refill_buf) return 0;
    uint32_t got = stats_store_read_since(now_unix - POWER_TREND_REFILL_SEC, (uint8_t)cfg->chan_type, s_refill_buf, POWER_TREND_REFILL_CAP);
    static float s_last_v[SREG_MAX_SENSINGS];
    static bool  s_has_v[SREG_MAX_SENSINGS];
    memset(s_has_v, 0, sizeof(s_has_v));
    int pushed = 0, matched = 0;
    for (uint32_t k = 0; k < got; k++) {
        const stats_record_t *rec = &s_refill_buf[k];
        int m = -1;
        for (int j = 0; j < n; j++) if (ids[j] == rec->sensing) { m = j; break; }
        if (m < 0 || rec->unix_time > now_unix) continue;
        matched++;
        s_last_v[m] = rec->value;
        s_has_v[m] = true;
        float sum = 0.0f, mn = 0.0f, mx = 0.0f;
        int c = 0;
        for (int j = 0; j < n; j++) {
            if (!s_has_v[j]) continue;
            float v = s_last_v[j];
            if (c == 0) { mn = mx = v; } else { if (v < mn) mn = v; if (v > mx) mx = v; }
            sum += v;
            c++;
        }
        float v = (cfg->stat == POWER_STAT_MAX) ? mx : (cfg->stat == POWER_STAT_MIN) ? mn : sum / (float)c;
        if (s_trend_has_last_value[idx] && v == s_trend_last_value[idx]) continue;
        trend_push(&s_trend[idx], now_ms - (now_unix - rec->unix_time) * 1000u, v);
        s_trend_last_value[idx] = v;
        s_trend_has_last_value[idx] = true;
        pushed++;
    }
    ESP_LOGI(TAG, "Relay%d trend refilled from records: %d samples (%d of %u records, 30 min)", idx, pushed, matched, (unsigned)got);
    return matched;
}

/* 2026-11-13(사용자 결정 11-12 — 빠른 재측정, Docs/한일_2026-09-28.txt 11-12절 7번) — 이력이 끊긴 사건(Apply, 기준 쪽·구성 바뀜, 끊김에서
 * 복구, 재부팅)으로 추세를 다시 채운 뒤, 최근 30분 기준 기록이 3개 미만이면 기준 측정기 보드마다 빠른 측정을 요청.
 * 횟수 = 이 릴레이의 추세 표본 수(최소 3)에서 이미 있는 기록 수를 뺀 만큼 */
#define POWER_FAST_MIN_RECORDS 3
static void fast_measure_if_short(int idx, const power_relay_config_t *cfg, const uint16_t *ids, int n, int have)
{
    if (have >= POWER_FAST_MIN_RECORDS || n <= 0) return;
    int need = (int)cfg->trend_sample_count;
    if (need < POWER_FAST_MIN_RECORDS) need = POWER_FAST_MIN_RECORDS;
    if (need > POWER_TREND_SAMPLES) need = POWER_TREND_SAMPLES;
    need -= have;
    static uint8_t s_boards[SREG_MAX_SENSINGS][6];   /* 판정 태스크에서만 — 스택에 안 둠 */
    int nb = 0;
    for (int i = 0; i < n; i++) {
        uint8_t mac[6];
        if (!sreg_sensing_board(ids[i], mac)) continue;
        bool dup = false;
        for (int b = 0; b < nb && !dup; b++) dup = memcmp(s_boards[b], mac, 6) == 0;
        if (dup) continue;
        memcpy(s_boards[nb++], mac, 6);
        node_hub_sens_fast_measure(mac, (uint8_t)need);
    }
    ESP_LOGI(TAG, "Relay%d(%s) only %d reference records in 30 min - fast measure x%d on %d board(s)", idx, cfg->alias, have, need, nb);
}

static const char *ref_name(power_relay_ref_t r)
{
    static const char *const names[] = { "none", "checking", "main", "basic", "basic (main checking)", "basic instead (main lost)",
                                         "basic instead (main fault)", "waiting", "waiting (off)", "cannot judge (fault)",
                                         "cannot judge (no sensor)" };
    return (unsigned)r < sizeof(names) / sizeof(names[0]) ? names[r] : "?";
}

static void ref_set(int idx, power_relay_ref_t r, uint32_t now_ms)
{
    if (r == POWER_REF_WAIT) {
        if (s_ref[idx] != POWER_REF_WAIT && s_ref[idx] != POWER_REF_WAIT_OFF) s_wait_since_ms[idx] = now_ms;
        if (now_ms - s_wait_since_ms[idx] >= POWER_RELAY_WAIT_HOLD_MS) r = POWER_REF_WAIT_OFF;
    }
    if (r != s_ref[idx]) {
        ESP_LOGI(TAG, "Relay%d(%s) reference: %s -> %s", idx, s_relay_cfg[idx].alias, ref_name(s_ref[idx]), ref_name(r));
        s_ref[idx] = r;
    }
}

power_relay_ref_t power_relay_get_ref(int idx)
{
    if (idx < 0 || idx >= POWER_RELAY_COUNT) return POWER_REF_NONE;
    return s_ref[idx];
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

    /* 2026-09-19(실기 버그 — "오버라이드는 센서 없어도 동작해야 되는 거야") — Override의
     * 반영 자체는 본설정 여부(configured/chan_type)나 참조 센서의 최신값 유무와 완전히
     * 무관해야 함. 예전엔 이 체크가 함수 맨 끝(297줄 부근)에만 있어서, configured==false
     * (본설정을 한 번도 안 거친 릴레이)거나 참조 센서가 아직 값을 안 보낸 상태(재부팅
     * 직후 흔함 — Sens는 자기 CASK 주기대로 뒤늦게 깨어남)면 오버라이드 여부를 보기도
     * 전에 함수가 리턴해버려서 Override가 영영 반영 안 되는 버그가 있었음(실기 확인:
     * "오버라이드 후 재부팅하고 1분 지나도 안 켜짐"). 여기서 독립적으로 먼저 처리 —
     * 아래(설정완료+센서값 필요) 자동판정 경로는 그대로 둠(센서 데이터가 있으면 추세
     * 이력을 계속 쌓아서, Override 해제 순간 바로 이어받을 수 있게 하는 기존 설계 유지 —
     * 아래 300줄 부근 주석 참고. 이 함수 끝에서 같은 조건을 한 번 더 평가하지만
     * currently_on이 이미 맞춰져 있어 그냥 조용히 no-op됨) */
    /* 2026-10-09 — 수동 강제의 자동 해제 시각이 지났으면 강제를 끔(그다음은 원래 동작 — 타이머 또는 센서 조건) */
    if (cfg->manual_override && manual_release_due(idx)) {
        cfg->manual_override = false;
        s_timer[idx].manual_until = 0;
        power_relay_save();
        timer_save();
        alarm_record(NOTIFY_TYPE_RELAY, "Relay manual end", "%s manual mode ended (auto release)",
                     s_relay_cfg[idx].alias[0] ? s_relay_cfg[idx].alias : "Relay");

    }
    if (cfg->manual_override) {
        bool currently_on = s_commanded_on[idx];
        bool want_on = cfg->manual_override_on;
        bool boot_delay_blocks = want_on && !s_ever_on_since_boot[idx] &&
                                  (now_ms - s_boot_ms_ref) < POWER_RELAY_BOOT_ON_DELAY_MS;
        if (want_on != currently_on && !boot_delay_blocks) {
            power_relay_command(idx, want_on, now_ms);
            if (want_on) s_ever_on_since_boot[idx] = true;
        }
    }

    /* 2026-11-12(사용자 설계) — 출력은 상태가 정함: 강제 값 → 설정(타이머/센서) 값 → 미설정이면 꺼짐. 해제 경로(자동·손)마다
     * 따로 끄지 않고, 매 판정마다 이 규칙으로 맞춤. 미설정 = active가 아님. 센서 방식인데 조건이 없거나(configured 아님) 채널이
     * 없으면(예전 Alias만 저장된 파일, 2026-09-16 가드) 판정할 수 없으니 미설정처럼 꺼짐 */
    bool use_timer = cfg->active && s_timer[idx].timer_mode;
    bool use_sensor = cfg->active && !s_timer[idx].timer_mode && cfg->configured && cfg->chan_type != SENSOR_CHAN_NONE;
    if (!use_sensor) ref_set(idx, POWER_REF_NONE, now_ms);
    if (!use_timer && !use_sensor) {
        if (!cfg->manual_override && s_commanded_on[idx]) power_relay_command(idx, false, now_ms);
        return;
    }

    /* 2026-10-09(할 일 AG) — 동작 = 타이머: 센서 조건 대신 시계로. 수동 강제 중이면 위에서 이미 처리 */
    if (use_timer) {
        if (cfg->manual_override) return;
        bool want_on = power_relay_timer_state(&s_timer[idx], NULL);
        if (want_on == s_commanded_on[idx]) return;
        if (want_on && !s_ever_on_since_boot[idx] && (now_ms - s_boot_ms_ref) < POWER_RELAY_BOOT_ON_DELAY_MS) return;
        power_relay_command_ex(idx, want_on, now_ms, true);
        if (want_on) s_ever_on_since_boot[idx] = true;
        return;
    }

    /* 여기부터 센서 방식(use_sensor) — configured·chan_type 가드는 위 출력 규칙에 합침(2026-11-12) */
    /* 2026-11-12(기준 측정기 상태 표 3) — 기준 상태가 동작을 정함: 확인 중·판단 대기(5분 안)는 출력 유지, 판단 대기 5분 지남·판단 불가는
     * 끔, 나머지(정밀 사용·간이 사용·간이 대신)는 그 쪽 값으로 판정. 강제 중이면 명령은 위 강제 블록이 함(아래 effective_want_on) */
    float value = 0.0f;
    uint8_t side = 0;
    static uint16_t s_ids[SREG_MAX_SENSINGS];   /* 판정 태스크에서만 */
    int n_ids = 0;
    power_relay_ref_t ref = ref_compute(cfg, &value, &side, s_ids, &n_ids);
    ref_set(idx, ref, now_ms);
    ref = s_ref[idx];
    if (side == 0) {
        s_ref_side[idx] = 0;   /* 2026-11-13 — 다시 값을 낼 때(끊김에서 복구 등) 이력이 끊긴 것으로 봄 */
        if (!cfg->manual_override && s_commanded_on[idx] &&
            (ref == POWER_REF_WAIT_OFF || ref == POWER_REF_FAIL_FAULT || ref == POWER_REF_FAIL_NONE)) {
            power_relay_command(idx, false, now_ms);
        }
        return;
    }
    /* 2026-11-13 — 기준 측정기 구성(쪽에 속한 센싱 목록)이 바뀌어도(새로 등록·해제) 이력이 끊긴 것으로 봄 */
    uint32_t ids_sig = 2166136261u ^ (uint32_t)n_ids;
    for (int i = 0; i < n_ids; i++) ids_sig = (ids_sig ^ s_ids[i]) * 16777619u;
    if (side != s_ref_side[idx] || ids_sig != s_ref_ids_sig[idx]) {   /* 기준값을 내는 쪽이 바뀜(정밀 <-> 간이·복구) — 추세는 새 쪽의 기록으로 */
        s_ref_side[idx] = side;
        s_ref_ids_sig[idx] = ids_sig;
        s_trend_refill[idx] = true;
    }
    if (s_trend_refill[idx] && (stats_store_index_ready() || !sd_storage_is_mounted())) {   /* 부팅 직후 SD 색인 전이면 다음 판정으로 미룸(그동안은 받은 값으로). SD가 없으면 플래시 기록만 */
        s_trend_refill[idx] = false;
        ESP_LOGI(TAG, "Relay%d(%s) history refill - reference %s, %s side, %d sensing(s)", idx, cfg->alias, ref_name(s_ref[idx]),
                 side == 1 ? "main" : "substitute", n_ids);
        fast_measure_if_short(idx, cfg, s_ids, n_ids, trend_refill(idx, cfg, s_ids, n_ids, now_ms));
    }
    /* 2026-09-17 — 값이 실제로 바뀐 경우만 "새 측정값"으로 인정해서 추세 샘플에 넣음(같은
     * 값이 반복 수신되는 건 센서가 그냥 안 자고 있을 뿐 새 정보가 아니므로 카운트 안 함) */
    if (!s_trend_has_last_value[idx] || value != s_trend_last_value[idx]) {
        trend_push(&s_trend[idx], now_ms, value);
        s_trend_last_value[idx] = value;
        s_trend_has_last_value[idx] = true;
    }

    float predicted = value;
    if (cfg->trend_enable) {
        uint32_t span_ms = 0;
        float slope_per_ms = trend_slope_per_ms(&s_trend[idx], now_ms, (int)cfg->trend_sample_count, &span_ms);
        /* 2026-09-16 원칙 유지("본 만큼만 내다본다") — 이제 그 "본 만큼"은 실제 관측된
         * 시간폭(span_ms)으로 적응적으로 정해짐(고정 초 값 아님) */
        predicted = value + slope_per_ms * (float)span_ms;
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

    /* 2026-09-18(Manual Override, 사용자 설계) — 자동판정(want_on)은 Override 중에도 그대로
     * 계속 계산해서 추세 이력을 살아있게 두되("Override 해제 순간 이미 쌓인 추세로 바로
     * 판단"), 실제 명령은 Override 중이면 그 방향으로 덮어씀 */
    bool effective_want_on = cfg->manual_override ? cfg->manual_override_on : want_on;

    if (effective_want_on == currently_on) return;

    /* 부팅 후 최초 On 전환은 원인(자동규칙/Override) 무관하게 이 유예시간만큼 지연 —
     * "설정에 의해 온 되는 시간이 필요해"(사용자 지시). Off로의 전환은 제약 없음 */
    if (effective_want_on && !s_ever_on_since_boot[idx] &&
        (now_ms - s_boot_ms_ref) < POWER_RELAY_BOOT_ON_DELAY_MS) {
        return;
    }

    /* 채터링 방지(최소유지시간)는 자동판정 전환에만 적용 — Override는 사용자의 직접 명령이라
     * 최소유지시간 대상이 아님(즉시 반영) */
    if (!cfg->manual_override) {
        if (s_last_transition_ms[idx] != 0 &&
            (now_ms - s_last_transition_ms[idx]) < (uint32_t)cfg->min_hold_sec * 1000) {
            return;
        }
    }
    power_relay_command(idx, effective_want_on, now_ms);
    if (effective_want_on) s_ever_on_since_boot[idx] = true;
}

static TaskHandle_t s_relay_task = NULL;
void power_relay_kick(void) { if (s_relay_task) xTaskNotifyGive(s_relay_task); }   /* 설정 바뀜 — 바로 다시 판정 */

static void power_relay_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        /* 2026-10-18 — 원격릴레이는 보릴 상태를 받아들인 뒤부터 판정(콘 부팅 직후 보일러를 꺼 버리지 않게) */
        /* 2026-11-18 — 원격 자리: 새 기기가 끊긴 주인의 자리를 쓰면 설정 초기화, 연결·끊기는 여기서 저장(통신 태스크 스택이 작음) */
        for (int k = 0; k < POWER_RELAY_REMOTE_MAX; k++) {
            if (!s_remote_reset_req[k]) continue;
            s_remote_reset_req[k] = false;
            int r = POWER_RELAY_REMOTE_FIRST + k;
            relay_defaults(r);
            timer_defaults(&s_timer[r]);
            relay_alias_disp_refresh(r);
            memset(&s_trend[r], 0, sizeof(s_trend[r]));
            s_trend_has_last_value[r] = false;
            s_trend_refill[r] = true;
            s_last_transition_ms[r] = 0;
            power_relay_save();
            timer_save();
        }
        for (int k = 0; k < POWER_RELAY_REMOTE_MAX; k++) {
            if (!s_remote_unforce_req[k]) continue;
            s_remote_unforce_req[k] = false;
            int r = POWER_RELAY_REMOTE_FIRST + k;
            if (!s_relay_cfg[r].manual_override && !s_timer[r].manual_until) continue;
            s_relay_cfg[r].manual_override = false;
            s_timer[r].manual_until = 0;
            power_relay_save();
            timer_save();
            ESP_LOGI(TAG, "Relay %d manual override cleared (disconnected by user)", r);
        }
        if (s_rs_dirty) remote_save();
        remote_sync(now_ms);
        for (int i = 0; i < POWER_RELAY_COUNT; i++) {
            /* 원격: 연결된 자리만, 그 기기 상태를 받아들인 뒤부터 */
            if (power_relay_is_remote(i) && (!power_relay_is_shown(i) || !s_remote_adopted[i - POWER_RELAY_REMOTE_FIRST])) continue;
            evaluate_relay(i, now_ms);
        }
        remote_sync(now_ms);
        /* 2026-10-09 — 타이머는 초 단위라 15초 주기 대신 다음 전환 시각에 맞춰 깸(+0.2초 여유). 설정이 바뀌면
         * power_relay_kick()이 바로 깨움 */
        uint32_t wait_ms = POWER_RELAY_EVAL_INTERVAL_MS;
        for (int i = 0; i < POWER_RELAY_COUNT; i++) {
            if (!s_relay_cfg[i].active || !s_timer[i].timer_mode || s_relay_cfg[i].manual_override) continue;
            uint32_t next = UINT32_MAX;
            power_relay_timer_state(&s_timer[i], &next);
            if (next != UINT32_MAX && next * 1000u + 200u < wait_ms) wait_ms = next * 1000u + 200u;
        }
        for (int r = POWER_RELAY_REMOTE_FIRST; r < POWER_RELAY_COUNT; r++) {
            power_remote_status_t rs;
            power_relay_get_remote_status(r, &rs);
            if (rs.state == POWER_REMOTE_PENDING && wait_ms > 5200) wait_ms = 5200;   /* 확인 중 — 5초 뒤 다시 보냄 */
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
    }
}

void power_relay_start(void)
{
    /* 2026-09-18(Manual Override 부팅유예 기준시각) */
    s_boot_ms_ref = (uint32_t)(esp_timer_get_time() / 1000);
    /* 2026-09-16(사용자 지적 — "메모리 35K까지 줄었어, 위험해") — 태스크 생성이 실제로
     * 내부RAM을 얼마나 쓰는지 추측 대신 실측(기존 MEMDIAG 관례와 동일) */
    size_t before = MEMDIAG_HEAP();
    /* 부팅 시 두 출력 모두 꺼짐으로 확정(CH422G 기본도 꺼짐 — 2026-10-02 실측: SSR LED 꺼짐) */
    for (int i = 0; i < POWER_RELAY_LOCAL_COUNT; i++) relay_set_output(i, false);
    /* 2026-09-09 CNTL 태스크 우선순위 체계(project_cntl_task_priority_scheme) — 통신17/
     * SR제어15/파일처리10 중 SR제어 자리를 여기서 처음 실사용.
     * 2026-09-16(실기 크래시 조사) — evaluate_relay()가 ui_main_query_power_source_value()를
     * 거쳐 node_hub_get_nodes(node_hub_node_t[NODE_HUB_MAX_NODES] 지역배열, 노드
     * 구조체 자체가 꽤 큼)까지 내려가는 깊은 호출체인 — main.c의 httpd 핸들러가 똑같은
     * 호출(node_hub_get_nodes)을 스택에서 하려고 기본 스택이 빠듯해서 8192로 올렸던
     * 전례(web_dashboard_start() 주석 참고)와 동일 소지가 있어 8192로 잡음.
     * 2026-09-16(사용자 지적 — "메모리 35K까지 줄었어, 위험해") — plain xTaskCreate()는
     * 스택을 내부RAM에서 할당함(esp_lv_adapter의 stack_in_psram=true와 같은 이유로 위험).
     * 이 프로젝트는 SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY+FREERTOS_TASK_CREATE_ALLOW_EXT_MEM이
     * 이미 켜져있어(sdkconfig 확인) xTaskCreateStatic()에 PSRAM 버퍼를 직접 주면 스택
     * 자체를 PSRAM에 둘 수 있음 — TCB(제어블록)만 내부RAM(작고 고정폭이라 무시 가능)
     * 2026-09-25(사용자 설계 — 통신/UI 코어 분리) — UI와 무관하므로 CAN과 같은 코어 1에 고정.
     * 코어 1 안에서 CAN(17)보다 낮은 15 유지 */
    static StaticTask_t s_power_relay_tcb;
    StackType_t *stack_buf = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (stack_buf) {
        s_relay_task = xTaskCreateStaticPinnedToCore(power_relay_task, "power_relay", 8192, NULL, 15, stack_buf, &s_power_relay_tcb, 1);
    } else {
        ESP_LOGE(TAG, "SR task stack PSRAM alloc failed - falling back to internal RAM");
        xTaskCreatePinnedToCore(power_relay_task, "power_relay", 8192, NULL, 15, &s_relay_task, 1);
    }
    size_t after = MEMDIAG_HEAP();
    ESP_LOGD(TAG, "MEMDIAG power_relay_start cost: internal %u -> %u (used %d bytes)",
             (unsigned)before, (unsigned)after, (int)before - (int)after);
}
