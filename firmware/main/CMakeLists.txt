idf_component_register(
                    SRC_DIRS "app" "drivers" "net" "ui"
                    INCLUDE_DIRS "include"
                    WHOLE_ARCHIVE
                    REQUIRES slint_ui littlefs nvs_flash esp_wifi esp_http_server esp_event esp_timer lwip cjson driver esp_driver_i2c esp_driver_ledc esp_driver_tsens kiln_control kiln_config app_update mbedtls esp_system mqtt
)
