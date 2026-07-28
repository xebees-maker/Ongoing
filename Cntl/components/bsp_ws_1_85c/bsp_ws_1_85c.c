/**
 * @file    bsp_ws_1_85c.c
 * @brief   Waveshare ESP32-S3-Touch-LCD-1.85C(V2) BSP 구현
 */

#include "esp_timer.h"
#include "bsp_ws_1_85c.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_lcd_st77916.h"
#include "esp_lcd_touch_cst816.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"

static const char *TAG = "bsp_ws_1_85c";

/* Waveshare 공식 데모(ESP32-S3-Touch-LCD-1.85C) LCD_Driver/ST77916.c의
 * vendor_specific_init_new 그대로 — 패널 레지스터 0x04가 특정 패턴일 때 적용 */
static const st77916_lcd_init_cmd_t bsp_st77916_vendor_specific_init_new[] = {
  {0xF0, (uint8_t []){0x28}, 1, 0},
  {0xF2, (uint8_t []){0x28}, 1, 0},
  {0x73, (uint8_t []){0xF0}, 1, 0},
  {0x7C, (uint8_t []){0xD1}, 1, 0},
  {0x83, (uint8_t []){0xE0}, 1, 0},
  {0x84, (uint8_t []){0x61}, 1, 0},
  {0xF2, (uint8_t []){0x82}, 1, 0},
  {0xF0, (uint8_t []){0x00}, 1, 0},
  {0xF0, (uint8_t []){0x01}, 1, 0},
  {0xF1, (uint8_t []){0x01}, 1, 0},
  {0xB0, (uint8_t []){0x56}, 1, 0},
  {0xB1, (uint8_t []){0x4D}, 1, 0},
  {0xB2, (uint8_t []){0x24}, 1, 0},
  {0xB4, (uint8_t []){0x87}, 1, 0},
  {0xB5, (uint8_t []){0x44}, 1, 0},
  {0xB6, (uint8_t []){0x8B}, 1, 0},
  {0xB7, (uint8_t []){0x40}, 1, 0},
  {0xB8, (uint8_t []){0x86}, 1, 0},
  {0xBA, (uint8_t []){0x00}, 1, 0},
  {0xBB, (uint8_t []){0x08}, 1, 0},
  {0xBC, (uint8_t []){0x08}, 1, 0},
  {0xBD, (uint8_t []){0x00}, 1, 0},
  {0xC0, (uint8_t []){0x80}, 1, 0},
  {0xC1, (uint8_t []){0x10}, 1, 0},
  {0xC2, (uint8_t []){0x37}, 1, 0},
  {0xC3, (uint8_t []){0x80}, 1, 0},
  {0xC4, (uint8_t []){0x10}, 1, 0},
  {0xC5, (uint8_t []){0x37}, 1, 0},
  {0xC6, (uint8_t []){0xA9}, 1, 0},
  {0xC7, (uint8_t []){0x41}, 1, 0},
  {0xC8, (uint8_t []){0x01}, 1, 0},
  {0xC9, (uint8_t []){0xA9}, 1, 0},
  {0xCA, (uint8_t []){0x41}, 1, 0},
  {0xCB, (uint8_t []){0x01}, 1, 0},
  {0xD0, (uint8_t []){0x91}, 1, 0},
  {0xD1, (uint8_t []){0x68}, 1, 0},
  {0xD2, (uint8_t []){0x68}, 1, 0},
  {0xF5, (uint8_t []){0x00, 0xA5}, 2, 0},
  {0xDD, (uint8_t []){0x4F}, 1, 0},
  {0xDE, (uint8_t []){0x4F}, 1, 0},
  {0xF1, (uint8_t []){0x10}, 1, 0},
  {0xF0, (uint8_t []){0x00}, 1, 0},
  {0xF0, (uint8_t []){0x02}, 1, 0},
  {0xE0, (uint8_t []){0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
  {0xE1, (uint8_t []){0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
  {0xF0, (uint8_t []){0x10}, 1, 0},
  {0xF3, (uint8_t []){0x10}, 1, 0},
  {0xE0, (uint8_t []){0x07}, 1, 0},
  {0xE1, (uint8_t []){0x00}, 1, 0},
  {0xE2, (uint8_t []){0x00}, 1, 0},
  {0xE3, (uint8_t []){0x00}, 1, 0},
  {0xE4, (uint8_t []){0xE0}, 1, 0},
  {0xE5, (uint8_t []){0x06}, 1, 0},
  {0xE6, (uint8_t []){0x21}, 1, 0},
  {0xE7, (uint8_t []){0x01}, 1, 0},
  {0xE8, (uint8_t []){0x05}, 1, 0},
  {0xE9, (uint8_t []){0x02}, 1, 0},
  {0xEA, (uint8_t []){0xDA}, 1, 0},
  {0xEB, (uint8_t []){0x00}, 1, 0},
  {0xEC, (uint8_t []){0x00}, 1, 0},
  {0xED, (uint8_t []){0x0F}, 1, 0},
  {0xEE, (uint8_t []){0x00}, 1, 0},
  {0xEF, (uint8_t []){0x00}, 1, 0},
  {0xF8, (uint8_t []){0x00}, 1, 0},
  {0xF9, (uint8_t []){0x00}, 1, 0},
  {0xFA, (uint8_t []){0x00}, 1, 0},
  {0xFB, (uint8_t []){0x00}, 1, 0},
  {0xFC, (uint8_t []){0x00}, 1, 0},
  {0xFD, (uint8_t []){0x00}, 1, 0},
  {0xFE, (uint8_t []){0x00}, 1, 0},
  {0xFF, (uint8_t []){0x00}, 1, 0},
  {0x60, (uint8_t []){0x40}, 1, 0},
  {0x61, (uint8_t []){0x04}, 1, 0},
  {0x62, (uint8_t []){0x00}, 1, 0},
  {0x63, (uint8_t []){0x42}, 1, 0},
  {0x64, (uint8_t []){0xD9}, 1, 0},
  {0x65, (uint8_t []){0x00}, 1, 0},
  {0x66, (uint8_t []){0x00}, 1, 0},
  {0x67, (uint8_t []){0x00}, 1, 0},
  {0x68, (uint8_t []){0x00}, 1, 0},
  {0x69, (uint8_t []){0x00}, 1, 0},
  {0x6A, (uint8_t []){0x00}, 1, 0},
  {0x6B, (uint8_t []){0x00}, 1, 0},
  {0x70, (uint8_t []){0x40}, 1, 0},
  {0x71, (uint8_t []){0x03}, 1, 0},
  {0x72, (uint8_t []){0x00}, 1, 0},
  {0x73, (uint8_t []){0x42}, 1, 0},
  {0x74, (uint8_t []){0xD8}, 1, 0},
  {0x75, (uint8_t []){0x00}, 1, 0},
  {0x76, (uint8_t []){0x00}, 1, 0},
  {0x77, (uint8_t []){0x00}, 1, 0},
  {0x78, (uint8_t []){0x00}, 1, 0},
  {0x79, (uint8_t []){0x00}, 1, 0},
  {0x7A, (uint8_t []){0x00}, 1, 0},
  {0x7B, (uint8_t []){0x00}, 1, 0},
  {0x80, (uint8_t []){0x48}, 1, 0},
  {0x81, (uint8_t []){0x00}, 1, 0},
  {0x82, (uint8_t []){0x06}, 1, 0},
  {0x83, (uint8_t []){0x02}, 1, 0},
  {0x84, (uint8_t []){0xD6}, 1, 0},
  {0x85, (uint8_t []){0x04}, 1, 0},
  {0x86, (uint8_t []){0x00}, 1, 0},
  {0x87, (uint8_t []){0x00}, 1, 0},
  {0x88, (uint8_t []){0x48}, 1, 0},
  {0x89, (uint8_t []){0x00}, 1, 0},
  {0x8A, (uint8_t []){0x08}, 1, 0},
  {0x8B, (uint8_t []){0x02}, 1, 0},
  {0x8C, (uint8_t []){0xD8}, 1, 0},
  {0x8D, (uint8_t []){0x04}, 1, 0},
  {0x8E, (uint8_t []){0x00}, 1, 0},
  {0x8F, (uint8_t []){0x00}, 1, 0},
  {0x90, (uint8_t []){0x48}, 1, 0},
  {0x91, (uint8_t []){0x00}, 1, 0},
  {0x92, (uint8_t []){0x0A}, 1, 0},
  {0x93, (uint8_t []){0x02}, 1, 0},
  {0x94, (uint8_t []){0xDA}, 1, 0},
  {0x95, (uint8_t []){0x04}, 1, 0},
  {0x96, (uint8_t []){0x00}, 1, 0},
  {0x97, (uint8_t []){0x00}, 1, 0},
  {0x98, (uint8_t []){0x48}, 1, 0},
  {0x99, (uint8_t []){0x00}, 1, 0},
  {0x9A, (uint8_t []){0x0C}, 1, 0},
  {0x9B, (uint8_t []){0x02}, 1, 0},
  {0x9C, (uint8_t []){0xDC}, 1, 0},
  {0x9D, (uint8_t []){0x04}, 1, 0},
  {0x9E, (uint8_t []){0x00}, 1, 0},
  {0x9F, (uint8_t []){0x00}, 1, 0},
  {0xA0, (uint8_t []){0x48}, 1, 0},
  {0xA1, (uint8_t []){0x00}, 1, 0},
  {0xA2, (uint8_t []){0x05}, 1, 0},
  {0xA3, (uint8_t []){0x02}, 1, 0},
  {0xA4, (uint8_t []){0xD5}, 1, 0},
  {0xA5, (uint8_t []){0x04}, 1, 0},
  {0xA6, (uint8_t []){0x00}, 1, 0},
  {0xA7, (uint8_t []){0x00}, 1, 0},
  {0xA8, (uint8_t []){0x48}, 1, 0},
  {0xA9, (uint8_t []){0x00}, 1, 0},
  {0xAA, (uint8_t []){0x07}, 1, 0},
  {0xAB, (uint8_t []){0x02}, 1, 0},
  {0xAC, (uint8_t []){0xD7}, 1, 0},
  {0xAD, (uint8_t []){0x04}, 1, 0},
  {0xAE, (uint8_t []){0x00}, 1, 0},
  {0xAF, (uint8_t []){0x00}, 1, 0},
  {0xB0, (uint8_t []){0x48}, 1, 0},
  {0xB1, (uint8_t []){0x00}, 1, 0},
  {0xB2, (uint8_t []){0x09}, 1, 0},
  {0xB3, (uint8_t []){0x02}, 1, 0},
  {0xB4, (uint8_t []){0xD9}, 1, 0},
  {0xB5, (uint8_t []){0x04}, 1, 0},
  {0xB6, (uint8_t []){0x00}, 1, 0},
  {0xB7, (uint8_t []){0x00}, 1, 0},
  {0xB8, (uint8_t []){0x48}, 1, 0},
  {0xB9, (uint8_t []){0x00}, 1, 0},
  {0xBA, (uint8_t []){0x0B}, 1, 0},
  {0xBB, (uint8_t []){0x02}, 1, 0},
  {0xBC, (uint8_t []){0xDB}, 1, 0},
  {0xBD, (uint8_t []){0x04}, 1, 0},
  {0xBE, (uint8_t []){0x00}, 1, 0},
  {0xBF, (uint8_t []){0x00}, 1, 0},
  {0xC0, (uint8_t []){0x10}, 1, 0},
  {0xC1, (uint8_t []){0x47}, 1, 0},
  {0xC2, (uint8_t []){0x56}, 1, 0},
  {0xC3, (uint8_t []){0x65}, 1, 0},
  {0xC4, (uint8_t []){0x74}, 1, 0},
  {0xC5, (uint8_t []){0x88}, 1, 0},
  {0xC6, (uint8_t []){0x99}, 1, 0},
  {0xC7, (uint8_t []){0x01}, 1, 0},
  {0xC8, (uint8_t []){0xBB}, 1, 0},
  {0xC9, (uint8_t []){0xAA}, 1, 0},
  {0xD0, (uint8_t []){0x10}, 1, 0},
  {0xD1, (uint8_t []){0x47}, 1, 0},
  {0xD2, (uint8_t []){0x56}, 1, 0},
  {0xD3, (uint8_t []){0x65}, 1, 0},
  {0xD4, (uint8_t []){0x74}, 1, 0},
  {0xD5, (uint8_t []){0x88}, 1, 0},
  {0xD6, (uint8_t []){0x99}, 1, 0},
  {0xD7, (uint8_t []){0x01}, 1, 0},
  {0xD8, (uint8_t []){0xBB}, 1, 0},
  {0xD9, (uint8_t []){0xAA}, 1, 0},
  {0xF3, (uint8_t []){0x01}, 1, 0},
  {0xF0, (uint8_t []){0x00}, 1, 0},
  {0x21, (uint8_t []){0x00}, 1, 0},
  {0x11, (uint8_t []){0x00}, 1, 120},
  {0x29, (uint8_t []){0x00}, 1, 0},
};

/* ── 내부 핸들 ── */
static lv_display_t          *s_lvgl_display = NULL;
static esp_lcd_touch_handle_t  s_touch_handle = NULL;
static SemaphoreHandle_t       s_lvgl_mutex   = NULL;
static TaskHandle_t             s_lvgl_task   = NULL;
static bsp_touch_hook_fn_t      s_touch_hook  = NULL;
static i2c_master_bus_handle_t  s_i2c_bus     = NULL;

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

    ESP_RETURN_ON_ERROR(bsp_i2c_init(&i2c_bus), TAG, "I2C init failed");
    s_i2c_bus = i2c_bus;
    ESP_RETURN_ON_ERROR(tca9554pwr_init(i2c_bus, 0x00), TAG, "TCA9554 init failed");
    ESP_RETURN_ON_ERROR(bsp_lcd_init(&lcd_io, &lcd_panel), TAG, "LCD init failed");
    ESP_RETURN_ON_ERROR(bsp_backlight_init(), TAG, "Backlight init failed");
    ESP_RETURN_ON_ERROR(bsp_touch_init(i2c_bus), TAG, "Touch init failed");
    ESP_RETURN_ON_ERROR(bsp_lvgl_init(lcd_io, lcd_panel), TAG, "LVGL init failed");
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

/* LCD_RST은 TCA9554 EXIO2 경유 — Waveshare ST7701_Reset()과 동일 */
static void bsp_lcd_reset_via_exio(void)
{
    Set_EXIO(BSP_LCD_RST_EXIO, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    Set_EXIO(BSP_LCD_RST_EXIO, true);
    vTaskDelay(pdMS_TO_TICKS(50));
}

static esp_err_t bsp_lcd_init(esp_lcd_panel_io_handle_t *out_io,
                               esp_lcd_panel_handle_t   *out_panel)
{
    bsp_lcd_reset_via_exio();

    /* QSPI 버스 */
    const spi_bus_config_t bus_cfg = {
        .data0_io_num     = BSP_LCD_SPI_DATA0,
        .data1_io_num     = BSP_LCD_SPI_DATA1,
        .sclk_io_num      = BSP_LCD_SPI_SCK,
        .data2_io_num     = BSP_LCD_SPI_DATA2,
        .data3_io_num     = BSP_LCD_SPI_DATA3,
        .data4_io_num     = -1,
        .data5_io_num     = -1,
        .data6_io_num     = -1,
        .data7_io_num     = -1,
        .max_transfer_sz  = BSP_LCD_H_RES * BSP_LCD_V_RES * sizeof(uint16_t),
        .flags            = SPICOMMON_BUSFLAG_MASTER,
    };
    ESP_RETURN_ON_ERROR(
        spi_bus_initialize(BSP_LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
        TAG, "spi_bus_initialize failed");

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num       = BSP_LCD_CS,
        .dc_gpio_num       = -1,
        .spi_mode          = 0,
        .pclk_hz           = 3 * 1000 * 1000, /* 레지스터 프로브용 저속 — 아래서 본 속도로 재생성 */
        .trans_queue_depth = 10,
        .lcd_cmd_bits      = 32,
        .lcd_param_bits    = 8,
        .flags = {
            .quad_mode = 1,
        },
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_HOST,
                                  &io_cfg, out_io),
        TAG, "esp_lcd_new_panel_io_spi failed");

    /* Waveshare 데모와 동일: 레지스터 0x04를 읽어 패널 배치를 식별하고,
     * 특정 패턴(0x00,0x02,0x7F,0x7F)이면 전용 init 커맨드 테이블을 적용.
     * "기성품 최소 구성"으로 이 단계를 건너뛰었다가 실기에서 화면이 깜빡이다
     * 가로줄+그라데이션으로 멈추는 증상이 나와서 다시 포팅함. */
    st77916_vendor_config_t vendor_config = {
        .flags = {
            .use_qspi_interface = 1,
        },
    };

    int lcd_cmd = 0x04;
    uint8_t register_data[4] = { 0 };
    lcd_cmd &= 0xff;
    lcd_cmd <<= 8;
    lcd_cmd |= (0x0BULL << 24); /* LCD_OPCODE_READ_CMD */
    if (esp_lcd_panel_io_rx_param(*out_io, lcd_cmd, register_data, sizeof(register_data)) == ESP_OK) {
        ESP_LOGI(TAG, "ST77916 reg 0x04: %02x %02x %02x %02x",
                 register_data[0], register_data[1], register_data[2], register_data[3]);
        if (register_data[0] == 0x00 && register_data[1] == 0x02 &&
            register_data[2] == 0x7F && register_data[3] == 0x7F) {
            vendor_config.init_cmds = bsp_st77916_vendor_specific_init_new;
            vendor_config.init_cmds_size = sizeof(bsp_st77916_vendor_specific_init_new) / sizeof(st77916_lcd_init_cmd_t);
            ESP_LOGI(TAG, "ST77916 vendor-specific init 테이블 적용");
        }
    } else {
        ESP_LOGW(TAG, "ST77916 reg 0x04 read failed — default init sequence will be used");
    }

    /* 본 속도로 IO 재생성 (Waveshare 데모와 동일 패턴) */
    io_cfg.pclk_hz = BSP_LCD_PIXEL_CLK_HZ;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_HOST,
                                  &io_cfg, out_io),
        TAG, "esp_lcd_new_panel_io_spi (full speed) failed");

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1, /* EXIO로 이미 리셋함 */
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config  = &vendor_config,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_st77916(*out_io, &panel_cfg, out_panel),
        TAG, "esp_lcd_new_panel_st77916 failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(*out_panel), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(*out_panel),  TAG, "panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(*out_panel, true), TAG, "panel disp_on_off failed");

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
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_CST816_CONFIG();
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_i2c(bus, &tp_io_cfg, &tp_io_handle),
        TAG, "esp_lcd_new_panel_io_i2c failed");

    esp_lcd_touch_config_t tp_cfg = {
        .x_max        = BSP_LCD_H_RES,
        .y_max        = BSP_LCD_V_RES,
        .rst_gpio_num = -1, /* EXIO1로 드라이버 내부에서 리셋 */
        .int_gpio_num = BSP_TOUCH_INT,
        .flags = {
            .swap_xy  = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_touch_new_i2c_cst816(tp_io_handle, &tp_cfg, &s_touch_handle),
        TAG, "esp_lcd_touch_new_i2c_cst816 failed");

    ESP_LOGI(TAG, "[INIT] Touch OK  addr=0x%02X INT=%d",
             ESP_LCD_TOUCH_IO_I2C_CST816_ADDRESS, BSP_TOUCH_INT);
    return ESP_OK;
}

/* ── LVGL 플러시 콜백 ── */
static void bsp_lvgl_flush_cb(lv_display_t *disp,
                               const lv_area_t *area,
                               uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel =
        (esp_lcd_panel_handle_t)lv_display_get_user_data(disp);

    /* RGB565 바이트 스왑 — SPI little-endian 보정. 실기에서 색이 이상하면 이 줄을 빼고 확인 */
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
