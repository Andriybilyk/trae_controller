#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string>

SemaphoreHandle_t kiln_config_fs_mutex();

std::string kiln_config_load_json_config();
bool kiln_config_save_json_config(const std::string &json);
void kiln_config_restore_json_config_file();
