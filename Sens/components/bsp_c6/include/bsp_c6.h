/**
 * @file    bsp_c6.h
 * @brief   Waveshare ESP32-C6-Touch-LCD-1.47 커스텀 BSP
 *
 * 핀 정보 출처: Waveshare 공식 ESP-IDF 데모(01_factory) bsp_display.h /
 *              bsp_touch.h / bsp_i2c.h / bsp_spi.h / bsp_battery.h — 확정값
 */
#pragma once

#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ════════════════════════════════════════════════════════════
 * LCD SPI 핀 (JD9853, 4-wire SPI) — 공식 데모 bsp_spi.h/bsp_display.h 확정
 * ════════════════════════════════════════════════════════════ */
#define BSP_LCD_SPI_HOST        SPI2_HOST
#define BSP_LCD_SPI_MOSI        GPIO_NUM_2    ///< LCD_DIN
#define BSP_LCD_SPI_SCK         GPIO_NUM_1    ///< LCD_CLK
#define BSP_LCD_SPI_MISO        GPIO_NUM_NC   ///< 미사용
#define BSP_LCD_CS              GPIO_NUM_14   ///< LCD_CS
#define BSP_LCD_DC              GPIO_NUM_15   ///< LCD_DC
#define BSP_LCD_RST             GPIO_NUM_22   ///< LCD_RST
#define BSP_LCD_BL              GPIO_NUM_23   ///< LCD_BL

/* ════════════════════════════════════════════════════════════
 * LCD 파라미터 (JD9853, 172×320) — S3 보드와 동일 패널
 * ════════════════════════════════════════════════════════════ */
#define BSP_LCD_H_RES           (172)
#define BSP_LCD_V_RES           (320)
#define BSP_LCD_COL_OFFSET      (34)
#define BSP_LCD_ROW_OFFSET      (0)
#define BSP_LCD_PIXEL_CLK_HZ    (80 * 1000 * 1000)  ///< 80MHz

/* ════════════════════════════════════════════════════════════
 * 백라이트 LEDC PWM
 * ════════════════════════════════════════════════════════════ */
#define BSP_LCD_LEDC_TIMER      LEDC_TIMER_0
#define BSP_LCD_LEDC_MODE       LEDC_LOW_SPEED_MODE
#define BSP_LCD_LEDC_CHANNEL    LEDC_CHANNEL_0
#define BSP_LCD_LEDC_DUTY_RES   LEDC_TIMER_10_BIT
#define BSP_LCD_LEDC_DUTY_MAX   (1024U)
#define BSP_LCD_LEDC_FREQ_HZ    (5000U)
#define BSP_LCD_BRIGHTNESS_DEFAULT  (80U)
#define BSP_LCD_BRIGHTNESS_OFF      (0U)

/* ════════════════════════════════════════════════════════════
 * 터치 I2C 핀 (AXS5106L) — 공식 데모 bsp_touch.h/bsp_i2c.h 확정
 * ════════════════════════════════════════════════════════════ */
#define BSP_TOUCH_I2C_PORT      ((i2c_port_num_t)0)
#define BSP_TOUCH_I2C_SDA       GPIO_NUM_18
#define BSP_TOUCH_I2C_SCL       GPIO_NUM_19
#define BSP_TOUCH_INT           GPIO_NUM_21
#define BSP_TOUCH_RST           GPIO_NUM_20
#define BSP_TOUCH_I2C_ADDR      (0x63U)
#define BSP_TOUCH_I2C_CLK_HZ    (400000U)
#define BSP_TOUCH_I2C_GLITCH_CNT (7U)
#define BSP_TOUCH_MAX_POINTS    (1U)

/* ════════════════════════════════════════════════════════════
 * 배터리 ADC — 공식 데모 bsp_battery.h 확정 (전용 게이트 핀 없음)
 * ════════════════════════════════════════════════════════════ */
#define BSP_BATTERY_ADC_UNIT    ADC_UNIT_1
#define BSP_BATTERY_ADC_CHANNEL ADC_CHANNEL_0
#define BSP_BATTERY_ADC_ATTEN   ADC_ATTEN_DB_12
#define BSP_BATTERY_DIV         (3.0f)

/* ════════════════════════════════════════════════════════════
 * 버튼 (BOOT)
 * ════════════════════════════════════════════════════════════ */
#define BSP_BUTTON_GPIO         GPIO_NUM_9

/* ════════════════════════════════════════════════════════════
 * LVGL 설정
 * ════════════════════════════════════════════════════════════ */
#define BSP_LVGL_TICK_MS        (2U)
#define BSP_LVGL_BUF_LINES      (40U)
#define BSP_LVGL_TASK_STACK     (8192U)
#define BSP_LVGL_TASK_PRIORITY  (4U)
#define BSP_LVGL_TASK_CORE      (0)   ///< ESP32-C6는 단일 코어

/* ════════════════════════════════════════════════════════════
 * 뮤텍스 대기 시간
 * ════════════════════════════════════════════════════════════ */
#define BSP_MUTEX_WAIT_FOREVER  (0U)
#define BSP_MUTEX_WAIT_DEFAULT  (1000U)

/* ════════════════════════════════════════════════════════════
 * 터치 인터셉터 훅
 * ════════════════════════════════════════════════════════════ */
typedef bool (*bsp_touch_hook_fn_t)(bool pressed, lv_point_t pt);
void bsp_indev_set_hook(bsp_touch_hook_fn_t fn);

/* ════════════════════════════════════════════════════════════
 * BSP 공개 API
 * ════════════════════════════════════════════════════════════ */
esp_err_t bsp_board_init(void);
i2c_master_bus_handle_t bsp_get_i2c_bus(void);
esp_err_t bsp_display_set_brightness(uint8_t brightness);
lv_display_t *bsp_get_lvgl_display(void);
bool bsp_lvgl_lock(uint32_t timeout_ms);
void bsp_lvgl_unlock(void);

#ifdef __cplusplus
}
#endif
