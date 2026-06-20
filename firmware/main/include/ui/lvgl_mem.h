#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *tc_lvgl_mem_alloc(size_t size);
void tc_lvgl_mem_free(void *ptr);
void *tc_lvgl_mem_realloc(void *ptr, size_t size);

#ifdef __cplusplus
}
#endif

