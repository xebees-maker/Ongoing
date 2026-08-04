/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "waveshare_rgb_lcd_port.h"

static const char *TAG = "example";

/* IDF 6.0.2 removed the legacy driver/i2c.h master API (i2c_param_config/
 * i2c_driver_install/i2c_master_write_to_device) that this example originally used
 * (it's written against an older IDF, per README "ESP-IDF >= 5.5"); ported to the
 * new i2c_master bus/device API here. Byte values, addresses, order and delays vs
 * the upstream vendor demo are otherwise unchanged. */
static i2c_master_bus_handle_t s_i2c_bus       = NULL;
static i2c_master_dev_handle_t s_ch422g_mode_dev = NULL; // I2C addr 0x24 - CH422G mode register
static i2c_master_dev_handle_t s_ch422g_data_dev = NULL; // I2C addr 0x38 - CH422G output register

static esp_err_t i2c_write_byte(i2c_master_dev_handle_t dev, uint8_t val)
{
    return i2c_master_transmit(dev, &val, 1, I2C_MASTER_TIMEOUT_MS);
}

/**
 * @brief I2C master initialization
 */
static esp_err_t i2c_master_init(void)
{
    if (s_i2c_bus != NULL) {
        return ESP_OK;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_MASTER_NUM,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (ret != ESP_OK) {
        return ret;
    }

    i2c_device_config_t mode_dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x24,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    ret = i2c_master_bus_add_device(s_i2c_bus, &mode_dev_cfg, &s_ch422g_mode_dev);
    if (ret != ESP_OK) {
        return ret;
    }

    i2c_device_config_t data_dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x38,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    return i2c_master_bus_add_device(s_i2c_bus, &data_dev_cfg, &s_ch422g_data_dev);
}

#if CONFIG_EXAMPLE_LCD_TOUCH_CONTROLLER_GT911

// GPIO initialization
void gpio_init(void)
{
    // Zero-initialize the config structure
    gpio_config_t io_conf = {};
    // Disable interrupt
    io_conf.intr_type = GPIO_INTR_DISABLE;
    // Bit mask of the pins, use GPIO4 here
    io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;
    // Set as input mode
    io_conf.mode = GPIO_MODE_OUTPUT;

    gpio_config(&io_conf);
}

// Reset the touch screen
static void waveshare_esp32_s3_touch_reset(void)
{
    i2c_write_byte(s_ch422g_mode_dev, 0x01);

    // Reset the touch screen. It is recommended to reset the touch screen before using it.
    i2c_write_byte(s_ch422g_data_dev, 0x2C);
    esp_rom_delay_us(100 * 1000);
    gpio_set_level(GPIO_INPUT_IO_4, 0);
    esp_rom_delay_us(100 * 1000);
    i2c_write_byte(s_ch422g_data_dev, 0x2E);
    esp_rom_delay_us(200 * 1000);
}

#endif

// Initialize RGB LCD
esp_err_t waveshare_esp32_s3_rgb_lcd_init(uint8_t frame_buffer_count,
                                          esp_lcd_panel_handle_t *panel_handle,
                                          esp_lcd_touch_handle_t *touch_handle)
{
    if (panel_handle == NULL || touch_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *panel_handle = NULL;
    *touch_handle = NULL;

    ESP_LOGI(TAG, "Install RGB LCD panel driver"); // Log the start of the RGB LCD panel driver installation
    esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT, // Set the clock source for the panel
        .timings = {
            .pclk_hz = EXAMPLE_LCD_PIXEL_CLOCK_HZ, // Pixel clock frequency
            .h_res = EXAMPLE_LCD_H_RES,            // Horizontal resolution
            .v_res = EXAMPLE_LCD_V_RES,            // Vertical resolution
#if ESP_PANEL_USE_1024_600_LCD
            .hsync_back_porch = 145, // Horizontal sync pulse width
            .hsync_front_porch = 170, // Horizontal back porch
            .hsync_pulse_width = 30, // Horizontal front porch
            .vsync_back_porch = 23,  // Vertical sync pulse width
            .vsync_front_porch = 12,  // Vertical back porch
            .vsync_pulse_width = 2,  // Vertical front porch
#else
            .hsync_pulse_width = 4, // Horizontal sync pulse width
            .hsync_back_porch = 8,  // Horizontal back porch
            .hsync_front_porch = 8, // Horizontal front porch
            .vsync_pulse_width = 4, // Vertical sync pulse width
            .vsync_back_porch = 8,  // Vertical back porch
            .vsync_front_porch = 8, // Vertical front porch
#endif
            .flags = {
                .pclk_active_neg = 1, // Active low pixel clock
            },
        },
        .data_width = EXAMPLE_RGB_DATA_WIDTH,                    // Data width for RGB
        /* bits_per_pixel/sram_trans_align/psram_trans_align (upstream vendor demo, older IDF)
         * were replaced by in/out_color_format in the RGB panel driver on this IDF version. */
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = frame_buffer_count,                           // Number of frame buffers
        .bounce_buffer_size_px = EXAMPLE_RGB_BOUNCE_BUFFER_SIZE, // Bounce buffer size in pixels
        .hsync_gpio_num = EXAMPLE_LCD_IO_RGB_HSYNC,              // GPIO number for horizontal sync
        .vsync_gpio_num = EXAMPLE_LCD_IO_RGB_VSYNC,              // GPIO number for vertical sync
        .de_gpio_num = EXAMPLE_LCD_IO_RGB_DE,                    // GPIO number for data enable
        .pclk_gpio_num = EXAMPLE_LCD_IO_RGB_PCLK,                // GPIO number for pixel clock
        .disp_gpio_num = EXAMPLE_LCD_IO_RGB_DISP,                // GPIO number for display
        .data_gpio_nums = {
            EXAMPLE_LCD_IO_RGB_DATA0,
            EXAMPLE_LCD_IO_RGB_DATA1,
            EXAMPLE_LCD_IO_RGB_DATA2,
            EXAMPLE_LCD_IO_RGB_DATA3,
            EXAMPLE_LCD_IO_RGB_DATA4,
            EXAMPLE_LCD_IO_RGB_DATA5,
            EXAMPLE_LCD_IO_RGB_DATA6,
            EXAMPLE_LCD_IO_RGB_DATA7,
            EXAMPLE_LCD_IO_RGB_DATA8,
            EXAMPLE_LCD_IO_RGB_DATA9,
            EXAMPLE_LCD_IO_RGB_DATA10,
            EXAMPLE_LCD_IO_RGB_DATA11,
            EXAMPLE_LCD_IO_RGB_DATA12,
            EXAMPLE_LCD_IO_RGB_DATA13,
            EXAMPLE_LCD_IO_RGB_DATA14,
            EXAMPLE_LCD_IO_RGB_DATA15,
        },
        .flags = {
            .fb_in_psram = 1, // Use PSRAM for framebuffer
        },
    };

    // Create a new RGB panel with the specified configuration
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, panel_handle));

    ESP_LOGI(TAG, "Initialize RGB LCD panel");         // Log the initialization of the RGB LCD panel
    ESP_ERROR_CHECK(esp_lcd_panel_init(*panel_handle)); // Initialize the LCD panel

#if CONFIG_EXAMPLE_LCD_TOUCH_CONTROLLER_GT911
    ESP_LOGI(TAG, "Initialize I2C bus");   // Log the initialization of the I2C bus
    ESP_ERROR_CHECK(i2c_master_init());    // Initialize the I2C master
    ESP_LOGI(TAG, "Initialize GPIO");      // Log GPIO initialization
    gpio_init();                           // Initialize GPIO pins
    ESP_LOGI(TAG, "Initialize Touch LCD"); // Log touch LCD initialization
    waveshare_esp32_s3_touch_reset();      // Reset the touch panel

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;                                          // Declare a handle for touch panel I/O
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG(); // Configure I2C for GT911 touch controller
    /* Upstream vendor demo sets 0 here (meant "use bus default" on the older IDF this
     * example targets) — the new i2c_master driver on this IDF version rejects 0 as an
     * invalid SCL frequency at i2c_master_bus_add_device() and aborts, so use the bus rate. */
    tp_io_config.scl_speed_hz = I2C_MASTER_FREQ_HZ;

    ESP_LOGI(TAG, "Initialize I2C panel IO");                                          // Log I2C panel I/O initialization
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_config, &tp_io_handle)); // Create new I2C panel I/O

    ESP_LOGI(TAG, "Initialize touch controller GT911"); // Log touch controller initialization
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = EXAMPLE_LCD_H_RES,                // Set maximum X coordinate
        .y_max = EXAMPLE_LCD_V_RES,                // Set maximum Y coordinate
        .rst_gpio_num = EXAMPLE_PIN_NUM_TOUCH_RST, // GPIO number for reset
        .int_gpio_num = EXAMPLE_PIN_NUM_TOUCH_INT, // GPIO number for interrupt
        .levels = {
            .reset = 0,     // Reset level
            .interrupt = 0, // Interrupt level
        },
        .flags = {
            .swap_xy = 0,  // No swap of X and Y
            .mirror_x = 0, // No mirroring of X
            .mirror_y = 0, // No mirroring of Y
        },
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, touch_handle)); // Create new I2C GT911 touch controller
#endif                                                                                 // CONFIG_EXAMPLE_LCD_TOUCH_CONTROLLER_GT911

    return ESP_OK; // Return success
}

/******************************* Turn on the screen backlight **************************************/
esp_err_t waveshare_rgb_lcd_backlight_on(void)
{
    // Configure CH422G to output mode
    ESP_ERROR_CHECK(i2c_master_init());
    ESP_ERROR_CHECK(i2c_write_byte(s_ch422g_mode_dev, 0x01));

    // Pull the backlight pin high to light the screen backlight
    ESP_ERROR_CHECK(i2c_write_byte(s_ch422g_data_dev, 0x1E));
    return ESP_OK;
}

i2c_master_bus_handle_t waveshare_rgb_lcd_get_i2c_bus(void)
{
    return s_i2c_bus;
}
