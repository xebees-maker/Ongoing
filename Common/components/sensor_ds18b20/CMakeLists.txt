idf_component_register(
    SRCS "ds18b20_sensor.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio log
    PRIV_REQUIRES espressif__ds18b20 espressif__onewire_bus
)
