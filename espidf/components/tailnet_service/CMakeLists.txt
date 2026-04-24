idf_component_register(
    SRCS "src/tailnet_service.c"
    INCLUDE_DIRS "include"
    REQUIRES microlink wifi_station freertos esp_timer
)
