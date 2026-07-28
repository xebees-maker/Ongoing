/**
 * @file
 * @brief ESP LCD touch: CST816
 *
 * 원본: Waveshare 공식 데모(ESP32-S3-Touch-LCD-1.85C) Touch_Driver/CST816.c/.h.
 * reset()이 직접 GPIO가 아니라 TCA9554 IO 익스팬더(EXIO1)를 토글하는 것이 특징 —
 * esp_lcd_touch_config_t.rst_gpio_num은 쓰지 않음(-1로 둘 것).
 */
#pragma once

#include "esp_lcd_touch.h"
#include "esp_lcd_panel_io.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t esp_lcd_touch_new_i2c_cst816(const esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t *config, esp_lcd_touch_handle_t *out_touch);

#define ESP_LCD_TOUCH_IO_I2C_CST816_ADDRESS (0x15)

#define ESP_LCD_TOUCH_IO_I2C_CST816_CONFIG()              \
    {                                                      \
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_CST816_ADDRESS,   \
        .scl_speed_hz = (400000),                          \
        .control_phase_bytes = 1,                          \
        .dc_bit_offset = 0,                                \
        .lcd_cmd_bits = 8,                                  \
        .flags =                                            \
        {                                                    \
            .disable_control_phase = 1,                       \
        }                                                      \
    }

#ifdef __cplusplus
}
#endif
