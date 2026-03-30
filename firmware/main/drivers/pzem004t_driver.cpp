#include "drivers/pzem004t_driver.h"

static pzem004t_reading_t s_last_reading = {0.0f, 0.0f, 0.0f, false};

void pzem004t_init(void) {
    s_last_reading.voltage = 0.0f;
    s_last_reading.current = 0.0f;
    s_last_reading.power = 0.0f;
    s_last_reading.ok = false;
}

bool pzem004t_get_reading(pzem004t_reading_t *out) {
    if (!out) return false;
    *out = s_last_reading;
    return out->ok;
}
