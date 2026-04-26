idf_component_register(
    SRCS "src/data_store.c"
    INCLUDE_DIRS "include"
    REQUIRES sensor_service time_service esp_partition esp_rom freertos heap
)
