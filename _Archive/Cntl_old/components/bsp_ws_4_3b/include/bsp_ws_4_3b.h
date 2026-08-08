/**
 * @file    bsp_ws_4_3b.h
 * @brief   Waveshare ESP32-S3-Touch-LCD-4.3B(800x480 RGB, 하우징+RS485/CAN/절연IO) BSP
 *
 * 핀/드라이버 구성 출처: Waveshare 공식 ESP-IDF 데모
 * (github.com/waveshareteam/ESP32-S3-Touch-LCD-4.3B) examples/ESP-IDF/09_lvgl_v9_demo
 * (components/waveshare_rgb_lcd_port.c/.h)와 05_IO_Test(lib/CH422G) 그대로 확인.
 * 이전 1.85C(원형 360x360, ST77916+CST816+TCA9554)와는 완전히 다른 보드 —
 * bsp_ws_1_85c는 더 이상 안 씀(project_cntl_4_3b_bsp 메모리 참고).
 *
 * 백라이트는 CH422G IO2를 통한 on/off만 가능 — PWM 밝기조절 하드웨어 자체가 없음
 * (공식 데모에도 디밍 코드 전혀 없음, 사용자 확인 후 on/off만 지원하기로 결정).
 *
 * 공개 API는 bsp_ws_1_85c.h와 최대한 이름을 맞춰서 main/ui_*.c 쪽 호출부 변경을
 * 최소화했다 — 단, bsp_display_set_brightness()는 0=off, 그 외 모두 on으로 동작(연속
 * 값이 아님을 시그니처는 유지하되 동작이 다름을 문서화).
 */
#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "lvgl.h"
#include "ch422g.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ════════════════════════════════════════════════════════════
 * 해상도/RGB 패널 (Waveshare 공식 데모 기준, 800x480 기본형)
 * ════════════════════════════════════════════════════════════ */
#define BSP_LCD_H_RES               (800)
#define BSP_LCD_V_RES               (480)
#define BSP_LCD_PIXEL_CLK_HZ        (16 * 1000 * 1000)
/* 2로 하면 esp_lcd_panel_draw_bitmap()이 어느 버퍼에 그리는지와 실제 화면에
 * 나오는(스캔아웃 중인) 버퍼가 안 맞아 화면이 하얗게(=버퍼 초기값) 나오는 문제가
 * 있었음 — 벤더 데모는 이걸 esp_lv_adapter라는 별도 어댑터로 동기화해주는데
 * 우리는 직접 draw_bitmap만 부르는 단순한 방식이라 1로 둬서 이 문제 자체를 피함
 * (그리는 버퍼 = 보이는 버퍼, 바운스버퍼가 PSRAM<->패널 대역폭은 이미 커버함) */
#define BSP_LCD_FB_COUNT            (1U)   ///< RGB 패널 프레임버퍼 개수(PSRAM)
#define BSP_LCD_BOUNCE_LINES        (10U)  ///< PSRAM 대역폭 튀는 것 방지용 SRAM 바운스 버퍼 줄 수

/* RGB 타이밍 — 공식 데모 800x480 기본값 */
#define BSP_LCD_HSYNC_PULSE_WIDTH   (4)
#define BSP_LCD_HSYNC_BACK_PORCH    (8)
#define BSP_LCD_HSYNC_FRONT_PORCH   (8)
#define BSP_LCD_VSYNC_PULSE_WIDTH   (4)
#define BSP_LCD_VSYNC_BACK_PORCH    (8)
#define BSP_LCD_VSYNC_FRONT_PORCH   (8)

/* ════════════════════════════════════════════════════════════
 * RGB LCD 핀
 * ════════════════════════════════════════════════════════════ */
#define BSP_LCD_PIN_VSYNC   GPIO_NUM_3
#define BSP_LCD_PIN_HSYNC   GPIO_NUM_46
#define BSP_LCD_PIN_DE      GPIO_NUM_5
#define BSP_LCD_PIN_PCLK    GPIO_NUM_7
#define BSP_LCD_PIN_DATA0   GPIO_NUM_14
#define BSP_LCD_PIN_DATA1   GPIO_NUM_38
#define BSP_LCD_PIN_DATA2   GPIO_NUM_18
#define BSP_LCD_PIN_DATA3   GPIO_NUM_17
#define BSP_LCD_PIN_DATA4   GPIO_NUM_10
#define BSP_LCD_PIN_DATA5   GPIO_NUM_39
#define BSP_LCD_PIN_DATA6   GPIO_NUM_0
#define BSP_LCD_PIN_DATA7   GPIO_NUM_45
#define BSP_LCD_PIN_DATA8   GPIO_NUM_48
#define BSP_LCD_PIN_DATA9   GPIO_NUM_47
#define BSP_LCD_PIN_DATA10  GPIO_NUM_21
#define BSP_LCD_PIN_DATA11  GPIO_NUM_1
#define BSP_LCD_PIN_DATA12  GPIO_NUM_2
#define BSP_LCD_PIN_DATA13  GPIO_NUM_42
#define BSP_LCD_PIN_DATA14  GPIO_NUM_41
#define BSP_LCD_PIN_DATA15  GPIO_NUM_40
/* LCD 리셋/백라이트는 직접 GPIO가 아니라 CH422G IO3/IO2 경유 */

/* ════════════════════════════════════════════════════════════
 * 공유 I2C 버스 — 터치(GT911) + CH422G IO 익스팬더
 * ════════════════════════════════════════════════════════════ */
#define BSP_I2C_PORT            ((i2c_port_num_t)0)
#define BSP_I2C_SDA             GPIO_NUM_8
#define BSP_I2C_SCL             GPIO_NUM_9
#define BSP_I2C_CLK_HZ          (400000U)
#define BSP_I2C_GLITCH_CNT      (7U)

/* ════════════════════════════════════════════════════════════
 * 터치 (GT911) — RST/INT 전부 CH422G IO1 경유(INT 핀은 이 보드에서 미사용, -1)
 * ════════════════════════════════════════════════════════════ */
#define BSP_TOUCH_MAX_POINTS    (5U)  ///< GT911 5점 멀티터치

/* ════════════════════════════════════════════════════════════
 * LVGL 설정
 * ════════════════════════════════════════════════════════════ */
#define BSP_LVGL_TICK_MS        (2U)
#define BSP_LVGL_BUF_LINES      (60U)
#define BSP_LVGL_TASK_STACK     (8192U)
#define BSP_LVGL_TASK_PRIORITY  (4U)
#define BSP_LVGL_TASK_CORE      (1)

#define BSP_MUTEX_WAIT_FOREVER  (0U)
#define BSP_MUTEX_WAIT_DEFAULT  (1000U)

/* 이 보드는 PWM 디밍이 없어서 0~100% 값이 아니라 on/off 두 값만 의미 있음 —
 * 1.85C 시절 호출부(ui_screen.c 등)를 그대로 쓸 수 있게 이름만 유지 */
#define BSP_LCD_BRIGHTNESS_DEFAULT  (1U)  ///< on
#define BSP_LCD_BRIGHTNESS_OFF      (0U)  ///< off

typedef bool (*bsp_touch_hook_fn_t)(bool pressed, lv_point_t pt);
void bsp_indev_set_hook(bsp_touch_hook_fn_t fn);

/**
 * @brief 보드 전체 초기화 — I2C → CH422G → LCD 리셋/RGB패널 → 백라이트 ON → GT911 터치 → LVGL
 */
esp_err_t bsp_board_init(void);

/** @brief 보드 I2C 버스 핸들 반환(터치/CH422G와 공유, 추가 디바이스 부착용) */
i2c_master_bus_handle_t bsp_get_i2c_bus(void);

/**
 * @brief 백라이트 on/off. 이 보드는 PWM 디밍 하드웨어가 없어서 0=off, 그 외 값은
 *        전부 on으로 취급(0~100% 연속 밝기 아님).
 */
esp_err_t bsp_display_set_brightness(uint8_t brightness);

/** @brief LVGL 디스플레이 핸들 반환(bsp_board_init 후 유효) */
lv_display_t *bsp_get_lvgl_display(void);

/** @brief LVGL 뮤텍스 획득 — UI 위젯 접근 전 반드시 호출 */
bool bsp_lvgl_lock(uint32_t timeout_ms);

/** @brief LVGL 뮤텍스 반환 */
void bsp_lvgl_unlock(void);

/**
 * @brief SSR(절연 출력 DO0/DO1) 제어 — CH422G OC0/OC1 경유, LCD/터치와 무관한 별도 뱅크.
 * @param channel 0 또는 1
 * @param on true=통전(SSR 닫힘), false=차단
 */
esp_err_t bsp_ssr_set(uint8_t channel, bool on);

/**
 * @brief 절연 입력 DI0/DI1 읽기 — 호출 중 아주 짧게 IO뱅크가 입력모드로 전환됨(SD카드
 *        트랜잭션과 겹치지 않게 호출부에서 주의).
 */
esp_err_t bsp_isolated_input_read(bool *out_di0, bool *out_di1);

#ifdef __cplusplus
}
#endif
