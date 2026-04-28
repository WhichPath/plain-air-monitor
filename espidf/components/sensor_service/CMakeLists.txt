idf_component_register(
    SRCS "src/sensor_service.c"
    INCLUDE_DIRS "include"
    REQUIRES bmp581 scd41 sgp41 sps30 sht45 esp_timer freertos heap
)
