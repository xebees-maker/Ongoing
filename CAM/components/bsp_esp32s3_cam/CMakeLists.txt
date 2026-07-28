idf_component_register(SRCS "bsp_esp32s3_cam.c"
                    INCLUDE_DIRS "include"
                    REQUIRES "esp_driver_gpio" "esp_driver_i2c" "log" "io_expander_ch32v003")
