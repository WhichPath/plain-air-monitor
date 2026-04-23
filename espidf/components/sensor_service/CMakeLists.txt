idf_component_register(
    SRCS "src/sensor_service.c"
    INCLUDE_DIRS "include"
    REQUIRES sps30 esp_timer freertos heap
)
