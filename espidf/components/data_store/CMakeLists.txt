idf_component_register(
    SRCS "src/data_store.c"
    INCLUDE_DIRS "include"
    REQUIRES sensor_service nvs_flash freertos
)
