#include "ui/ui_backend.h"

#include "esp_log.h"

extern "C" void lvgl_ui_run(void);

static const char *TAG = "UI_BACKEND";

void ui_backend_run(void) {
    ESP_LOGI(TAG, "UI backend selected: LVGL");
    lvgl_ui_run();
}
