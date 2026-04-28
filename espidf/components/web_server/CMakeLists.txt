idf_component_register(
    SRCS "src/web_server.c"
    INCLUDE_DIRS "include"
    EMBED_TXTFILES "dashboard.html"
    EMBED_FILES "assets/sensirion_logo.png" "assets/bosch_logo.svg"
    REQUIRES data_store sensor_service tailnet_service time_service wifi_station esp_http_server esp_timer heap app_update
)
