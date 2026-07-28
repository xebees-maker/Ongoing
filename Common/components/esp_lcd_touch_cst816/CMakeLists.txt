idf_component_register(SRCS "esp_lcd_touch_cst816.c"
                    INCLUDE_DIRS "include"
                    REQUIRES "esp_lcd" "driver" "esp_driver_i2c" "espressif__esp_lcd_touch" "io_expander_tca9554")
