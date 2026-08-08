/**
 * @file    bsp_ws_1_85c.h
 * @brief   Waveshare ESP32-S3-Touch-LCD-1.85C(V2, 원형 360x360) 커스텀 BSP
 *
 * 핀/드라이버 구성 출처: 사용자 제공 V2 회로도(ESP32-S3-Touch-LCD-1.85C_V2.pdf) +
 * Waveshare 공식 ESP-IDF 데모(github.com/waveshareteam/ESP32-S3-Touch-LCD-1.85C)의
 * LCD_Driver/ST77916.h, Touch_Driver/CST816.h, EXIO/TCA9554PWR.h — 둘 다 대조해 확정.
 *
 * LCD_RST/TP_RST는 직접 GPIO가 아니라 TCA9554 IO 익스팬더(I2C, addr 0x20)의
 * EXIO2/EXIO1을 거친다 — bsp_ws_1_47과의 가장 큰 구조적 차이.
 *
 * 공개 API는 bsp_ws_1_47.h와 이름을 동일하게 맞춰서, main/ui_*.c 쪽은
 * #include 경로만 바꾸면 그대로 동작하도록 했다.
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
#include "tca9554pwr.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ════════════════════════════════════════════════════════════
 * LCD QSPI 핀 (ST77916) — Waveshare 공식 데모 ST77916.h 확정
 * ════════════════════════════════════════════════════════════ */
#define BSP_LCD_SPI_HOST        SPI2_HOST
#define BSP_LCD_SPI_SCK         GPIO_NUM_40   ///< LCD_SCK
#define BSP_LCD_SPI_DATA0       GPIO_NUM_46   ///< LCD_SDA0
#define BSP_LCD_SPI_DATA1       GPIO_NUM_45   ///< LCD_SDA1
#define BSP_LCD_SPI_DATA2       GPIO_NUM_42   ///< LCD_SDA2
#define BSP_LCD_SPI_DATA3       GPIO_NUM_41   ///< LCD_SDA3
#define BSP_LCD_CS              GPIO_NUM_21   ///< LCD_CS
#define BSP_LCD_TE              GPIO_NUM_18   ///< LCD_TE
#define BSP_LCD_BL              GPIO_NUM_5    ///< BL_PWM
/* LCD_RST은 직접 GPIO가 아니라 TCA9554 EXIO2 — BSP_LCD_RST_EXIO로 표기 */
#define BSP_LCD_RST_EXIO        TCA9554_EXIO2

/* ════════════════════════════════════════════════════════════
 * LCD 파라미터 (ST77916, 360×360 원형)
 * ════════════════════════════════════════════════════════════ */
#define BSP_LCD_H_RES           (360)
#define BSP_LCD_V_RES           (360)
#define BSP_LCD_PIXEL_CLK_HZ    (40 * 1000 * 1000)  ///< Waveshare 데모 ESP_PANEL_LCD_SPI_CLK_HZ 기본값

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
 * 공유 I2C 버스 — 터치(CST816) + TCA9554 IO 익스팬더
 * ════════════════════════════════════════════════════════════ */
#define BSP_I2C_PORT            ((i2c_port_num_t)0)
#define BSP_I2C_SDA             GPIO_NUM_11
#define BSP_I2C_SCL             GPIO_NUM_10
#define BSP_I2C_CLK_HZ          (400000U)
#define BSP_I2C_GLITCH_CNT      (7U)

/* ════════════════════════════════════════════════════════════
 * 터치 (CST816)
 * ════════════════════════════════════════════════════════════ */
#define BSP_TOUCH_INT           GPIO_NUM_4    ///< TP_INT
/* TP_RST은 직접 GPIO가 아니라 TCA9554 EXIO1 — esp_lcd_touch_cst816 드라이버 내부에서 처리 */
#define BSP_TOUCH_MAX_POINTS    (1U)

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
 * BSP 공개 API — bsp_ws_1_47.h와 동일한 이름/시그니처
 * ════════════════════════════════════════════════════════════ */

/**
 * @brief 보드 전체 초기화
 *        순서: I2C → TCA9554 익스팬더 → LCD 리셋(EXIO2)/QSPI/패널 → 백라이트 → 터치(EXIO1) → LVGL
 */
esp_err_t bsp_board_init(void);

/**
 * @brief 보드 I2C 버스 핸들 반환 (터치/TCA9554와 공유, 추가 디바이스 부착용)
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
