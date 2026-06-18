/**
 * @file    bsp_ws_1_47.h
 * @brief   Waveshare ESP32-S3-Touch-LCD-1.47 커스텀 BSP
 *
 * 핀 정보 출처:
 *   - Waveshare 공식 핀맵 이미지 (확정)
 *   - Waveshare 공식 샘플 코드 bsp_display.h / bsp_touch.h / bsp_i2c.h
 *
 * 샘플코드 대비 제거: QMI8658, 배터리 ADC, SD카드, WiFi
 * 주의: 샘플코드의 TP_INT=47, TP_RST=48 은 핀맵 이미지와 반대이나
 *       실동작 기준인 샘플코드를 우선 신뢰함
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
 * LCD SPI 핀 (JD9853, 4-wire SPI) — 샘플코드 bsp_display.h 확정
 * ════════════════════════════════════════════════════════════ */
#define BSP_LCD_SPI_HOST        SPI2_HOST
#define BSP_LCD_SPI_MOSI        GPIO_NUM_39   ///< LCD_DIN
#define BSP_LCD_SPI_SCK         GPIO_NUM_38   ///< LCD_CLK
#define BSP_LCD_SPI_MISO        GPIO_NUM_NC   ///< 미사용
#define BSP_LCD_CS              GPIO_NUM_21   ///< LCD_CS
#define BSP_LCD_DC              GPIO_NUM_45   ///< LCD_DC
#define BSP_LCD_RST             GPIO_NUM_40   ///< LCD_RST
#define BSP_LCD_BL              GPIO_NUM_46   ///< LCD_BL

/* ════════════════════════════════════════════════════════════
 * LCD 파라미터 (JD9853, 172×320)
 * 주의: 샘플코드는 set_gap() 주석처리 버그 있음
 *       → 반드시 esp_lcd_panel_set_gap(panel, 34, 0) 호출 필요
 * ════════════════════════════════════════════════════════════ */
#define BSP_LCD_H_RES           (172)
#define BSP_LCD_V_RES           (320)
#define BSP_LCD_COL_OFFSET      (34)          ///< JD9853 x_gap — set_gap()으로 적용
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
#define BSP_LCD_BRIGHTNESS_DEFAULT  (80U)     ///< 기본 백라이트 밝기 (%)
#define BSP_LCD_BRIGHTNESS_OFF      (0U)      ///< 백라이트 OFF

/* ════════════════════════════════════════════════════════════
 * 터치 I2C 핀 (AXS5106L) — 샘플코드 bsp_touch.h / bsp_i2c.h 확정
 * ════════════════════════════════════════════════════════════ */
#define BSP_TOUCH_I2C_PORT      ((i2c_port_num_t)0)
#define BSP_TOUCH_I2C_SDA       GPIO_NUM_42   ///< TP_SDA
#define BSP_TOUCH_I2C_SCL       GPIO_NUM_41   ///< TP_SCL
#define BSP_TOUCH_INT           GPIO_NUM_47   ///< TP_INT (샘플코드 기준, 핀맵과 반전 주의)
#define BSP_TOUCH_RST           GPIO_NUM_48   ///< TP_RST (샘플코드 기준, 핀맵과 반전 주의)
#define BSP_TOUCH_I2C_ADDR      (0x63U)       ///< AXS5106L 확정 주소
#define BSP_TOUCH_I2C_CLK_HZ    (400000U)
#define BSP_TOUCH_I2C_GLITCH_CNT (7U)         ///< I2C 글리치 필터 카운트
#define BSP_TOUCH_MAX_POINTS    (1U)          ///< 최대 터치 포인트 수

/* ════════════════════════════════════════════════════════════
 * LVGL 설정
 * ════════════════════════════════════════════════════════════ */
#define BSP_LVGL_TICK_MS        (2U)
#define BSP_LVGL_BUF_LINES      (40U)         ///< DMA 드로우 버퍼 라인 수
#define BSP_LVGL_TASK_STACK     (8192U)       ///< LVGL 태스크 스택 크기
#define BSP_LVGL_TASK_PRIORITY  (4U)          ///< LVGL 태스크 우선순위
#define BSP_LVGL_TASK_CORE      (1)           ///< LVGL 태스크 실행 코어 (APP_CPU)

/* ════════════════════════════════════════════════════════════
 * 뮤텍스 대기 시간
 * ════════════════════════════════════════════════════════════ */
#define BSP_MUTEX_WAIT_FOREVER  (0U)          ///< 무한 대기
#define BSP_MUTEX_WAIT_DEFAULT  (1000U)       ///< 기본 대기 시간 (ms)

/* ════════════════════════════════════════════════════════════
 * 터치 인터셉터 훅
 *   - 터치 데이터가 LVGL에 전달되기 전에 호출됨
 *   - true 반환 시 해당 이벤트를 소비(LVGL 전달 차단)
 * ════════════════════════════════════════════════════════════ */
typedef bool (*bsp_touch_hook_fn_t)(bool pressed, lv_point_t pt);
void bsp_indev_set_hook(bsp_touch_hook_fn_t fn);

/* ════════════════════════════════════════════════════════════
 * BSP 공개 API
 * ════════════════════════════════════════════════════════════ */

/**
 * @brief 보드 전체 초기화
 *        순서: I2C → SPI/LCD → set_gap → invert → 터치 → LVGL → 백라이트
 */
esp_err_t bsp_board_init(void);

/**
 * @brief 보드 I2C 버스 핸들 반환 (터치와 공유, 추가 디바이스 부착용)
 */
i2c_master_bus_handle_t bsp_get_i2c_bus(void);

/**
 * @brief 백라이트 밝기 설정
 * @param brightness 0~100 (%)
 */
esp_err_t bsp_display_set_brightness(uint8_t brightness);

/**
 * @brief LVGL 디스플레이 핸들 반환 (bsp_board_init 후 유효)
 */
lv_display_t *bsp_get_lvgl_display(void);

/**
 * @brief LVGL 뮤텍스 획득 — UI 위젯 접근 전 반드시 호출
 * @param timeout_ms 대기 시간 ms, BSP_MUTEX_WAIT_FOREVER(0) = 무한 대기
 * @return true: 획득 성공
 */
bool bsp_lvgl_lock(uint32_t timeout_ms);

/**
 * @brief LVGL 뮤텍스 반환
 */
void bsp_lvgl_unlock(void);

#ifdef __cplusplus
}
#endif
