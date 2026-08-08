/**
 * @file    bsp_ws_4_3b.c
 * @brief   Waveshare ESP32-S3-Touch-LCD-4.3B BSP 구현
 *
 * LVGL 연결은 esp_lvgl_adapter(공식 보조 라이브러리) 대신 직접 짠 flush/touch
 * 콜백을 씀 — 어댑터의 RGB TRIPLE_PARTIAL 경로에서 확인된 버그(esp-iot-solution
 * 저장소 이슈 #627 드로우버퍼 PSRAM 무시, #659 stride 정렬, #719 유사 증상 미해결)를
 * 다 우회해봐도 LVGL이 그린 내용이 실제 화면에 반영이 안 되는 문제가 남아있었고,
 * 반면 esp_lcd_panel_draw_bitmap()으로 직접 그리는 방식은 이번 세션 내내 단색/
 * 부분영역(사분할) 전부 100% 정확하게 동작함이 확인됨 — 그래서 그 검증된 경로를
 * 그대로 LVGL의 flush_cb로 씀. LVGL 자체(위젯/스타일 API)는 그대로 다 쓸 수 있고,
 * 바뀐 건 "그린 픽셀을 화면에 보내는 방법"뿐임.
 */

#include "bsp_ws_4_3b.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"

static const char *TAG = "bsp_ws_4_3b";

/* GT911 INT핀 — CH422G 미경유, ESP32 직결(GPIO4). 리셋 상승엣지 순간의 레벨로
 * I2C 주소가 0x5D/0x14로 갈리는 GT911 특성상 반드시 로우로 잡아줘야 함(Waveshare 공식
 * 데모 waveshare_esp32_s3_touch_reset()의 GPIO_INPUT_IO_4와 동일 배선/타이밍) */
#define BSP_TOUCH_ADDR_SEL_GPIO   GPIO_NUM_4

/* ── 내부 핸들 ── */
static lv_display_t           *s_lvgl_display = NULL;
static esp_lcd_touch_handle_t  s_touch_handle = NULL;
static bsp_touch_hook_fn_t     s_touch_hook   = NULL;
static i2c_master_bus_handle_t s_i2c_bus      = NULL;
static esp_lcd_panel_handle_t  s_lcd_panel    = NULL;
static SemaphoreHandle_t       s_lvgl_mutex   = NULL;
static TaskHandle_t            s_lvgl_task    = NULL;

void bsp_indev_set_hook(bsp_touch_hook_fn_t fn)
{
    s_touch_hook = fn;
}

/* ════════════════════════════════════════════════════════════
 * 내부 함수 선언
 * ════════════════════════════════════════════════════════════ */
static esp_err_t bsp_i2c_init(i2c_master_bus_handle_t *out_bus);
static esp_err_t bsp_lcd_init(esp_lcd_panel_handle_t *out_panel);
static esp_err_t bsp_touch_init(i2c_master_bus_handle_t bus);
static esp_err_t bsp_lvgl_init(esp_lcd_panel_handle_t panel);
static void      bsp_lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area,
                                    uint8_t *px_map);
static void      bsp_lvgl_touch_cb(lv_indev_t *indev, lv_indev_data_t *data);
static void      bsp_lvgl_tick_cb(void *arg);
static void      bsp_lvgl_task(void *arg);

/* ════════════════════════════════════════════════════════════
 * 공개 API 구현
 * ════════════════════════════════════════════════════════════ */

esp_err_t bsp_board_init(void)
{
    i2c_master_bus_handle_t i2c_bus = NULL;

    ESP_RETURN_ON_ERROR(bsp_i2c_init(&i2c_bus), TAG, "I2C init failed");
    s_i2c_bus = i2c_bus;
    ESP_RETURN_ON_ERROR(ch422g_init(i2c_bus), TAG, "CH422G init failed");

    /* LCD 물리 리셋을 esp_lcd_panel_init()보다 먼저 풀어줘야 함 — RGB 패널은
     * 리셋 중에도 ESP32가 계속 타이밍 신호를 내보내는데, 패널 TCON이 리셋 해제
     * 후에도 그 상태를 못 따라잡으면 화면이 안정적으로 안 나올 수 있음. */
    ESP_RETURN_ON_ERROR(ch422g_set_io(CH422G_IO_LCD_RST, false), TAG, "lcd rst low failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(ch422g_set_io(CH422G_IO_LCD_RST, true), TAG, "lcd rst high failed");
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_RETURN_ON_ERROR(bsp_lcd_init(&s_lcd_panel), TAG, "LCD init failed");
    ESP_RETURN_ON_ERROR(bsp_display_set_brightness(1), TAG, "Backlight on failed");
    ESP_RETURN_ON_ERROR(bsp_touch_init(i2c_bus), TAG, "Touch init failed");
    ESP_RETURN_ON_ERROR(ch422g_set_io(CH422G_IO_SD_CS, true), TAG, "SD CS deselect failed");
    ESP_RETURN_ON_ERROR(bsp_lvgl_init(s_lcd_panel), TAG, "LVGL init failed");

    ESP_LOGI(TAG, "Board init complete");
    return ESP_OK;
}

esp_err_t bsp_display_set_brightness(uint8_t brightness)
{
    /* PWM 디밍 하드웨어 없음 — 0=off, 그 외엔 전부 on(project_cntl_4_3b_bsp 메모리 참고,
     * 사용자가 on/off만 지원하기로 확정) */
    return ch422g_set_io(CH422G_IO_BACKLIGHT, brightness != 0);
}

lv_display_t *bsp_get_lvgl_display(void)
{
    return s_lvgl_display;
}

i2c_master_bus_handle_t bsp_get_i2c_bus(void)
{
    return s_i2c_bus;
}

bool bsp_lvgl_lock(uint32_t timeout_ms)
{
    assert(s_lvgl_mutex != NULL);
    TickType_t ticks = (timeout_ms == BSP_MUTEX_WAIT_FOREVER)
                       ? portMAX_DELAY
                       : pdMS_TO_TICKS(timeout_ms);
    return (xSemaphoreTakeRecursive(s_lvgl_mutex, ticks) == pdTRUE);
}

void bsp_lvgl_unlock(void)
{
    assert(s_lvgl_mutex != NULL);
    xSemaphoreGiveRecursive(s_lvgl_mutex);
}

esp_err_t bsp_ssr_set(uint8_t channel, bool on)
{
    uint8_t bit = (channel == 0) ? CH422G_OD_DO0 : CH422G_OD_DO1;
    return ch422g_set_do(bit, on);
}

esp_err_t bsp_isolated_input_read(bool *out_di0, bool *out_di1)
{
    return ch422g_read_di(out_di0, out_di1);
}

/* ════════════════════════════════════════════════════════════
 * 내부 함수 구현
 * ════════════════════════════════════════════════════════════ */

/* ESP32만 리셋되는 소프트 리셋(esptool 플래시 후 RTS리셋, 코드 크래시 등) 중간에
 * CH422G와의 I2C 트랜잭션이 끊기면, CH422G가 SDA를 계속 로우로 물고 있는 채로 남을
 * 수 있음(CH422G는 ESP32와 별도 전원이라 전원 자체를 안 뽑으면 안 풀림). i2c_new_master_bus()
 * 전에 SCL을 9번 토글해서 걸린 슬레이브를 풀어주는 표준 I2C 버스 복구 절차. */
static void bsp_i2c_bus_recover(void)
{
    gpio_config_t scl_conf = {
        .pin_bit_mask = (1ULL << BSP_I2C_SCL),
        .mode         = GPIO_MODE_OUTPUT_OD,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&scl_conf);

    gpio_config_t sda_conf = {
        .pin_bit_mask = (1ULL << BSP_I2C_SDA),
        .mode         = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&sda_conf);

    gpio_set_level(BSP_I2C_SDA, 1);
    gpio_set_level(BSP_I2C_SCL, 1);
    esp_rom_delay_us(5);

    for (int i = 0; i < 9; i++) {
        if (gpio_get_level(BSP_I2C_SDA)) {
            break;  /* 슬레이브가 이미 SDA를 놓음 */
        }
        gpio_set_level(BSP_I2C_SCL, 0);
        esp_rom_delay_us(5);
        gpio_set_level(BSP_I2C_SCL, 1);
        esp_rom_delay_us(5);
    }

    /* STOP 조건: SCL 하이 상태에서 SDA 로우->하이 */
    gpio_set_level(BSP_I2C_SDA, 0);
    esp_rom_delay_us(5);
    gpio_set_level(BSP_I2C_SCL, 1);
    esp_rom_delay_us(5);
    gpio_set_level(BSP_I2C_SDA, 1);
    esp_rom_delay_us(5);

    /* i2c_new_master_bus()가 이 두 핀을 다시 제대로 설정할 수 있게 GPIO 핀 설정을
     * 완전히 초기화 상태로 되돌림 — 여기서 만든 OUTPUT_OD 설정이 남아있으면 이후
     * I2C 페리페럴의 핀먹스 설정과 충돌할 수 있음 */
    gpio_reset_pin(BSP_I2C_SDA);
    gpio_reset_pin(BSP_I2C_SCL);
}

static esp_err_t bsp_i2c_init(i2c_master_bus_handle_t *out_bus)
{
    bsp_i2c_bus_recover();

    i2c_master_bus_config_t cfg = {
        .clk_source            = I2C_CLK_SRC_DEFAULT,
        .i2c_port              = BSP_I2C_PORT,
        .scl_io_num            = BSP_I2C_SCL,
        .sda_io_num            = BSP_I2C_SDA,
        .glitch_ignore_cnt     = BSP_I2C_GLITCH_CNT,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(
        i2c_new_master_bus(&cfg, out_bus),
        TAG, "i2c_new_master_bus failed");
    ESP_LOGI(TAG, "[INIT] I2C OK  SDA=%d SCL=%d", BSP_I2C_SDA, BSP_I2C_SCL);
    return ESP_OK;
}

static esp_err_t bsp_lcd_init(esp_lcd_panel_handle_t *out_panel)
{
    esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = BSP_LCD_PIXEL_CLK_HZ,
            .h_res   = BSP_LCD_H_RES,
            .v_res   = BSP_LCD_V_RES,
            .hsync_pulse_width  = BSP_LCD_HSYNC_PULSE_WIDTH,
            .hsync_back_porch   = BSP_LCD_HSYNC_BACK_PORCH,
            .hsync_front_porch  = BSP_LCD_HSYNC_FRONT_PORCH,
            .vsync_pulse_width  = BSP_LCD_VSYNC_PULSE_WIDTH,
            .vsync_back_porch   = BSP_LCD_VSYNC_BACK_PORCH,
            .vsync_front_porch  = BSP_LCD_VSYNC_FRONT_PORCH,
            .flags = {
                .pclk_active_neg = 1,
            },
        },
        .data_width       = 16,
        .in_color_format  = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        /* 버퍼 1개 — 직접 draw_bitmap을 쓰는 flush_cb라 "그리는 버퍼=보이는 버퍼"가
         * 항상 성립함. 여러 버퍼를 순환시키는 어댑터식 접근에서 버퍼 추적이 어긋나
         * LVGL이 그린 내용이 화면에 반영 안 되던 문제를 구조적으로 피함. */
        .num_fbs          = 1,
        .bounce_buffer_size_px = BSP_LCD_H_RES * BSP_LCD_BOUNCE_LINES,
        .hsync_gpio_num = BSP_LCD_PIN_HSYNC,
        .vsync_gpio_num = BSP_LCD_PIN_VSYNC,
        .de_gpio_num    = BSP_LCD_PIN_DE,
        .pclk_gpio_num  = BSP_LCD_PIN_PCLK,
        .disp_gpio_num  = -1,
        .data_gpio_nums = {
            BSP_LCD_PIN_DATA0,  BSP_LCD_PIN_DATA1,  BSP_LCD_PIN_DATA2,  BSP_LCD_PIN_DATA3,
            BSP_LCD_PIN_DATA4,  BSP_LCD_PIN_DATA5,  BSP_LCD_PIN_DATA6,  BSP_LCD_PIN_DATA7,
            BSP_LCD_PIN_DATA8,  BSP_LCD_PIN_DATA9,  BSP_LCD_PIN_DATA10, BSP_LCD_PIN_DATA11,
            BSP_LCD_PIN_DATA12, BSP_LCD_PIN_DATA13, BSP_LCD_PIN_DATA14, BSP_LCD_PIN_DATA15,
        },
        .flags = {
            .fb_in_psram = 1,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&panel_config, out_panel), TAG, "esp_lcd_new_rgb_panel failed");
    /* 임시 진단: reset() 뺴고 이전에 실제로 화면이 나왔던 구성과 동일하게 테스트 */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(*out_panel), TAG, "panel init failed");

    ESP_LOGI(TAG, "[INIT] LCD OK  %dx%d @ %luMHz",
             BSP_LCD_H_RES, BSP_LCD_V_RES,
             (unsigned long)(BSP_LCD_PIXEL_CLK_HZ / 1000000));
    return ESP_OK;
}

/* GT911 리셋 — RST는 CH422G IO1 경유, INT(주소선택)는 GPIO4 직결. Waveshare 공식
 * 데모 waveshare_esp32_s3_touch_reset()의 바이트값·순서를 그대로 재현:
 *   Mode=0x01(IO_OE만) -> IO_OUT=0x2C(BACKLIGHT|LCD_RST|DI1, TOUCH_RST=0) 유지 100ms
 *   -> INT(GPIO4) low 진입 100ms 추가 대기 -> IO_OUT=0x2E(위에 TOUCH_RST=1 추가) -> 200ms.
 * ch422g_set_io_raw()로 섀도우 병합 없이 절대값을 직접 씀. INT를 로우로 잡아야
 * 리셋 상승엣지에서 I2C주소 0x5D로 고정됨. */
static void bsp_touch_reset_via_ch422g(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BSP_TOUCH_ADDR_SEL_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_conf);

    ch422g_set_io_raw(CH422G_MODE_IO_OE,
                       CH422G_IO_BACKLIGHT | CH422G_IO_LCD_RST | CH422G_IO_DI1);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(BSP_TOUCH_ADDR_SEL_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    ch422g_set_io_raw(CH422G_MODE_IO_OE,
                       CH422G_IO_TOUCH_RST | CH422G_IO_BACKLIGHT | CH422G_IO_LCD_RST | CH422G_IO_DI1);
    vTaskDelay(pdMS_TO_TICKS(200));
}

static esp_err_t bsp_touch_init(i2c_master_bus_handle_t bus)
{
    bsp_touch_reset_via_ch422g();

    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tp_io_cfg.scl_speed_hz = BSP_I2C_CLK_HZ;
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_i2c(bus, &tp_io_cfg, &tp_io_handle),
        TAG, "esp_lcd_new_panel_io_i2c failed");

    esp_lcd_touch_config_t tp_cfg = {
        .x_max        = BSP_LCD_H_RES,
        .y_max        = BSP_LCD_V_RES,
        .rst_gpio_num = -1,  /* CH422G IO1로 이미 리셋함 */
        .int_gpio_num = -1,  /* GPIO4는 주소선택용으로만 씀 — 인터럽트 모드 대신 폴링 사용 */
        .levels = {
            .reset     = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy  = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, &s_touch_handle),
        TAG, "esp_lcd_touch_new_i2c_gt911 failed");

    ESP_LOGI(TAG, "[INIT] Touch(GT911) OK");
    return ESP_OK;
}

/* ── LVGL 플러시 콜백 — 이번 세션에서 단색/부분영역(사분할) 전부 100% 정확하게
 * 동작함이 확인된 방식. RGB 패널은 SPI와 달리 16비트 워드를 병렬로 그대로
 * 내보내므로 SPI용 BSP처럼 lv_draw_sw_rgb565_swap()이 필요 없음. */
static void bsp_lvgl_flush_cb(lv_display_t *disp,
                               const lv_area_t *area,
                               uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel =
        (esp_lcd_panel_handle_t)lv_display_get_user_data(disp);

    esp_lcd_panel_draw_bitmap(panel,
                              area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1,
                              px_map);
    lv_display_flush_ready(disp);
}

/* ── LVGL 터치 입력 콜백 ── */
static void bsp_lvgl_touch_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint16_t touch_x, touch_y, touch_strength;
    uint8_t  touch_cnt = 0;

    esp_lcd_touch_read_data(s_touch_handle);
    bool touched = esp_lcd_touch_get_coordinates(
        s_touch_handle, &touch_x, &touch_y, &touch_strength,
        &touch_cnt, BSP_TOUCH_MAX_POINTS);

    if (touched && touch_cnt > 0) {
        data->point.x = touch_x;
        data->point.y = touch_y;
        data->state   = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }

    if (s_touch_hook && s_touch_hook(data->state == LV_INDEV_STATE_PRESSED, data->point)) {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/* ── LVGL tick 타이머 콜백 ── */
static void bsp_lvgl_tick_cb(void *arg)
{
    lv_tick_inc(BSP_LVGL_TICK_MS);
}

/* ── LVGL 전용 태스크 ── */
static void bsp_lvgl_task(void *arg)
{
    ESP_LOGI(TAG, "LVGL task started");
    while (true) {
        if (bsp_lvgl_lock(BSP_MUTEX_WAIT_FOREVER)) {
            uint32_t delay_ms = lv_timer_handler();
            bsp_lvgl_unlock();
            vTaskDelay(pdMS_TO_TICKS(delay_ms > 0 ? delay_ms : 1));
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

static esp_err_t bsp_lvgl_init(esp_lcd_panel_handle_t panel)
{
    s_lvgl_mutex = xSemaphoreCreateRecursiveMutex();
    ESP_RETURN_ON_FALSE(s_lvgl_mutex, ESP_ERR_NO_MEM, TAG, "mutex create failed");

    lv_init();

    /* flush_cb은 esp_lcd_panel_draw_bitmap()으로 PSRAM 프레임버퍼에 복사하는
     * 방식이라 이 중간 드로우버퍼 자체는 DMA-capable일 필요가 없음 — PSRAM에 할당 */
    size_t buf_size = BSP_LCD_H_RES * BSP_LVGL_BUF_LINES * sizeof(lv_color_t);
    lv_color_t *buf1 = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    lv_color_t *buf2 = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(buf1 && buf2, ESP_ERR_NO_MEM, TAG, "draw buf alloc failed");

    s_lvgl_display = lv_display_create(BSP_LCD_H_RES, BSP_LCD_V_RES);
    ESP_RETURN_ON_FALSE(s_lvgl_display, ESP_ERR_NO_MEM, TAG, "lv_display_create failed");

    lv_display_set_flush_cb(s_lvgl_display, bsp_lvgl_flush_cb);
    lv_display_set_buffers(s_lvgl_display, buf1, buf2,
                           buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_user_data(s_lvgl_display, panel);

    lv_indev_t *indev = lv_indev_create();
    ESP_RETURN_ON_FALSE(indev, ESP_ERR_NO_MEM, TAG, "lv_indev_create failed");
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, bsp_lvgl_touch_cb);

    const esp_timer_create_args_t tick_args = {
        .callback = bsp_lvgl_tick_cb,
        .name     = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer;
    ESP_RETURN_ON_ERROR(
        esp_timer_create(&tick_args, &tick_timer),
        TAG, "esp_timer_create failed");
    ESP_RETURN_ON_ERROR(
        esp_timer_start_periodic(tick_timer, BSP_LVGL_TICK_MS * 1000),
        TAG, "esp_timer_start_periodic failed");

    BaseType_t res = xTaskCreatePinnedToCore(
        bsp_lvgl_task,
        "lvgl",
        BSP_LVGL_TASK_STACK,
        NULL,
        BSP_LVGL_TASK_PRIORITY,
        &s_lvgl_task,
        BSP_LVGL_TASK_CORE
    );
    ESP_RETURN_ON_FALSE(res == pdPASS, ESP_FAIL, TAG, "LVGL task create failed");

    ESP_LOGI(TAG, "[INIT] LVGL OK");
    return ESP_OK;
}
