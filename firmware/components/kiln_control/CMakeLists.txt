idf_component_register(
    SRCS "thermal_control.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES driver esp_timer cjson freertos kiln_config kiln_hal_heater kiln_safety
)
