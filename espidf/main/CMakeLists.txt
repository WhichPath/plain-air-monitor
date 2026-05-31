set(PM_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(PM_CREDENTIALS_FILE "${CMAKE_SOURCE_DIR}/sdkconfig.credentials")
file(MAKE_DIRECTORY "${PM_GENERATED_DIR}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PM_CREDENTIALS_FILE}")

function(pm_extract_credential key fallback out_var)
    if(EXISTS "${PM_CREDENTIALS_FILE}")
        file(STRINGS "${PM_CREDENTIALS_FILE}" PM_CREDENTIAL_LINES REGEX "^${key}=")
        list(LENGTH PM_CREDENTIAL_LINES PM_CREDENTIAL_LINE_COUNT)
        if(PM_CREDENTIAL_LINE_COUNT GREATER 0)
            list(GET PM_CREDENTIAL_LINES 0 PM_CREDENTIAL_LINE)
            string(REGEX REPLACE "^${key}=\"(.*)\"[ \t\r]*$" "\\1" PM_VALUE "${PM_CREDENTIAL_LINE}")
            string(REPLACE "\\" "\\\\" PM_VALUE "${PM_VALUE}")
            string(REPLACE "\"" "\\\"" PM_VALUE "${PM_VALUE}")
            set(${out_var} "\"${PM_VALUE}\"" PARENT_SCOPE)
            return()
        endif()
    endif()
    set(${out_var} "${fallback}" PARENT_SCOPE)
endfunction()

pm_extract_credential("CONFIG_ML_WIFI_SSID" "CONFIG_ML_WIFI_SSID" PM_WIFI_SSID)
pm_extract_credential("CONFIG_ML_WIFI_PASSWORD" "CONFIG_ML_WIFI_PASSWORD" PM_WIFI_PASSWORD)
pm_extract_credential("CONFIG_ML_TAILSCALE_AUTH_KEY" "CONFIG_ML_TAILSCALE_AUTH_KEY" PM_TAILSCALE_AUTH_KEY)
pm_extract_credential("CONFIG_ML_DEVICE_NAME" "CONFIG_ML_DEVICE_NAME" PM_DEVICE_NAME)

configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/pm_credentials.h.in"
    "${PM_GENERATED_DIR}/pm_credentials.h"
    @ONLY
)

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "." "${PM_GENERATED_DIR}"
    REQUIRES
        data_store
        display_service
        driver
        nvs_flash
        runtime_config
        sensor_service
        tailnet_service
        time_service
        web_server
        wifi_station
)
