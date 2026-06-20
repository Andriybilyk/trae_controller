#include "theme.h"
#include "esp_lvgl_port.h"

void theme_init() {
    lv_theme_t* th = lv_theme_default_init(
        lv_disp_get_default(),
        COL_ACCENT,
        COL_ACCENT,
        true,
        &lv_font_montserrat_14
    );
    if (th) {
        lv_disp_set_theme(lv_disp_get_default(), th);
    }
}
