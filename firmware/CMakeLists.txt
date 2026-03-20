cmake_minimum_required(VERSION 3.16)

# Force LittleFS component to process our data folder
set(EXTRA_COMPONENT_DIRS "components" "slint_ui" "managed_components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(trae_controller)

littlefs_create_partition_image(storage data)
