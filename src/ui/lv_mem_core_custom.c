#include "lvgl.h"

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

#include "ui/lvgl_mem.h"

void lv_mem_init(void) {
    return;
}

void lv_mem_deinit(void) {
    return;
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) {
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool) {
    LV_UNUSED(pool);
    return;
}

void *lv_malloc_core(size_t size) {
    return tc_lvgl_mem_alloc(size);
}

void *lv_realloc_core(void *p, size_t new_size) {
    return tc_lvgl_mem_realloc(p, new_size);
}

void lv_free_core(void *p) {
    tc_lvgl_mem_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) {
    if (!mon_p) return;
    lv_memzero(mon_p, sizeof(*mon_p));
}

lv_result_t lv_mem_test_core(void) {
    return LV_RESULT_OK;
}

#endif
