idf_component_register(
    SRCS "src/web_server.c"
    INCLUDE_DIRS "include"
    EMBED_TXTFILES "dashboard.html"
    REQUIRES data_store sensor_service tailnet_service wifi_station esp_http_server esp_timer heap
)
