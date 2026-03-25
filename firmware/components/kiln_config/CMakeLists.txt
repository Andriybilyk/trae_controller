idf_component_register(
    SRCS
        "config_store.cpp"
        "fs_utils.cpp"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_gpio
    PRIV_REQUIRES nvs_flash cjson
)
