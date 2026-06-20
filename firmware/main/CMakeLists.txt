set(UI_GEN_DIR "${CMAKE_CURRENT_LIST_DIR}/../../src/ui")

file(GLOB_RECURSE MAIN_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/app/*.c"
    "${CMAKE_CURRENT_LIST_DIR}/app/*.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/drivers/*.c"
    "${CMAKE_CURRENT_LIST_DIR}/drivers/*.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/net/*.c"
    "${CMAKE_CURRENT_LIST_DIR}/net/*.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/ui/screens/*.cpp"
)

list(APPEND MAIN_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/ui/lvgl_backend.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/ui/ui_backend.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/ui/lv_font_pt_sans_10.c"
)

file(GLOB_RECURSE UI_GEN_SRCS
    "${UI_GEN_DIR}/*.c"
)

file(GLOB_RECURSE UI_GEN_EXCLUDE
    "${UI_GEN_DIR}/images/*.c"
    "${UI_GEN_DIR}/images.c"
    "${UI_GEN_DIR}/fonts/font_*.c"
)

list(REMOVE_ITEM UI_GEN_SRCS ${UI_GEN_EXCLUDE})

list(APPEND MAIN_SRCS ${UI_GEN_SRCS})

file(GLOB PICO_RES_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/ui/picopixel/images/*.c"
    "${CMAKE_CURRENT_LIST_DIR}/ui/picopixel/fonts/font_*.c"
)

list(APPEND MAIN_SRCS
    ${PICO_RES_SRCS}
    "${CMAKE_CURRENT_LIST_DIR}/ui/ui_images_bridge.c"
)

set(MAIN_REQUIRES
    littlefs nvs_flash esp_wifi esp_http_server esp_event esp_timer lwip cjson driver esp_driver_i2c esp_driver_ledc esp_driver_tsens kiln_control kiln_config app_update mbedtls esp_system mqtt esp_mm esp_lvgl_port esp_lcd_st7701 esp_lcd_touch_gt911
)
if(IDF_TARGET STREQUAL "esp32p4")
    list(APPEND MAIN_REQUIRES esp32_p4_function_ev_board)
endif()

idf_component_register(
    SRCS ${MAIN_SRCS}
    INCLUDE_DIRS "include" "${UI_GEN_DIR}" "${CMAKE_CURRENT_LIST_DIR}/ui"
    WHOLE_ARCHIVE
    REQUIRES ${MAIN_REQUIRES}
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC LV_LVGL_H_INCLUDE_SIMPLE)

idf_component_get_property(LVGL_COMP_LIB lvgl__lvgl COMPONENT_LIB)
if(LVGL_COMP_LIB)
    target_include_directories(${LVGL_COMP_LIB} PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
endif()
