idf_component_register(
    SRCS "src/display_service.c"
    INCLUDE_DIRS "include"
    REQUIRES
        driver
        esp_lcd
        esp_timer
        freertos
        heap
        sensor_service
        tailnet_service
        time_service
        wifi_station
)
