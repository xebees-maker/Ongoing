/**
 * @file    bsp_c6.c
 * @brief   Waveshare ESP32-C6-Touch-LCD-1.47 BSP 구현
 */

#include "esp_timer.h"
#include "bsp_c6.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_lcd_jd9853.h"
#include "esp_lcd_touch_axs5106.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "bsp_c6";

/* ── 내부 핸들 ── */
static lv_display_t          *s_lvgl_display = NULL;
static esp_lcd_touch_handle_t s_touch_handle = NULL;
static SemaphoreHandle_t      s_lvgl_mutex   = NULL;
static TaskHandle_t           s_lvgl_task    = NULL;
static bsp_touch_hook_fn_t    s_touch_hook   = NULL;
static i2c_master_bus_handle_t s_i2c_bus     = NULL;

void bsp_indev_set_hook(bsp_touch_hook_fn_t fn)
{
    s_touch_hook = fn;
}

/* ════════════════════════════════════════════════════════════
 * 내부 함수 선언
 * ════════════════════════════════════════════════════════════ */
static esp_err_t bsp_i2c_init(i2c_master_bus_handle_t *out_bus);
static esp_err_t bsp_lcd_init(esp_lcd_panel_io_handle_t *out_io,
                               esp_lcd_panel_handle_t   *out_panel);
static esp_err_t bsp_backlight_init(void);
static esp_err_t bsp_touch_init(i2c_master_bus_handle_t bus);
static esp_err_t bsp_lvgl_init(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_handle_t   panel);
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
    esp_err_t ret = ESP_OK;
    i2c_master_bus_handle_t  i2c_bus    = NULL;
    esp_lcd_panel_io_handle_t lcd_io    = NULL;
    esp_lcd_panel_handle_t    lcd_panel = NULL;

    ESP_RETURN_ON_ERROR(bsp_i2c_init(&i2c_bus),           TAG, "I2C init failed");
    s_i2c_bus = i2c_bus;
    ESP_RETURN_ON_ERROR(bsp_lcd_init(&lcd_io, &lcd_panel), TAG, "LCD init failed");
    ESP_RETURN_ON_ERROR(bsp_backlight_init(),              TAG, "Backlight init failed");
    ESP_RETURN_ON_ERROR(bsp_touch_init(i2c_bus),           TAG, "Touch init failed");
    ESP_RETURN_ON_ERROR(bsp_lvgl_init(lcd_io, lcd_panel),  TAG, "LVGL init failed");
    ESP_RETURN_ON_ERROR(bsp_display_set_brightness(BSP_LCD_BRIGHTNESS_DEFAULT),
                                                           TAG, "Backlight on failed");

    ESP_LOGI(TAG, "Board init complete");
    return ret;
}

esp_err_t bsp_display_set_brightness(uint8_t brightness)
{
    if (brightness > 100) {
        brightness = 100;
    }
    uint32_t duty = ((uint32_t)brightness * (BSP_LCD_LEDC_DUTY_MAX - 1)) / 100;
    ESP_RETURN_ON_ERROR(
        ledc_set_duty(BSP_LCD_LEDC_MODE, BSP_LCD_LEDC_CHANNEL, duty),
        TAG, "ledc_set_duty failed");
    ESP_RETURN_ON_ERROR(
        ledc_update_duty(BSP_LCD_LEDC_MODE, BSP_LCD_LEDC_CHANNEL),
        TAG, "ledc_update_duty failed");
    return ESP_OK;
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

/* ════════════════════════════════════════════════════════════
 * 내부 함수 구현
 * ════════════════════════════════════════════════════════════ */

static esp_err_t bsp_i2c_init(i2c_master_bus_handle_t *out_bus)
{
    i2c_master_bus_config_t cfg = {
        .clk_source            = I2C_CLK_SRC_DEFAULT,
        .i2c_port              = BSP_TOUCH_I2C_PORT,
        .scl_io_num            = BSP_TOUCH_I2C_SCL,
        .sda_io_num            = BSP_TOUCH_I2C_SDA,
        .glitch_ignore_cnt     = BSP_TOUCH_I2C_GLITCH_CNT,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(
        i2c_new_master_bus(&cfg, out_bus),
        TAG, "i2c_new_master_bus failed");
    ESP_LOGI(TAG, "[INIT] I2C OK  SDA=%d SCL=%d", BSP_TOUCH_I2C_SDA, BSP_TOUCH_I2C_SCL);
    return ESP_OK;
}

static esp_err_t bsp_lcd_init(esp_lcd_panel_io_handle_t *out_io,
                               esp_lcd_panel_handle_t   *out_panel)
{
    /* SPI 버스 (MISO 미사용) */
    spi_bus_config_t bus_cfg = JD9853_PANEL_BUS_SPI_CONFIG(
        BSP_LCD_SPI_SCK,
        BSP_LCD_SPI_MOSI,
        BSP_LCD_H_RES * BSP_LCD_V_RES * sizeof(uint16_t)
    );
    ESP_RETURN_ON_ERROR(
        spi_bus_initialize(BSP_LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
        TAG, "spi_bus_initialize failed");

    /* 패널 IO */
    esp_lcd_panel_io_spi_config_t io_cfg =
        JD9853_PANEL_IO_SPI_CONFIG(BSP_LCD_CS, BSP_LCD_DC, NULL, NULL);
    io_cfg.pclk_hz = BSP_LCD_PIXEL_CLK_HZ;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_HOST,
                                  &io_cfg, out_io),
        TAG, "esp_lcd_new_panel_io_spi failed");

    /* 패널 디바이스 */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BSP_LCD_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_jd9853(*out_io, &panel_cfg, out_panel),
        TAG, "esp_lcd_new_panel_jd9853 failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(*out_panel),  TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(*out_panel),   TAG, "panel init failed");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_set_gap(*out_panel, BSP_LCD_COL_OFFSET, BSP_LCD_ROW_OFFSET),
        TAG, "panel set_gap failed");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_invert_color(*out_panel, true),
        TAG, "panel invert_color failed");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_mirror(*out_panel, false, false),
        TAG, "panel mirror failed");
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_disp_on_off(*out_panel, true),
        TAG, "panel disp_on_off failed");

    ESP_LOGI(TAG, "[INIT] LCD OK  %dx%d @ %luMHz",
             BSP_LCD_H_RES, BSP_LCD_V_RES,
             (unsigned long)(BSP_LCD_PIXEL_CLK_HZ / 1000000));
    return ESP_OK;
}

static esp_err_t bsp_backlight_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = BSP_LCD_LEDC_MODE,
        .timer_num       = BSP_LCD_LEDC_TIMER,
        .duty_resolution = BSP_LCD_LEDC_DUTY_RES,
        .freq_hz         = BSP_LCD_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_cfg), TAG, "ledc_timer_config failed");

    ledc_channel_config_t ch_cfg = {
        .speed_mode = BSP_LCD_LEDC_MODE,
        .channel    = BSP_LCD_LEDC_CHANNEL,
        .timer_sel  = BSP_LCD_LEDC_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = BSP_LCD_BL,
        .duty       = BSP_LCD_BRIGHTNESS_OFF,
        .hpoint     = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch_cfg), TAG, "ledc_channel_config failed");

    ESP_LOGI(TAG, "[INIT] Backlight PWM OK  GPIO=%d", BSP_LCD_BL);
    return ESP_OK;
}

static esp_err_t bsp_touch_init(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BSP_TOUCH_I2C_ADDR,
        .scl_speed_hz    = BSP_TOUCH_I2C_CLK_HZ,
    };
    i2c_master_dev_handle_t dev_handle;
    ESP_RETURN_ON_ERROR(
        i2c_master_bus_add_device(bus, &dev_cfg, &dev_handle),
        TAG, "i2c_master_bus_add_device failed");

    esp_lcd_touch_config_t tp_cfg = {
        .x_max        = BSP_LCD_H_RES,
        .y_max        = BSP_LCD_V_RES,
        .rst_gpio_num = BSP_TOUCH_RST,
        .int_gpio_num = BSP_TOUCH_INT,
        .levels = {
            .reset     = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy  = 0,
            .mirror_x = 1,
            .mirror_y = 0,
        },
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_touch_new_i2c_axs5106(dev_handle, &tp_cfg, &s_touch_handle),
        TAG, "esp_lcd_touch_new_i2c_axs5106 failed");

    ESP_LOGI(TAG, "[INIT] Touch OK  addr=0x%02X INT=%d RST=%d",
             BSP_TOUCH_I2C_ADDR, BSP_TOUCH_INT, BSP_TOUCH_RST);
    return ESP_OK;
}

/* ── LVGL 플러시 콜백 ── */
static void bsp_lvgl_flush_cb(lv_display_t *disp,
                               const lv_area_t *area,
                               uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel =
        (esp_lcd_panel_handle_t)lv_display_get_user_data(disp);

    lv_draw_sw_rgb565_swap(px_map, lv_area_get_size(area));

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

static esp_err_t bsp_lvgl_init(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_handle_t   panel)
{
    s_lvgl_mutex = xSemaphoreCreateRecursiveMutex();
    ESP_RETURN_ON_FALSE(s_lvgl_mutex, ESP_ERR_NO_MEM, TAG, "mutex create failed");

    lv_init();

    size_t buf_size = BSP_LCD_H_RES * BSP_LVGL_BUF_LINES * sizeof(lv_color_t);
    lv_color_t *buf1 = heap_caps_malloc(buf_size, MALLOC_CAP_DMA);
    lv_color_t *buf2 = heap_caps_malloc(buf_size, MALLOC_CAP_DMA);
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
