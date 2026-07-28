#include "tca9554pwr.h"
#include "esp_check.h"
#include "esp_log.h"

static i2c_master_dev_handle_t i2c_handle;

static const char *TAG = "tca9554";

uint8_t Read_REG(uint8_t REG)
{
    uint8_t bitsStatus = 0;
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(i2c_handle, (uint8_t[]) {REG}, 1, &bitsStatus, 1, 0xffff), TAG, "Read input reg failed");
    return bitsStatus;
}

void Write_REG(uint8_t REG, uint8_t Data)
{
    uint8_t data[] = {REG, Data};
    ESP_RETURN_VOID_ON_ERROR(i2c_master_transmit(i2c_handle, data, sizeof(data), 0xffff), TAG, "Write output reg failed");
}

void Mode_EXIO(uint8_t Pin, uint8_t State)
{
    (void)State;
    uint8_t bitsStatus = Read_REG(TCA9554_CONFIG_REG);
    uint8_t Data = (0x01 << (Pin - 1)) | bitsStatus;
    Write_REG(TCA9554_CONFIG_REG, Data);
}

void Mode_EXIOS(uint8_t PinState)
{
    Write_REG(TCA9554_CONFIG_REG, PinState);
}

uint8_t Read_EXIO(uint8_t Pin)
{
    uint8_t inputBits = Read_REG(TCA9554_INPUT_REG);
    return (inputBits >> (Pin - 1)) & 0x01;
}

uint8_t Read_EXIOS(void)
{
    return Read_REG(TCA9554_INPUT_REG);
}

void Set_EXIO(uint8_t Pin, bool State)
{
    if (Pin < 1 || Pin > 8) {
        ESP_LOGE(TAG, "Set_EXIO: invalid pin %d", Pin);
        return;
    }
    uint8_t bitsStatus = Read_REG(TCA9554_OUTPUT_REG);
    uint8_t Data = State ? ((0x01 << (Pin - 1)) | bitsStatus)
                         : (~(0x01 << (Pin - 1)) & bitsStatus);
    Write_REG(TCA9554_OUTPUT_REG, Data);
}

void Set_EXIOS(uint8_t PinState)
{
    Write_REG(TCA9554_OUTPUT_REG, PinState);
}

void Set_Toggle(uint8_t Pin)
{
    uint8_t bitsStatus = Read_EXIO(Pin);
    Set_EXIO(Pin, (bool)!bitsStatus);
}

esp_err_t tca9554pwr_init(i2c_master_bus_handle_t i2c_bus, uint8_t pin_state)
{
    const i2c_device_config_t i2c_dev_cfg = {
        .device_address = TCA9554_ADDRESS,
        .scl_speed_hz   = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_bus, &i2c_dev_cfg, &i2c_handle),
                         TAG, "Add new I2C device failed");

    Mode_EXIOS(pin_state);
    return ESP_OK;
}
