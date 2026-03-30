#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float voltage;
    float current;
    float power;
    bool ok;
} pzem004t_reading_t;

void pzem004t_init(void);
bool pzem004t_get_reading(pzem004t_reading_t *out);

#ifdef __cplusplus
}
#endif
