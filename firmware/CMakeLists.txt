cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(trae_controller)
set(PARTITION_TABLE_CSV "partitions_custom.csv")
