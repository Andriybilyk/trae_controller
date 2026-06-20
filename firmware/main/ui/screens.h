#pragma once
#include "lvgl.h"

typedef enum {
    SCREEN_NONE = 0,
    SCREEN_MAIN,
    SCREEN_PROGRAMS,
    SCREEN_PROGRAM_EDIT,
    SCREEN_SETTINGS,
    SCREEN_BOOT,
    SCREEN_COUNT
} ScreenId;



void screens_init();
void screens_show(ScreenId id);
void screens_request_show(ScreenId id);
void screens_process_pending();
ScreenId screens_get_current();
void screens_update_current();

void screen_main_create();
void screen_main_update();
void screen_programs_create();
void screen_programs_update();
void screen_program_edit_create(int slot);
void screen_settings_create();
void screen_boot_create();
void screen_boot_update(int percent, const char* status);
