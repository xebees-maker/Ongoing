#include "bsp_esp32s3_cam.h"
#include "io_expander_ch32v003.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "bsp_esp32s3_cam";

static i2c_master_bus_handle_t s_i2c_bus = NULL;

/* 2026-09-26 — PWR_LED(EXIO6) 쓰기 전용 태스크. 상태 LED 패턴 변경이 WiFi 콜백(send_cb/recv_cb)
 * 과 esp_timer 태스크에서 일어나는데, 거기서 I2C 트랜잭션을 직접 하면 그 태스크들이 버스 대기만큼
 * 막힘 — 알림(eSetValueWithOverwrite)으로 최신 값만 넘기고 여기서 씀(이벤트 방식, 폴링 없음) */
#define PWR_LED_TASK_STACK  3072
#define PWR_LED_TASK_PRIO   3

static TaskHandle_t       s_pwr_led_task = NULL;
static StaticTask_t       s_pwr_led_tcb;
static SemaphoreHandle_t  s_pwr_led_mutex = NULL;  /* s_pwr_led_disabled 확인+쓰기를 shutdown과 직렬화 */
static StaticSemaphore_t  s_pwr_led_mutex_buf;
static bool               s_pwr_led_disabled = false;

static void pwr_led_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t value = 0;
        xTaskNotifyWait(0, 0, &value, portMAX_DELAY);
        xSemaphoreTake(s_pwr_led_mutex, portMAX_DELAY);
        if (!s_pwr_led_disabled) {
            ch32v003_set_output(BSP_CAM_IO_EXPANDER_PWR_LED_PIN, value != 0);
        }
        xSemaphoreGive(s_pwr_led_mutex);
    }
}

static esp_err_t pwr_led_start(void)
{
    s_pwr_led_mutex = xSemaphoreCreateMutexStatic(&s_pwr_led_mutex_buf);

    StackType_t *stack = (StackType_t *)heap_caps_malloc(PWR_LED_TASK_STACK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!stack) stack = (StackType_t *)heap_caps_malloc(PWR_LED_TASK_STACK, MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(stack, ESP_ERR_NO_MEM, TAG, "PWR_LED 태스크 스택 할당 실패");

    s_pwr_led_task = xTaskCreateStatic(pwr_led_task, "pwr_led", PWR_LED_TASK_STACK / sizeof(StackType_t),
                                       NULL, PWR_LED_TASK_PRIO, stack, &s_pwr_led_tcb);
    return ESP_OK;
}

void bsp_esp32s3_cam_pwr_led_set(bool on)
{
    if (!s_pwr_led_task) return;
    xTaskNotify(s_pwr_led_task, on ? 1u : 0u, eSetValueWithOverwrite);
}

void bsp_esp32s3_cam_pwr_led_shutdown(void)
{
    if (!s_pwr_led_mutex) return;
    xSemaphoreTake(s_pwr_led_mutex, portMAX_DELAY);
    s_pwr_led_disabled = true;
    ch32v003_set_output(BSP_CAM_IO_EXPANDER_PWR_LED_PIN, false);
    xSemaphoreGive(s_pwr_led_mutex);
}

esp_err_t bsp_esp32s3_cam_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source            = I2C_CLK_SRC_DEFAULT,
        .i2c_port               = BSP_CAM_I2C_PORT,
        .scl_io_num             = BSP_CAM_I2C_SCL,
        .sda_io_num             = BSP_CAM_I2C_SDA,
        .glitch_ignore_cnt      = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus), TAG, "i2c_new_master_bus failed");

    ESP_RETURN_ON_ERROR(ch32v003_init(s_i2c_bus), TAG, "ch32v003_init failed");

    /* 2026-08-23 — 배터리 전원 자체유지(래치). PWR 버튼은 배터리를 레귤레이터에 일시
     * 연결만 해주고, 이 핀을 소프트웨어가 켜야 버튼을 놔도 계속 켜져 있음. 가장 먼저 켬 —
     * 늦으면 그 사이에 버튼을 놓았을 때 전원이 나갈 수 있음 */
    ch32v003_set_output(BSP_CAM_IO_EXPANDER_BAT_EN_PIN, true);

    /* 2026-09-26 — 예전엔 여기서 IO2/IO6을 "SD 인에이블"로 켰음(Waveshare SD 예제 재현). 스키매틱
     * 확인 결과 IO2=SD_CS, IO6=PWR_LED — SD는 제거됐고 IO6은 상태 LED로 씀(꺼진 채 시작) */
    ESP_RETURN_ON_ERROR(pwr_led_start(), TAG, "PWR_LED 태스크 시작 실패");

    ESP_LOGI(TAG, "보드 초기화 완료 (I2C SDA=%d SCL=%d, BAT_EN ON)",
             BSP_CAM_I2C_SDA, BSP_CAM_I2C_SCL);
    return ESP_OK;
}

i2c_master_bus_handle_t bsp_esp32s3_cam_get_i2c_bus(void)
{
    return s_i2c_bus;
}
