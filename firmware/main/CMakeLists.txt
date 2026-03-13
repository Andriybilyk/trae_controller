idf_component_register(SRC_DIRS "."
                    INCLUDE_DIRS "include"
                    REQUIRES lvgl littlefs nvs_flash esp_wifi esp_http_server esp_event esp_timer lwip cjson driver)
