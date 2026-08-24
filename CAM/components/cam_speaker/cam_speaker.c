#include "cam_speaker.h"

#include <math.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2s_std.h"
#include "es8311.h"

#include "bsp_esp32s3_cam.h"
#include "io_expander_ch32v003.h"

/* 스키매틱 핀아웃표(2026-08-23 확인) — I2S_MCLK=IO10 SCLK=IO11 LRCK=IO12 DSDIN=IO14,
 * PA_CTRL=EXIO4(IO 익스팬더), CODEC_SCL/SDA=IO7/IO8(공유 I2C 버스) */
#define SPK_I2S_MCLK_PIN  GPIO_NUM_10
#define SPK_I2S_BCLK_PIN  GPIO_NUM_11
#define SPK_I2S_WS_PIN    GPIO_NUM_12
#define SPK_I2S_DOUT_PIN  GPIO_NUM_14
#define SPK_PA_CTRL_PIN   CH32V003_IO_4

#define SPK_SAMPLE_RATE   16000

static const char *TAG = "cam_speaker";
static i2s_chan_handle_t s_tx_chan = NULL;
static es8311_handle_t   s_codec   = NULL;
static QueueHandle_t     s_evt_queue = NULL;
/* 2026-08-23 — RTC_DATA_ATTR: 딥슬립은 매 사이클 완전 재부팅이라 일반 RAM 변수는 초기화됨.
 * 콘솔로 켠 상태가 딥슬립 사이클 사이에도 유지되려면 RTC 메모리에 둬야 함(s_last_synced_channel
 * 과 같은 패턴, esp_now_channelsync.c 참고) — 완전 전원차단(POWERON)이나 RWDT의 RESET_RTC가
 * 아닌 한 유지됨 */
/* 2026-08-24(사용자 지시) — "내가 특별히 다시 지정하기 전까지는, 캠이 리붓하더라도 솔3,4만
 * 나도록 항상 설정해." RTC_DATA_ATTR 초기값은 딥슬립 재부팅 때는 적용 안 되고(메모리가 그대로
 * 유지됨), 완전 전원차단이나 재플래시(esptool RTS 리셋도 여기 포함됨을 실기로 확인)처럼 RTC가
 * 진짜로 초기화되는 경우에만 적용됨 — 그래서 이 기본값이 "리붓해도 항상 솔3,4"라는 요구를
 * 정확히 구현함(콘솔로 다시 켜거나 solo를 바꾸면 그 값이 다음 리셋 전까지 유지) */
static RTC_DATA_ATTR volatile bool     s_enabled   = true;    /* 기본 켜짐(2026-08-24 변경) */
static RTC_DATA_ATTR volatile uint32_t s_solo_mask = 0x0C;    /* 기본 solo 3,4(SCAN_CHANNEL+SWEEP_DONE) — bit(N-1), N=3,4 -> 0x04|0x08 */
static volatile bool s_playing = false;  /* speaker_task가 play_event() 실행 중인 동안만 true */

static void play_tone_blocking(float freq_hz, uint32_t duration_ms)
{
    if (!s_tx_chan) {
        return;
    }

    const int total_samples = (SPK_SAMPLE_RATE * (int)duration_ms) / 1000;
    const double phase_inc = 2.0 * M_PI * freq_hz / SPK_SAMPLE_RATE;
    double phase = 0.0;

    int16_t buf[256 * 2]; /* stereo interleaved */
    int samples_done = 0;

    while (samples_done < total_samples) {
        int chunk = total_samples - samples_done;
        if (chunk > 256) {
            chunk = 256;
        }
        for (int i = 0; i < chunk; i++) {
            int16_t s = (int16_t)(sin(phase) * 12000.0);
            buf[2 * i]     = s;
            buf[2 * i + 1] = s;
            phase += phase_inc;
            if (phase > 2.0 * M_PI) {
                phase -= 2.0 * M_PI;
            }
        }
        size_t written = 0;
        i2s_channel_write(s_tx_chan, buf, (size_t)chunk * 2 * sizeof(int16_t), &written, portMAX_DELAY);
        samples_done += chunk;
    }
}

/* 2026-08-23 — 사용자 지정 매핑(도미솔시 — 도=C4 261.63Hz, 미=E4 329.63Hz, 솔=G4 392.00Hz,
 * 시=B4 493.88Hz) */
#define NOTE_DO  261.63f
#define NOTE_MI  329.63f
#define NOTE_SOL 392.00f
#define NOTE_SI  493.88f

/* 2026-08-23 — 실제로 재생된 SCAN_CHANNEL 횟수 카운트(사용자 지시: "사운드 재생할 때마다
 * 카운트 증가시키고, 스윕완료에서 전체 카운트 시리얼 로그 찍고 리셋") — 귀로 세는 것보다
 * 정확하게 "한 랩에 채널스캔이 몇 번 재생됐는지" 확인하기 위함 */
static uint32_t s_scan_channel_play_count = 0;

static void play_event(cam_speaker_event_t event)
{
    switch (event) {
        case SPK_EVT_POWERON_BATTERY:      /* 1. 도 1초 */
            play_tone_blocking(NOTE_DO, 1000);
            break;
        case SPK_EVT_POWERON_USB:          /* 2. 솔 1초 */
            play_tone_blocking(NOTE_SOL, 1000);
            break;
        case SPK_EVT_SCAN_CHANNEL:         /* 3. 미 0.1초 */
            s_scan_channel_play_count++;
            play_tone_blocking(NOTE_MI, 100);
            break;
        case SPK_EVT_SWEEP_DONE:           /* 4. 미 0.5초 */
            ESP_LOGI(TAG, "스윕완료 — 이번 랩 SCAN_CHANNEL 재생 횟수: %u", (unsigned)s_scan_channel_play_count);
            s_scan_channel_play_count = 0;
            play_tone_blocking(NOTE_MI, 500);
            break;
        case SPK_EVT_ADVERTISE_SENT:       /* 5. 솔 0.1초 */
            play_tone_blocking(NOTE_SOL, 100);
            break;
        case SPK_EVT_PING:                 /* 6. 시 0.1초 */
            play_tone_blocking(NOTE_SI, 100);
            break;
        case SPK_EVT_PONG:                 /* 7. 도 0.1초 */
            play_tone_blocking(NOTE_DO, 100);
            break;
        case SPK_EVT_COUNT:
            break;
    }
}

static void speaker_task(void *arg)
{
    (void)arg;
    cam_speaker_event_t evt;
    for (;;) {
        if (xQueueReceive(s_evt_queue, &evt, portMAX_DELAY) == pdTRUE) {
            s_playing = true;
            play_event(evt);
            s_playing = false;
        }
    }
}

void cam_speaker_notify(cam_speaker_event_t event)
{
    if (!s_evt_queue || !s_enabled) {
        return;
    }
    if (s_solo_mask != 0 && !(s_solo_mask & (1u << (uint32_t)event))) {
        return;  /* solo 중인데 이 이벤트 비트가 꺼져있으면 무음 */
    }
    xQueueSend(s_evt_queue, &event, 0); /* 논블로킹 — 가득 차면 조용히 버림 */
}

void cam_speaker_set_enabled(bool enabled)
{
    s_enabled = enabled;
    ESP_LOGI(TAG, "소리 로그 %s", enabled ? "켜짐" : "꺼짐");
}

void cam_speaker_set_solo_mask(uint32_t mask)
{
    s_solo_mask = mask;
    if (mask == 0) {
        ESP_LOGI(TAG, "소리 solo 해제 — 전체 재생");
    } else {
        ESP_LOGI(TAG, "소리 solo 마스크: 0x%02x", (unsigned)mask);
    }
}

void cam_speaker_wait_idle(uint32_t timeout_ms)
{
    if (!s_enabled || !s_evt_queue) {
        return;
    }
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    while (s_playing || uxQueueMessagesWaiting(s_evt_queue) > 0) {
        if ((xTaskGetTickCount() - start) >= timeout_ticks) {
            ESP_LOGW(TAG, "재생 대기 타임아웃(%ums) — 남은 소리 못 다 듣고 잠듦", (unsigned)timeout_ms);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t cam_speaker_init(void)
{
    ch32v003_set_output(SPK_PA_CTRL_PIN, true);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    /* 2026-08-24(사용자 지시) — ESP-IDF I2S 드라이버 소스(i2s_common.c) 확인: 이게 꺼져있으면
     * (기본값) 전송 콜백 후 DMA 버퍼를 안 지워서, 다음 쓰기가 늦으면(또는 없으면) 마지막
     * 오디오를 계속 반복 재생함. 켜면 매 콜백 후 자동으로 무음(0)으로 memset — 톤이 끝나도
     * 계속 반복 재생되던 문제의 근본 원인/해결책(실기로 확인됨) */
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL), TAG, "i2s_new_channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SPK_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = SPK_I2S_MCLK_PIN,
            .bclk = SPK_I2S_BCLK_PIN,
            .ws   = SPK_I2S_WS_PIN,
            .dout = SPK_I2S_DOUT_PIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_chan, &std_cfg), TAG, "i2s_channel_init_std_mode");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_chan), TAG, "i2s_channel_enable");

    s_codec = es8311_create(bsp_esp32s3_cam_get_i2c_bus(), ES8311_ADDRRES_0);
    ESP_RETURN_ON_FALSE(s_codec != NULL, ESP_FAIL, TAG, "es8311_create failed");

    const es8311_clock_config_t clk_cfg = {
        .mclk_inverted      = false,
        .sclk_inverted      = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency     = SPK_SAMPLE_RATE * 256,
        .sample_frequency   = SPK_SAMPLE_RATE,
    };
    ESP_RETURN_ON_ERROR(es8311_init(s_codec, &clk_cfg, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16), TAG, "es8311_init");
    ESP_RETURN_ON_ERROR(es8311_voice_volume_set(s_codec, 70, NULL), TAG, "volume");
    ESP_RETURN_ON_ERROR(es8311_voice_mute(s_codec, false), TAG, "unmute");

    s_evt_queue = xQueueCreate(16, sizeof(cam_speaker_event_t));
    ESP_RETURN_ON_FALSE(s_evt_queue != NULL, ESP_ERR_NO_MEM, TAG, "xQueueCreate failed");
    xTaskCreate(speaker_task, "cam_speaker", 3072, NULL, 5, NULL);

    ESP_LOGI(TAG, "스피커 초기화 완료");
    return ESP_OK;
}
