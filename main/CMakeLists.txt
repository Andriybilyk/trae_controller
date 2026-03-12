idf_component_register(SRCS "LGFX_Setup.cpp"
                    "main.cpp"
                    "thermal_control.cpp"
                    "ui_manager.cpp"
                    "wifi_connection.cpp"
                    "wifi_server.cpp"
                    "dns_server.cpp"
INCLUDE_DIRS "include"
REQUIRES lvgl arduinojson littlefs nvs_flash esp_wifi esp_http_server esp_event esp_timer bt lovyangfx lwip)
