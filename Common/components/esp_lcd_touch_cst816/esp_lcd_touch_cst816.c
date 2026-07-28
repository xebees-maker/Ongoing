#include "esp_lcd_touch_cst816.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "tca9554pwr.h"

#define POINT_NUM_MAX  (1)
#define DATA_START_REG (0x02)
#define CHIP_ID_REG    (0xA7)
#define AutoSleep_REG  (0xFE)

static const char *TAG = "esp_lcd_touch_cst816";

static esp_err_t read_data(esp_lcd_touch_handle_t tp);
static bool get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num, uint8_t max_point_num);
static esp_err_t del(esp_lcd_touch_handle_t tp);

static esp_err_t i2c_read_bytes(esp_lcd_touch_handle_t tp, uint16_t reg, uint8_t *data, uint8_t len);
static esp_err_t i2c_write_bytes(esp_lcd_touch_handle_t tp, uint16_t reg, uint8_t *data, uint8_t len);

static esp_err_t reset(esp_lcd_touch_handle_t tp);
static esp_err_t read_id(esp_lcd_touch_handle_t tp);
static void auto_sleep(esp_lcd_touch_handle_t tp, bool sleep_enabled);

esp_err_t esp_lcd_touch_new_i2c_cst816(const esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t *config, esp_lcd_touch_handle_t *out_touch)
{
    ESP_RETURN_ON_FALSE(io, ESP_ERR_INVALID_ARG, TAG, "Invalid io");
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "Invalid config");
    ESP_RETURN_ON_FALSE(out_touch, ESP_ERR_INVALID_ARG, TAG, "Invalid touch handle");

    esp_err_t ret = ESP_OK;
    esp_lcd_touch_handle_t cst816 = calloc(1, sizeof(esp_lcd_touch_t));
    ESP_GOTO_ON_FALSE(cst816, ESP_ERR_NO_MEM, err, TAG, "Touch handle malloc failed");

    cst816->io = io;
    cst816->read_data = read_data;
    cst816->get_xy = get_xy;
    cst816->del = del;
    cst816->data.lock.owner = portMUX_FREE_VAL;
    memcpy(&cst816->config, config, sizeof(esp_lcd_touch_config_t));

    /* TP_INT은 직접 GPIO. TP_RST는 TCA9554 EXIO1을 거치므로 rst_gpio_num은 안 씀(reset() 참조) */
    if (cst816->config.int_gpio_num != GPIO_NUM_NC) {
        const gpio_config_t int_gpio_config = {
            .mode = GPIO_MODE_INPUT,
            .intr_type = (cst816->config.levels.interrupt ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE),
            .pin_bit_mask = BIT64(cst816->config.int_gpio_num)
        };
        ESP_GOTO_ON_ERROR(gpio_config(&int_gpio_config), err, TAG, "GPIO intr config failed");
        if (cst816->config.interrupt_callback) {
            esp_lcd_touch_register_interrupt_callback(cst816, cst816->config.interrupt_callback);
        }
    }

    ESP_GOTO_ON_ERROR(reset(cst816), err, TAG, "Reset failed");
    ESP_GOTO_ON_ERROR(read_id(cst816), err, TAG, "Read id failed");
    *out_touch = cst816;
    auto_sleep(cst816, false);
    return ESP_OK;

err:
    if (cst816) {
        del(cst816);
    }
    ESP_LOGE(TAG, "Initialization failed!");
    return ret;
}

static esp_err_t read_data(esp_lcd_touch_handle_t tp)
{
    typedef struct {
        uint8_t num;
        uint8_t x_h : 4;
        uint8_t     : 4;
        uint8_t x_l;
        uint8_t y_h : 4;
        uint8_t     : 4;
        uint8_t y_l;
    } data_t;

    data_t point;
    ESP_RETURN_ON_ERROR(i2c_read_bytes(tp, DATA_START_REG, (uint8_t *)&point, sizeof(data_t)), TAG, "I2C read failed");

    portENTER_CRITICAL(&tp->data.lock);
    point.num = (point.num > POINT_NUM_MAX ? POINT_NUM_MAX : point.num);
    tp->data.points = point.num;
    for (int i = 0; i < point.num; i++) {
        tp->data.coords[i].x = point.x_h << 8 | point.x_l;
        tp->data.coords[i].y = point.y_h << 8 | point.y_l;
    }
    portEXIT_CRITICAL(&tp->data.lock);

    return ESP_OK;
}

static bool get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num, uint8_t max_point_num)
{
    portENTER_CRITICAL(&tp->data.lock);
    *point_num = (tp->data.points > max_point_num ? max_point_num : tp->data.points);
    for (size_t i = 0; i < *point_num; i++) {
        x[i] = tp->data.coords[i].x;
        y[i] = tp->data.coords[i].y;
        if (strength) {
            strength[i] = tp->data.coords[i].strength;
        }
    }
    tp->data.points = 0;
    portEXIT_CRITICAL(&tp->data.lock);

    return (*point_num > 0);
}

static esp_err_t del(esp_lcd_touch_handle_t tp)
{
    if (tp->config.int_gpio_num != GPIO_NUM_NC) {
        gpio_reset_pin(tp->config.int_gpio_num);
        if (tp->config.interrupt_callback) {
            gpio_isr_handler_remove(tp->config.int_gpio_num);
        }
    }
    free(tp);
    return ESP_OK;
}

static esp_err_t reset(esp_lcd_touch_handle_t tp)
{
    (void)tp;
    Set_EXIO(TCA9554_EXIO1, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    Set_EXIO(TCA9554_EXIO1, true);
    vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
}

static esp_err_t read_id(esp_lcd_touch_handle_t tp)
{
    uint8_t id;
    ESP_RETURN_ON_ERROR(i2c_read_bytes(tp, CHIP_ID_REG, &id, 1), TAG, "I2C read failed");
    ESP_LOGI(TAG, "IC id: %d", id);
    return ESP_OK;
}

static void auto_sleep(esp_lcd_touch_handle_t tp, bool sleep_enabled)
{
    uint8_t val = (uint8_t)(!sleep_enabled);
    i2c_write_bytes(tp, AutoSleep_REG, &val, 1);
}

static esp_err_t i2c_read_bytes(esp_lcd_touch_handle_t tp, uint16_t reg, uint8_t *data, uint8_t len)
{
    ESP_RETURN_ON_FALSE(data, ESP_ERR_INVALID_ARG, TAG, "Invalid data");
    return esp_lcd_panel_io_rx_param(tp->io, reg, data, len);
}

static esp_err_t i2c_write_bytes(esp_lcd_touch_handle_t tp, uint16_t reg, uint8_t *data, uint8_t len)
{
    assert(tp != NULL);
    return esp_lcd_panel_io_tx_param(tp->io, reg, data, len);
}
