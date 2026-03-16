#include "ui/ui_manager.h"

#include "drivers/display_driver.h"
#include "net/wifi_server.h"

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

extern const lv_font_t lv_font_pt_sans_20;
extern const lv_font_t lv_font_pt_sans_14;
extern const lv_font_t lv_font_pt_sans_10;

static const char *TAG = "UI";

UIManager uiManager;

volatile uint32_t g_lvgl_timer_count = 0;

// LVGL draw buffer (10 lines)
static uint8_t s_lvgl_buf1[480 * 10 * 2]; // RGB565

static lv_obj_t *listContainer = nullptr;

static uint64_t s_last_clock_update_ms = 0;
static uint64_t s_last_lvgl_tick_ms = 0;

static std::string read_file_to_string(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return {};
    }
    std::string out;
    out.resize((size_t)size);
    const size_t r = fread(out.data(), 1, out.size(), f);
    fclose(f);
    out.resize(r);
    return out;
}

static bool write_string_to_file(const char *path, const std::string &data) {
    const std::string tmp = std::string(path) + ".tmp";
    FILE *f = fopen(tmp.c_str(), "w");
    if (!f) return false;
    const size_t w = fwrite(data.data(), 1, data.size(), f);
    fflush(f);
    const int fd = fileno(f);
    if (fd >= 0) (void)fsync(fd);
    fclose(f);
    if (w != data.size()) {
        (void)unlink(tmp.c_str());
        return false;
    }
    if (rename(tmp.c_str(), path) != 0) {
        (void)unlink(tmp.c_str());
        return false;
    }
    return true;
}

static void lvgl_test_timer_cb(lv_timer_t *t) {
    (void)t;
    g_lvgl_timer_count = g_lvgl_timer_count + 1;
}

static void hide_stop_modal(UIManager *ui) {
    if (!ui || !ui->modalStopConfirm) return;
    lv_obj_del(ui->modalStopConfirm);
    ui->modalStopConfirm = nullptr;
}

static void stop_modal_cancel_cb(lv_event_t *e) {
    UIManager *ui = (UIManager *)lv_event_get_user_data(e);
    hide_stop_modal(ui);
}

static void stop_modal_confirm_cb(lv_event_t *e) {
    UIManager *ui = (UIManager *)lv_event_get_user_data(e);
    if (ui) ui->stopFiring();
    hide_stop_modal(ui);
}

static void show_stop_modal(UIManager *ui) {
    if (!ui || ui->modalStopConfirm) return;

    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    ui->modalStopConfirm = overlay;
    lv_obj_set_size(overlay, 480, 320);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_90, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(overlay);
    lv_obj_set_size(card, 360, 220);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x0B0B0B), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x3F3F46), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_pad_all(card, 18, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *iconBg = lv_obj_create(card);
    lv_obj_set_size(iconBg, 64, 64);
    lv_obj_set_style_bg_color(iconBg, lv_color_hex(0x7F1D1D), 0);
    lv_obj_set_style_bg_opa(iconBg, LV_OPA_60, 0);
    lv_obj_set_style_radius(iconBg, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(iconBg, 0, 0);
    lv_obj_align(iconBg, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_clear_flag(iconBg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *icon = lv_label_create(iconBg);
    lv_label_set_text(icon, LV_SYMBOL_STOP);
    lv_obj_set_style_text_color(icon, lv_color_hex(0xEF4444), 0);
    lv_obj_center(icon);

    const bool isUa = ui->isUkrainian();
    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, isUa ? "Зупинити випал?" : "STOP FIRING?");
    if (isUa) lv_label_set_text(title, "Зупинити випал?");
    lv_obj_set_style_text_font(title, &lv_font_pt_sans_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 78);

    lv_obj_t *body = lv_label_create(card);
    lv_label_set_text(body, isUa ? "Ви впевнені, що хочете зупинити процес?" : "Are you sure you want to abort the process?");
    if (isUa) lv_label_set_text(body, "Ви впевнені, що хочете зупинити процес?");
    lv_obj_set_style_text_font(body, &lv_font_pt_sans_14, 0);
    lv_obj_set_style_text_color(body, lv_color_hex(0xA1A1AA), 0);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 114);

    lv_obj_t *btnCancel = lv_btn_create(card);
    lv_obj_set_size(btnCancel, 150, 60);
    lv_obj_align(btnCancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(btnCancel, lv_color_hex(0x27272A), 0);
    lv_obj_set_style_radius(btnCancel, 14, 0);
    lv_obj_add_event_cb(btnCancel, stop_modal_cancel_cb, LV_EVENT_CLICKED, ui);
    lv_obj_t *lblCancel = lv_label_create(btnCancel);
    lv_label_set_text(lblCancel, isUa ? "Скасувати" : "CANCEL");
    if (isUa) lv_label_set_text(lblCancel, "Скасувати");
    lv_obj_set_style_text_font(lblCancel, &lv_font_pt_sans_20, 0);
    lv_obj_center(lblCancel);

    lv_obj_t *btnStop = lv_btn_create(card);
    lv_obj_set_size(btnStop, 150, 60);
    lv_obj_align(btnStop, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(btnStop, lv_color_hex(0xDC2626), 0);
    lv_obj_set_style_radius(btnStop, 14, 0);
    lv_obj_add_event_cb(btnStop, stop_modal_confirm_cb, LV_EVENT_CLICKED, ui);
    lv_obj_t *lblStop = lv_label_create(btnStop);
    lv_label_set_text(lblStop, isUa ? "Зупинити" : "STOP");
    if (isUa) lv_label_set_text(lblStop, "Зупинити");
    lv_obj_set_style_text_font(lblStop, &lv_font_pt_sans_20, 0);
    lv_obj_center(lblStop);
}

static void update_clock_label() {
    if (!uiManager.labelClock) return;

    // No RTC here: show uptime as HH:MM.
    const uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    const uint32_t hh = (uptime_s / 3600U) % 100U;
    const uint32_t mm = (uptime_s / 60U) % 60U;

    char t[8];
    snprintf(t, sizeof(t), "%02u:%02u", (unsigned)hh, (unsigned)mm);
    lv_label_set_text(uiManager.labelClock, t);
}


static void btn_event_handler(lv_event_t *e) {
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    UIManager *ui = (UIManager *)lv_event_get_user_data(e);

    if (obj == ui->btnStart) {
        KilnState state = thermalCtrl.getState();
        if (state.isFiring) show_stop_modal(ui);
        else ui->startFiring();
        return;
    }

    if (obj == ui->btnPrograms) {
        ui->showPrograms();
        return;
    }

    if (obj == ui->btnSchedules) {
        ui->showPrograms();
        return;
    }

    if (obj == ui->btnEdit) {
        ui->showEditor();
        return;
    }

    if (obj == ui->btnBack) {
        ui->showMain();
        return;
    }

    if (obj == ui->btnEditorBack) {
        ui->showPrograms();
        return;
    }

    if (obj == ui->btnEditorSave) {
        (void)ui->saveEditedSchedule();
        ui->showMain();
        return;
    }

    if (obj == ui->btnLang) {
        ui->toggleLanguage();
        return;
    }
}

static const char *tr(bool ua, const char *en, const char *uk) {
    return ua ? uk : en;
}

#if 0
static void apply_language_labels(UIManager *ui) {
    if (!ui) return;
    const bool isUa = ui->isUkrainian();
    if (ui->langLabel) lv_label_set_text(ui->langLabel, isUa ? "UA" : "EN");
    if (ui->labelProgramMeta) lv_label_set_text(ui->labelProgramMeta, tr(isUa, "Tap to select", "Торкніться щоб вибрати"));
    if (ui->lblCurrent) lv_label_set_text(ui->lblCurrent, tr(isUa, "CURRENT TEMP", "Поточна температура"));
    if (ui->lblTarget) lv_label_set_text(ui->lblTarget, tr(isUa, "TARGET", "Цільова"));
    if (ui->lblRemain) lv_label_set_text(ui->lblRemain, tr(isUa, "TIME LEFT", "Залишилось часу"));
    if (ui->lblSelectProgram) lv_label_set_text(ui->lblSelectProgram, tr(isUa, "SELECT PROGRAM", "Вибір програми"));
    if (ui->lblStart) lv_label_set_text(ui->lblStart, tr(isUa, LV_SYMBOL_PLAY "\nSTART", LV_SYMBOL_PLAY "\nСтарт"));
    if (ui->lblBack) lv_label_set_text(ui->lblBack, tr(isUa, "BACK", "Назад"));
    if (ui->lblProgramsTitle) lv_label_set_text(ui->lblProgramsTitle, tr(isUa, "LIBRARY", "Бібліотека"));
    if (ui->labelProgramName && ui->selectedProgramName.empty()) {
        lv_label_set_text(ui->labelProgramName, tr(isUa, "None", "Немає"));
    }
    if (ui->lblSchedules) lv_label_set_text(ui->lblSchedules, tr(isUa, LV_SYMBOL_SETTINGS "\nPrograms", LV_SYMBOL_SETTINGS "\nПрограми"));
    if (ui->lblEditorBack) lv_label_set_text(ui->lblEditorBack, tr(isUa, "BACK", "Назад"));
    if (ui->lblEditorSave) lv_label_set_text(ui->lblEditorSave, tr(isUa, "SAVE", "Зберегти"));
    if (ui->lblEditorTitle) lv_label_set_text(ui->lblEditorTitle, tr(isUa, "EDITOR", "Редактор"));
}

static void update_status_v2(UIManager *ui, const KilnState &state) {
    if (!ui) return;
    const bool isUa = ui->isUkrainian();

    if (ui->labelWifi) {
        const bool ok = !wifiServer.isAPMode;
        lv_label_set_text(ui->labelWifi, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_color(ui->labelWifi, ok ? lv_color_hex(0x10B981) : lv_color_hex(0x52525B), 0);
    }

    if (ui->labelTemp) {
        char t[16];
        snprintf(t, sizeof(t), "%.1f", state.currentTemp);
        lv_label_set_text(ui->labelTemp, t);
    }

    if (ui->labelTarget) {
        char t[16];
        if (state.targetTemp > 0.0f) snprintf(t, sizeof(t), "%.0fC", state.targetTemp);
        else snprintf(t, sizeof(t), "--");
        lv_label_set_text(ui->labelTarget, t);
    }

    if (ui->labelStatusBadge) {
        const char *s = "IDLE";
        switch (state.status) {
            case KILN_IDLE: s = "IDLE"; break;
            case KILN_RAMP: s = "RAMP"; break;
            case KILN_HOLD: s = "HOLD"; break;
            case KILN_COMPLETE: s = "COMPLETE"; break;
            case KILN_ERROR: s = "ERROR"; break;
            default: s = state.isFiring ? "FIRING" : "IDLE"; break;
        }
        if (!state.errorMsg.empty() && state.errorMsg != "System Boot" && strcmp(s, "IDLE") == 0) s = "ERROR";
        lv_label_set_text(ui->labelStatusBadge, s);

        lv_color_t bg = lv_color_hex(0x27272A);
        if (strcmp(s, "ERROR") == 0) bg = lv_color_hex(0xEF4444);
        else if (strcmp(s, "IDLE") != 0) bg = lv_color_hex(0x10B981);
        lv_obj_set_style_bg_color(ui->labelStatusBadge, bg, 0);
        lv_obj_set_style_text_color(ui->labelStatusBadge, lv_color_hex(strcmp(s, "IDLE") != 0 && strcmp(s, "ERROR") != 0 ? 0x000000 : 0xFFFFFF), 0);
    }

    if (ui->barProgress) {
        int v = 0;
        if (state.targetTemp > 0.0f && state.currentTemp > 0.0f) {
            v = (int)std::min(100.0f, (state.currentTemp / state.targetTemp) * 100.0f);
        }
        lv_bar_set_value(ui->barProgress, v, LV_ANIM_OFF);
    }

    if (ui->labelTimeRemaining) {
        if (state.isFiring && state.timeRemaining >= 0) {
            const int hours = state.timeRemaining / 60;
            const int mins = state.timeRemaining % 60;
            char trBuf[16];
            snprintf(trBuf, sizeof(trBuf), "%02d:%02d", hours, mins);
            lv_label_set_text(ui->labelTimeRemaining, trBuf);
        } else {
            lv_label_set_text(ui->labelTimeRemaining, "--:--");
        }
    }

    if (state.isFiring) {
        if (ui->lblSelectProgram) lv_label_set_text(ui->lblSelectProgram, tr(isUa, "STEP", "Крок"));
        if (ui->labelProgramName) {
            char stepBuf[24];
            snprintf(stepBuf, sizeof(stepBuf), "%d / %d", state.currentStep, state.totalSteps);
            lv_obj_set_style_text_font(ui->labelProgramName, &lv_font_montserrat_48, 0);
            lv_label_set_text(ui->labelProgramName, stepBuf);
        }
        if (ui->labelProgramMeta) {
            char meta[64];
            if (state.status == KILN_HOLD) {
                snprintf(meta, sizeof(meta), "%s %.0fC", isUa ? "Витримка" : "Hold", state.targetTemp);
            } else {
                snprintf(meta, sizeof(meta), "%s %.0fC", isUa ? "Нагрів до" : "Ramp to", state.targetTemp);
            }
            lv_label_set_text(ui->labelProgramMeta, meta);
        }
    } else {
        if (ui->lblSelectProgram) lv_label_set_text(ui->lblSelectProgram, tr(isUa, "SELECT PROGRAM", "Вибір програми"));
        if (ui->labelProgramName) {
            lv_obj_set_style_text_font(ui->labelProgramName, &lv_font_pt_sans_20, 0);
            if (!ui->selectedProgramName.empty()) {
                lv_label_set_text(ui->labelProgramName, ui->selectedProgramName.c_str());
            } else {
                lv_label_set_text(ui->labelProgramName, tr(isUa, "Select Schedule", "Виберіть програму"));
            }
        }
        if (ui->labelProgramMeta) {
            if (state.totalSteps > 0) {
                char meta[64];
                snprintf(meta, sizeof(meta), "%d %s", state.totalSteps, isUa ? "кроків" : "segments");
                lv_label_set_text(ui->labelProgramMeta, meta);
            } else {
                lv_label_set_text(ui->labelProgramMeta, tr(isUa, "Tap to select", "Торкніться щоб вибрати"));
            }
        }
    }

    if (ui->faultBanner && ui->labelFault) {
        const SafetyStats safety = thermalCtrl.getSafetyStats();
        if (safety.faultActive) {
            lv_obj_clear_flag(ui->faultBanner, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(ui->labelFault, safety.faultReason[0] ? safety.faultReason : "FAULT");
        } else {
            lv_obj_add_flag(ui->faultBanner, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (ui->btnStart) {
        lv_obj_t *lbl = lv_obj_get_child(ui->btnStart, 0);
        if (state.isFiring) {
            lv_obj_set_size(ui->btnStart, 216, 80);
            lv_obj_set_style_bg_color(ui->btnStart, lv_color_hex(0xDC2626), 0);
            if (lbl) lv_label_set_text(lbl, tr(isUa, "STOP", "Стоп"));
            if (ui->btnSchedules) lv_obj_add_flag(ui->btnSchedules, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_set_size(ui->btnStart, 104, 80);
            if (ui->btnSchedules) lv_obj_clear_flag(ui->btnSchedules, LV_OBJ_FLAG_HIDDEN);
            if (state.totalSteps > 0) {
                lv_obj_set_style_bg_color(ui->btnStart, lv_color_hex(0x10B981), 0);
                if (lbl) lv_label_set_text(lbl, tr(isUa, LV_SYMBOL_PLAY "\nSTART", LV_SYMBOL_PLAY "\nСтарт"));
            } else {
                lv_obj_set_style_bg_color(ui->btnStart, lv_color_hex(0x27272A), 0);
                if (lbl) lv_label_set_text(lbl, tr(isUa, LV_SYMBOL_PLAY "\nSELECT", LV_SYMBOL_PLAY "\nВибір"));
            }
        }
    }
}

static void show_programs_v2(UIManager *ui) {
    if (!ui || !listContainer) return;
    const bool isUa = ui->isUkrainian();

    lv_obj_clean(listContainer);

    const std::string file = read_file_to_string("/littlefs/schedules.json");
    if (file.empty()) {
        lv_obj_t *lbl = lv_label_create(listContainer);
        lv_label_set_text(lbl, tr(isUa, "No schedules", "Немає програм"));
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
        lv_scr_load(ui->screenPrograms);
        return;
    }

    cJSON *arr = cJSON_Parse(file.c_str());
    if (!arr || !cJSON_IsArray(arr)) {
        if (arr) cJSON_Delete(arr);
        lv_obj_t *lbl = lv_label_create(listContainer);
        lv_label_set_text(lbl, tr(isUa, "JSON error", "Помилка даних"));
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
        lv_scr_load(ui->screenPrograms);
        return;
    }

    int idx = 0;
    cJSON *obj = nullptr;
    cJSON_ArrayForEach(obj, arr) {
        const cJSON *nameItem = cJSON_GetObjectItem(obj, "name");
        const char *name = (cJSON_IsString(nameItem) && nameItem->valuestring) ? nameItem->valuestring : tr(isUa, "Unnamed", "Без назви");

        const cJSON *stepsArr = cJSON_GetObjectItem(obj, "steps");
        if (!cJSON_IsArray(stepsArr)) stepsArr = cJSON_GetObjectItem(obj, "segments");
        int steps = cJSON_IsArray(stepsArr) ? cJSON_GetArraySize(stepsArr) : 0;
        const bool isSelected = (ui->selectedProgramName == name);

        lv_obj_t *row = lv_btn_create(listContainer);
        lv_obj_set_size(row, 460, 56);
        lv_obj_set_style_bg_color(row, isSelected ? lv_color_hex(0x1F2937) : lv_color_hex(0x0F0F0F), 0);
        lv_obj_set_style_border_color(row, isSelected ? lv_color_hex(0x10B981) : lv_color_hex(0x27272A), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_add_event_cb(row, program_edit_handler, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

        lv_obj_t *lblName = lv_label_create(row);
        lv_label_set_text(lblName, name);
        lv_obj_set_style_text_color(lblName, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(lblName, &lv_font_pt_sans_14, 0);
        lv_obj_align(lblName, LV_ALIGN_LEFT_MID, 12, -8);

        lv_obj_t *lblSteps = lv_label_create(row);
        lv_label_set_text_fmt(lblSteps, "%d %s", steps, isUa ? "кроків" : "segments");
        lv_obj_set_style_text_color(lblSteps, lv_color_hex(0xA1A1AA), 0);
        lv_obj_set_style_text_font(lblSteps, &lv_font_pt_sans_10, 0);
        lv_obj_align(lblSteps, LV_ALIGN_LEFT_MID, 12, 10);

        lv_obj_t *btnSelect = lv_btn_create(row);
        lv_obj_set_size(btnSelect, 38, 38);
        lv_obj_set_style_bg_color(btnSelect, isSelected ? lv_color_hex(0x10B981) : lv_color_hex(0x27272A), 0);
        lv_obj_set_style_radius(btnSelect, 19, 0);
        lv_obj_align(btnSelect, LV_ALIGN_RIGHT_MID, -10, 0);
        lv_obj_add_event_cb(btnSelect, program_select_handler, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
        lv_obj_t *lblSel = lv_label_create(btnSelect);
        lv_label_set_text(lblSel, LV_SYMBOL_PLAY);
        lv_obj_set_style_text_color(lblSel, isSelected ? lv_color_hex(0x000000) : lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(lblSel);

        idx++;
    }

    cJSON_Delete(arr);
    lv_scr_load(ui->screenPrograms);
}

#endif

void UIManager::applyLanguageLabels() {
    const bool isUa = isUkrainian();
    if (langLabel) lv_label_set_text(langLabel, isUa ? "UA" : "EN");
    if (lblCurrent) lv_label_set_text(lblCurrent, tr(isUa, "CURRENT TEMP", "Поточна температура"));
    if (lblTarget) lv_label_set_text(lblTarget, tr(isUa, "TARGET", "Цільова"));
    if (lblRemain) lv_label_set_text(lblRemain, tr(isUa, "TIME LEFT", "Залишилось часу"));
    if (lblSelectProgram) lv_label_set_text(lblSelectProgram, tr(isUa, "SELECT PROGRAM", "Вибір програми"));
    if (lblStart) lv_label_set_text(lblStart, tr(isUa, "▶\nSTART", "▶\nСтарт"));
    if (lblBack) lv_label_set_text(lblBack, tr(isUa, "BACK", "Назад"));
    if (lblProgramsTitle) lv_label_set_text(lblProgramsTitle, tr(isUa, "LIBRARY", "Бібліотека"));
    if (labelProgramName && selectedProgramName.empty()) {
        lv_label_set_text(labelProgramName, tr(isUa, "Select Schedule", "Виберіть програму"));
    }
    if (labelProgramMeta) lv_label_set_text(labelProgramMeta, tr(isUa, "Tap to select", "Торкніться щоб вибрати"));
    if (lblSchedules) lv_label_set_text(lblSchedules, tr(isUa, "PROGRAMS", "ПРОГРАМИ"));
    if (lblEditorBack) lv_label_set_text(lblEditorBack, tr(isUa, "BACK", "Назад"));
    if (lblEditorSave) lv_label_set_text(lblEditorSave, tr(isUa, "SAVE", "Зберегти"));
    if (lblEditorTitle) lv_label_set_text(lblEditorTitle, tr(isUa, "EDITOR", "Редактор"));
}

void UIManager::updateStatusV2(const KilnState &state) {
    const bool isUa = isUkrainian();

    if (labelWifi) {
        const bool ok = !wifiServer.isAPMode;
        lv_label_set_text(labelWifi, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_color(labelWifi, ok ? lv_color_hex(0x10B981) : lv_color_hex(0x52525B), 0);
    }

    if (labelTemp) {
        char t[16];
        snprintf(t, sizeof(t), "%.1f", state.currentTemp);
        lv_label_set_text(labelTemp, t);
    }

    if (labelTarget) {
        char t[16];
        if (state.targetTemp > 0.0f) snprintf(t, sizeof(t), "%.0f°C", state.targetTemp);
        else snprintf(t, sizeof(t), "--");
        lv_label_set_text(labelTarget, t);
    }

    if (labelStatusBadge) {
        const char *s = "IDLE";
        switch (state.status) {
            case KILN_IDLE: s = "IDLE"; break;
            case KILN_RAMP: s = "RAMP"; break;
            case KILN_HOLD: s = "HOLD"; break;
            case KILN_COMPLETE: s = "COMPLETE"; break;
            case KILN_ERROR: s = "ERROR"; break;
            default: s = state.isFiring ? "FIRING" : "IDLE"; break;
        }
        if (!state.errorMsg.empty() && state.errorMsg != "System Boot" && strcmp(s, "IDLE") == 0) s = "ERROR";
        lv_label_set_text(labelStatusBadge, s);

        lv_color_t bg = lv_color_hex(0x27272A);
        if (strcmp(s, "ERROR") == 0) bg = lv_color_hex(0xEF4444);
        else if (strcmp(s, "IDLE") != 0) bg = lv_color_hex(0x10B981);
        lv_obj_set_style_bg_color(labelStatusBadge, bg, 0);
        lv_obj_set_style_text_color(labelStatusBadge, lv_color_hex(strcmp(s, "IDLE") != 0 && strcmp(s, "ERROR") != 0 ? 0x000000 : 0xFFFFFF), 0);
    }

    if (barProgress) {
        int v = 0;
        if (state.targetTemp > 0.0f && state.currentTemp > 0.0f) {
            v = (int)std::min(100.0f, (state.currentTemp / state.targetTemp) * 100.0f);
        }
        lv_bar_set_value(barProgress, v, LV_ANIM_OFF);
    }

    if (labelTimeRemaining) {
        if (state.isFiring && state.timeRemaining >= 0) {
            const int hours = state.timeRemaining / 60;
            const int mins = state.timeRemaining % 60;
            char trBuf[16];
            snprintf(trBuf, sizeof(trBuf), "%02d:%02d", hours, mins);
            lv_label_set_text(labelTimeRemaining, trBuf);
        } else {
            lv_label_set_text(labelTimeRemaining, "--:--");
        }
    }

    if (state.isFiring) {
        if (lblSelectProgram) lv_label_set_text(lblSelectProgram, tr(isUa, "STEP", "Крок"));
        if (labelProgramIcon) lv_obj_add_flag(labelProgramIcon, LV_OBJ_FLAG_HIDDEN);
        if (labelProgramName) {
            char stepBuf[24];
            snprintf(stepBuf, sizeof(stepBuf), "%d / %d", state.currentStep, state.totalSteps);
            lv_obj_set_style_text_font(labelProgramName, &lv_font_montserrat_48, 0);
            lv_label_set_text(labelProgramName, stepBuf);
        }
        if (labelProgramMeta) {
            char meta[64];
            if (state.status == KILN_HOLD) {
                snprintf(meta, sizeof(meta), "%s %.0f°C", isUa ? "Витримка" : "Hold", state.targetTemp);
            } else {
                snprintf(meta, sizeof(meta), "%s %.0f°C", isUa ? "Нагрів до" : "Ramp to", state.targetTemp);
            }
            lv_label_set_text(labelProgramMeta, meta);
        }
    } else {
        if (lblSelectProgram) lv_label_set_text(lblSelectProgram, tr(isUa, "SELECT PROGRAM", "Вибір програми"));
        if (labelProgramName) {
            lv_obj_set_style_text_font(labelProgramName, &lv_font_pt_sans_20, 0);
            if (!selectedProgramName.empty()) {
                lv_label_set_text(labelProgramName, selectedProgramName.c_str());
            } else {
                lv_label_set_text(labelProgramName, tr(isUa, "Select Schedule", "Виберіть програму"));
            }
        }
        if (labelProgramMeta) {
            if (state.totalSteps > 0 && !selectedProgramName.empty()) {
                char meta[64];
                snprintf(meta, sizeof(meta), "%d %s", state.totalSteps, isUa ? "кроків" : "segments");
                lv_label_set_text(labelProgramMeta, meta);
            } else {
                lv_label_set_text(labelProgramMeta, tr(isUa, "Tap to select", "Торкніться щоб вибрати"));
            }
        }
        if (labelProgramIcon) {
            if (selectedProgramName.empty()) lv_obj_clear_flag(labelProgramIcon, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(labelProgramIcon, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (faultBanner && labelFault) {
        const SafetyStats safety = thermalCtrl.getSafetyStats();
        if (safety.faultActive) {
            lv_obj_clear_flag(faultBanner, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(labelFault, safety.faultReason[0] ? safety.faultReason : "FAULT");
        } else {
            lv_obj_add_flag(faultBanner, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (btnStart) {
        lv_obj_t *lbl = lv_obj_get_child(btnStart, 0);
        if (state.isFiring) {
            lv_obj_set_size(btnStart, 212, 80);
            lv_obj_set_style_bg_color(btnStart, lv_color_hex(0xDC2626), 0);
            if (lbl) lv_label_set_text(lbl, tr(isUa, "■\nSTOP", "■\nСтоп"));
            if (btnSchedules) lv_obj_add_flag(btnSchedules, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_set_size(btnStart, 102, 80);
            if (btnSchedules) lv_obj_clear_flag(btnSchedules, LV_OBJ_FLAG_HIDDEN);
            if (state.totalSteps > 0) {
                lv_obj_set_style_bg_color(btnStart, lv_color_hex(0x10B981), 0);
                if (lbl) lv_label_set_text(lbl, tr(isUa, "▶\nSTART", "▶\nСтарт"));
            } else {
                lv_obj_set_style_bg_color(btnStart, lv_color_hex(0x27272A), 0);
                if (lbl) lv_label_set_text(lbl, tr(isUa, "▶\nSELECT", "▶\nВибір"));
            }
        }
    }
}

void UIManager::showProgramsV2() {
    if (!listContainer) return;
    const bool isUa = isUkrainian();

    lv_obj_clean(listContainer);

    const std::string file = read_file_to_string("/littlefs/schedules.json");
    if (file.empty()) {
        lv_obj_t *lbl = lv_label_create(listContainer);
        lv_label_set_text(lbl, tr(isUa, "No schedules", "Немає програм"));
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
        lv_scr_load(screenPrograms);
        return;
    }

    cJSON *arr = cJSON_Parse(file.c_str());
    if (!arr || !cJSON_IsArray(arr)) {
        if (arr) cJSON_Delete(arr);
        lv_obj_t *lbl = lv_label_create(listContainer);
        lv_label_set_text(lbl, tr(isUa, "JSON error", "Помилка даних"));
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
        lv_scr_load(screenPrograms);
        return;
    }

    int idx = 0;
    cJSON *obj = nullptr;
    cJSON_ArrayForEach(obj, arr) {
        const cJSON *nameItem = cJSON_GetObjectItem(obj, "name");
        const char *name = (cJSON_IsString(nameItem) && nameItem->valuestring) ? nameItem->valuestring : tr(isUa, "Unnamed", "Без назви");

        const cJSON *stepsArr = cJSON_GetObjectItem(obj, "steps");
        if (!cJSON_IsArray(stepsArr)) stepsArr = cJSON_GetObjectItem(obj, "segments");
        int steps = cJSON_IsArray(stepsArr) ? cJSON_GetArraySize(stepsArr) : 0;
        const bool isSelected = (selectedProgramName == name);

        lv_obj_t *row = lv_btn_create(listContainer);
        lv_obj_set_size(row, 460, 56);
        lv_obj_set_style_bg_color(row, isSelected ? lv_color_hex(0x1F2937) : lv_color_hex(0x0F0F0F), 0);
        lv_obj_set_style_border_color(row, isSelected ? lv_color_hex(0x10B981) : lv_color_hex(0x27272A), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_add_event_cb(row, program_edit_handler, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

        lv_obj_t *lblName = lv_label_create(row);
        lv_label_set_text(lblName, name);
        lv_obj_set_style_text_color(lblName, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(lblName, &lv_font_pt_sans_14, 0);
        lv_obj_align(lblName, LV_ALIGN_LEFT_MID, 12, -8);

        lv_obj_t *lblSteps = lv_label_create(row);
        lv_label_set_text_fmt(lblSteps, "%d %s", steps, isUa ? "кроків" : "segments");
        lv_obj_set_style_text_color(lblSteps, lv_color_hex(0xA1A1AA), 0);
        lv_obj_set_style_text_font(lblSteps, &lv_font_pt_sans_10, 0);
        lv_obj_align(lblSteps, LV_ALIGN_LEFT_MID, 12, 10);

        lv_obj_t *btnSelect = lv_btn_create(row);
        lv_obj_set_size(btnSelect, 38, 38);
        lv_obj_set_style_bg_color(btnSelect, isSelected ? lv_color_hex(0x10B981) : lv_color_hex(0x27272A), 0);
        lv_obj_set_style_radius(btnSelect, 19, 0);
        lv_obj_align(btnSelect, LV_ALIGN_RIGHT_MID, -10, 0);
        lv_obj_add_event_cb(btnSelect, program_select_handler, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
        lv_obj_t *lblSel = lv_label_create(btnSelect);
        lv_label_set_text(lblSel, "▶");
        lv_obj_set_style_text_color(lblSel, isSelected ? lv_color_hex(0x000000) : lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(lblSel);

        idx++;
    }

    cJSON_Delete(arr);
    lv_scr_load(screenPrograms);
}

static bool load_schedule_index(int index, std::string &name_out, std::string &json_out) {
    FILE *f = fopen("/littlefs/schedules.json", "r");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open schedules.json");
        return false;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buffer = (char *)malloc(fsize + 1);
    if (!buffer) {
        fclose(f);
        return false;
    }

    fread(buffer, 1, fsize, f);
    fclose(f);
    buffer[fsize] = 0;

    cJSON *arr = cJSON_Parse(buffer);
    free(buffer);

    if (!arr || !cJSON_IsArray(arr)) {
        ESP_LOGE(TAG, "Failed to parse schedules.json (not an array)");
        if (arr) cJSON_Delete(arr);
        return false;
    }

    cJSON *item = cJSON_GetArrayItem(arr, index);
    if (!item || !cJSON_IsObject(item)) {
        cJSON_Delete(arr);
        return false;
    }

    const cJSON *nameItem = cJSON_GetObjectItem(item, "name");
    if (cJSON_IsString(nameItem) && nameItem->valuestring) {
        name_out = nameItem->valuestring;
    }

    char *rendered = cJSON_PrintUnformatted(item);
    if (rendered) {
        json_out.assign(rendered);
        free(rendered);
    }

    cJSON_Delete(arr);
    return !json_out.empty();
}

static void program_select_handler(lv_event_t *e) {
    int index = (int)(intptr_t)lv_event_get_user_data(e);

    std::string name;
    std::string json;
    if (!load_schedule_index(index, name, json)) return;

    uiManager.selectedProgramName = name;
    uiManager.selectedProgramJson = json;
    thermalCtrl.loadSchedule(json);
    ESP_LOGI(TAG, "Loaded schedule index %d", index);
    uiManager.showMain();
}

static void program_edit_handler(lv_event_t *e) {
    int index = (int)(intptr_t)lv_event_get_user_data(e);

    std::string name;
    std::string json;
    if (!load_schedule_index(index, name, json)) return;

    uiManager.selectedProgramName = name;
    uiManager.selectedProgramJson = json;
    uiManager.showEditor();
}


UIManager::UIManager() {
    lastUpdate = 0;
    selectedProgramName.clear();
    selectedProgramJson.clear();
    labelWifi = nullptr;
    labelTimeRemaining = nullptr;
    labelProgramMeta = nullptr;
    labelProgramIcon = nullptr;
    screenEditor = nullptr;
    btnEdit = nullptr;
    btnSchedules = nullptr;
    lblSchedules = nullptr;
    editorList = nullptr;
    btnEditorSave = nullptr;
    btnEditorBack = nullptr;
    lblEditorSave = nullptr;
    lblEditorBack = nullptr;
    lblEditorTitle = nullptr;
    faultBanner = nullptr;
    labelFault = nullptr;
    btnFaultReset = nullptr;
    modalStopConfirm = nullptr;
}

void UIManager::begin() {
    ESP_LOGI(TAG, "UIManager::begin()");

    display_driver_init();
    lv_init();
    (void)lv_timer_create(lvgl_test_timer_cb, 100, NULL);

    lv_display_t *disp = lv_display_create(480, 320);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, display_driver_lvgl_flush);
    lv_display_set_buffers(disp, s_lvgl_buf1, NULL, sizeof(s_lvgl_buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, display_driver_lvgl_touch_read);
    lv_indev_set_display(indev, disp);

    createScreens();
    showMain();
}

void UIManager::loop() {
    const uint64_t now_ms = esp_timer_get_time() / 1000ULL;
    if (s_last_lvgl_tick_ms == 0) s_last_lvgl_tick_ms = now_ms;
    const uint32_t delta = (uint32_t)std::min<uint64_t>(100, now_ms - s_last_lvgl_tick_ms);
    s_last_lvgl_tick_ms = now_ms;
    lv_tick_inc(delta);
    lv_timer_handler();

    if (now_ms - s_last_clock_update_ms > 1000) {
        s_last_clock_update_ms = now_ms;
        update_clock_label();

        const uint32_t rev = wifiServer.getSchedulesRevision();
        if (rev != lastSchedulesRevision) {
            lastSchedulesRevision = rev;
            if (lv_scr_act() == screenPrograms) {
                showPrograms();
            }
        }
    }
}

void UIManager::createScreens() {
    // --- Main (Dashboard) ---
    screenMain = lv_obj_create(NULL);
    lv_obj_clear_flag(screenMain, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screenMain, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(screenMain, 0, 0);
    lv_obj_set_style_text_font(screenMain, &lv_font_pt_sans_20, 0);
    lv_obj_set_style_text_color(screenMain, lv_color_hex(0xFFFFFF), 0);

    // Header
    lv_obj_t *header = lv_obj_create(screenMain);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(header, 480, 40);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x0B0B0B), 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(header, lv_color_hex(0x27272A), 0);
    lv_obj_set_style_pad_hor(header, 12, 0);
    lv_obj_set_style_pad_ver(header, 6, 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *brandDot = lv_obj_create(header);
    lv_obj_set_size(brandDot, 8, 8);
    lv_obj_set_style_bg_color(brandDot, lv_color_hex(0x10B981), 0);
    lv_obj_set_style_radius(brandDot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(brandDot, 0, 0);
    lv_obj_align(brandDot, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *brand = lv_label_create(header);
    lv_label_set_text(brand, "KILN PRO");
    lv_obj_set_style_text_color(brand, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(brand, &lv_font_pt_sans_14, 0);
    lv_obj_align_to(brand, brandDot, LV_ALIGN_OUT_RIGHT_MID, 6, 0);

    btnLang = lv_btn_create(header);
    lv_obj_set_size(btnLang, 40, 22);
    lv_obj_set_style_bg_color(btnLang, lv_color_hex(0x27272A), 0);
    lv_obj_set_style_radius(btnLang, 6, 0);
    lv_obj_align(btnLang, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(btnLang, btn_event_handler, LV_EVENT_CLICKED, this);
    langLabel = lv_label_create(btnLang);
    lv_obj_set_style_text_font(langLabel, &lv_font_pt_sans_10, 0);
    lv_obj_center(langLabel);

    labelClock = lv_label_create(header);
    lv_label_set_text(labelClock, "--:--");
    lv_obj_set_style_text_color(labelClock, lv_color_hex(0xA1A1AA), 0);
    lv_obj_set_style_text_font(labelClock, &lv_font_pt_sans_10, 0);
    lv_obj_align_to(labelClock, btnLang, LV_ALIGN_OUT_LEFT_MID, -10, 0);

    labelWifi = lv_label_create(header);
    lv_label_set_text(labelWifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(labelWifi, &lv_font_montserrat_14, 0);
    lv_obj_align_to(labelWifi, labelClock, LV_ALIGN_OUT_LEFT_MID, -10, 0);

    // Fault banner area (between header and content)
    faultBanner = lv_obj_create(screenMain);
    lv_obj_clear_flag(faultBanner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(faultBanner, 480, 34);
    lv_obj_align(faultBanner, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_color(faultBanner, lv_color_hex(0x7F1D1D), 0);
    lv_obj_set_style_border_width(faultBanner, 0, 0);
    lv_obj_set_style_pad_hor(faultBanner, 10, 0);
    lv_obj_set_style_pad_ver(faultBanner, 4, 0);
    lv_obj_add_flag(faultBanner, LV_OBJ_FLAG_HIDDEN);

    labelFault = lv_label_create(faultBanner);
    lv_label_set_text(labelFault, "FAULT");
    lv_obj_set_style_text_color(labelFault, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(labelFault, LV_ALIGN_LEFT_MID, 0, 0);

    btnFaultReset = lv_btn_create(faultBanner);
    lv_obj_set_size(btnFaultReset, 90, 26);
    lv_obj_align(btnFaultReset, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(btnFaultReset, lv_color_hex(0xDC2626), 0);
    lv_obj_set_style_radius(btnFaultReset, 8, 0);
    lv_obj_add_event_cb(btnFaultReset, [](lv_event_t *e) {
        (void)e;
        (void)thermalCtrl.clearFault();
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *lblReset = lv_label_create(btnFaultReset);
    lv_label_set_text(lblReset, "RESET");
    lv_obj_center(lblReset);

    // Content container (full height below header)
    lv_obj_t *content = lv_obj_create(screenMain);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(content, 480, 280);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_color(content, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_style_pad_column(content, 8, 0);
    lv_obj_set_layout(content, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);

    // Left column
    lv_obj_t *left = lv_obj_create(content);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(left, 236, 280);
    lv_obj_set_style_bg_color(left, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_side(left, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_width(left, 1, 0);
    lv_obj_set_style_border_color(left, lv_color_hex(0x27272A), 0);
    lv_obj_set_style_pad_all(left, 12, 0);

    labelStatusBadge = lv_label_create(left);
    lv_label_set_text(labelStatusBadge, "IDLE");
    lv_obj_set_style_bg_opa(labelStatusBadge, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(labelStatusBadge, lv_color_hex(0x27272A), 0);
    lv_obj_set_style_radius(labelStatusBadge, 10, 0);
    lv_obj_set_style_text_color(labelStatusBadge, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(labelStatusBadge, &lv_font_pt_sans_10, 0);
    lv_obj_set_style_pad_hor(labelStatusBadge, 8, 0);
    lv_obj_set_style_pad_ver(labelStatusBadge, 4, 0);
    lv_obj_align(labelStatusBadge, LV_ALIGN_TOP_RIGHT, 0, 0);

    lv_obj_t *lblCur = lv_label_create(left);
    lblCurrent = lblCur;
    lv_obj_set_style_text_color(lblCur, lv_color_hex(0x71717A), 0);
    lv_obj_set_style_text_font(lblCur, &lv_font_pt_sans_10, 0);
    lv_obj_align(lblCur, LV_ALIGN_TOP_LEFT, 0, 8);

    labelTemp = lv_label_create(left);
    lv_label_set_text(labelTemp, "25.0");
    lv_obj_set_style_text_color(labelTemp, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(labelTemp, &lv_font_montserrat_48, 0);
    lv_obj_align(labelTemp, LV_ALIGN_TOP_LEFT, 0, 24);

    lv_obj_t *tempUnit = lv_label_create(left);
    lv_label_set_text(tempUnit, "\xC2\xB0""C");
    lv_obj_set_style_text_color(tempUnit, lv_color_hex(0x71717A), 0);
    lv_obj_set_style_text_font(tempUnit, &lv_font_montserrat_14, 0);
    lv_obj_align_to(tempUnit, labelTemp, LV_ALIGN_OUT_RIGHT_BOTTOM, 6, -8);

    lv_obj_t *box = lv_obj_create(left);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(box, 212, 94);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x0F0F0F), 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x27272A), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 10, 0);
    lv_obj_align(box, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_pad_all(box, 12, 0);

    lv_obj_t *tgtLbl = lv_label_create(box);
    lblTarget = tgtLbl;
    lv_obj_set_style_text_color(tgtLbl, lv_color_hex(0xA1A1AA), 0);
    lv_obj_set_style_text_font(tgtLbl, &lv_font_pt_sans_10, 0);
    lv_obj_align(tgtLbl, LV_ALIGN_TOP_LEFT, 0, 0);

    labelTarget = lv_label_create(box);
    lv_label_set_text(labelTarget, "--");
    lv_obj_set_style_text_color(labelTarget, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(labelTarget, LV_ALIGN_TOP_RIGHT, 0, 0);

    lv_obj_t *remLbl = lv_label_create(box);
    lblRemain = remLbl;
    lv_obj_set_style_text_color(remLbl, lv_color_hex(0xA1A1AA), 0);
    lv_obj_set_style_text_font(remLbl, &lv_font_pt_sans_10, 0);
    lv_obj_align(remLbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    labelTimeRemaining = lv_label_create(box);
    lv_label_set_text(labelTimeRemaining, "--:--");
    lv_obj_set_style_text_color(labelTimeRemaining, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(labelTimeRemaining, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    barProgress = lv_bar_create(left);
    lv_obj_set_size(barProgress, 212, 8);
    lv_obj_align(barProgress, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_bar_set_range(barProgress, 0, 100);
    lv_bar_set_value(barProgress, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(barProgress, lv_color_hex(0x27272A), LV_PART_MAIN);
    lv_obj_set_style_bg_color(barProgress, lv_color_hex(0x10B981), LV_PART_INDICATOR);

    // Right column
    lv_obj_t *right = lv_obj_create(content);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(right, 236, 280);
    lv_obj_set_style_bg_color(right, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(right, 0, 0);
    lv_obj_set_style_pad_all(right, 12, 0);
    lv_obj_set_style_pad_row(right, 8, 0);

    btnPrograms = lv_btn_create(right);
    lv_obj_set_size(btnPrograms, 212, 180);
    lv_obj_set_style_bg_color(btnPrograms, lv_color_hex(0x0F0F0F), 0);
    lv_obj_set_style_border_color(btnPrograms, lv_color_hex(0x27272A), 0);
    lv_obj_set_style_border_width(btnPrograms, 1, 0);
    lv_obj_set_style_radius(btnPrograms, 12, 0);
    lv_obj_align(btnPrograms, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_add_event_cb(btnPrograms, btn_event_handler, LV_EVENT_CLICKED, this);

    lv_obj_t *selLbl = lv_label_create(btnPrograms);
    lblSelectProgram = selLbl;
    lv_obj_set_style_text_color(selLbl, lv_color_hex(0x10B981), 0);
    lv_obj_set_style_text_font(selLbl, &lv_font_pt_sans_10, 0);
    lv_obj_align(selLbl, LV_ALIGN_TOP_MID, 0, 10);

    labelProgramName = lv_label_create(btnPrograms);
    lv_label_set_text(labelProgramName, "Select Schedule");
    lv_obj_set_style_text_color(labelProgramName, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(labelProgramName, &lv_font_pt_sans_20, 0);
    lv_obj_align(labelProgramName, LV_ALIGN_CENTER, 0, -2);

    labelProgramIcon = lv_label_create(btnPrograms);
    lv_label_set_text(labelProgramIcon, "∿");
    lv_obj_set_style_text_color(labelProgramIcon, lv_color_hex(0x3F3F46), 0);
    lv_obj_set_style_text_font(labelProgramIcon, &lv_font_pt_sans_20, 0);
    lv_obj_align(labelProgramIcon, LV_ALIGN_CENTER, 0, -24);

    labelProgramMeta = lv_label_create(btnPrograms);
    lv_label_set_text(labelProgramMeta, "Tap to select");
    lv_obj_set_style_text_color(labelProgramMeta, lv_color_hex(0x71717A), 0);
    lv_obj_set_style_text_font(labelProgramMeta, &lv_font_pt_sans_10, 0);
    lv_obj_align(labelProgramMeta, LV_ALIGN_BOTTOM_MID, 0, -12);

    lv_obj_t *btnRow = lv_obj_create(right);
    lv_obj_clear_flag(btnRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(btnRow, 212, 80);
    lv_obj_align(btnRow, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(btnRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btnRow, 0, 0);
    lv_obj_set_layout(btnRow, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(btnRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btnRow, 8, 0);

    btnStart = lv_btn_create(btnRow);
    lv_obj_set_size(btnStart, 102, 80);
    lv_obj_set_style_radius(btnStart, 12, 0);
    lv_obj_set_style_bg_color(btnStart, lv_color_hex(0x27272A), 0);
    lv_obj_add_event_cb(btnStart, btn_event_handler, LV_EVENT_CLICKED, this);
    lblStart = lv_label_create(btnStart);
    lv_obj_set_style_text_font(lblStart, &lv_font_pt_sans_14, 0);
    lv_obj_center(lblStart);

    btnSchedules = lv_btn_create(btnRow);
    lv_obj_set_size(btnSchedules, 102, 80);
    lv_obj_set_style_radius(btnSchedules, 12, 0);
    lv_obj_set_style_bg_color(btnSchedules, lv_color_hex(0x27272A), 0);
    lv_obj_add_event_cb(btnSchedules, btn_event_handler, LV_EVENT_CLICKED, this);
    lblSchedules = lv_label_create(btnSchedules);
    lv_obj_set_style_text_font(lblSchedules, &lv_font_pt_sans_14, 0);
    lv_obj_center(lblSchedules);

    // --- Programs ---
    createProgramsScreen();

    // --- Editor ---
    createEditorScreen();

    updateLanguage();
}

void UIManager::createProgramsScreen() {
    screenPrograms = lv_obj_create(NULL);
    lv_obj_clear_flag(screenPrograms, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screenPrograms, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(screenPrograms, 0, 0);
    lv_obj_set_style_text_font(screenPrograms, &lv_font_pt_sans_20, 0);
    lv_obj_set_style_text_color(screenPrograms, lv_color_hex(0xFFFFFF), 0);

    lv_obj_t *header = lv_obj_create(screenPrograms);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(header, 480, 40);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x0B0B0B), 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(header, lv_color_hex(0x27272A), 0);
    lv_obj_set_style_pad_hor(header, 12, 0);
    lv_obj_set_style_pad_ver(header, 6, 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *icon = lv_label_create(header);
    lv_label_set_text(icon, "°C");
    lv_obj_set_style_text_color(icon, lv_color_hex(0x10B981), 0);
    lv_obj_set_style_text_font(icon, &lv_font_pt_sans_14, 0);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *title = lv_label_create(header);
    lblProgramsTitle = title;
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_pt_sans_20, 0);
    lv_obj_align_to(title, icon, LV_ALIGN_OUT_RIGHT_MID, 8, 0);

    btnBack = lv_btn_create(header);
    lv_obj_set_size(btnBack, 36, 36);
    lv_obj_align(btnBack, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(btnBack, lv_color_hex(0x27272A), 0);
    lv_obj_set_style_radius(btnBack, 12, 0);
    lv_obj_add_event_cb(btnBack, btn_event_handler, LV_EVENT_CLICKED, this);
    lv_obj_t *b = lv_label_create(btnBack);
    lblBack = b;
    lv_label_set_text(b, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(b, &lv_font_montserrat_14, 0);
    lv_obj_center(b);

    listContainer = lv_obj_create(screenPrograms);
    lv_obj_set_size(listContainer, 480, 280);
    lv_obj_align(listContainer, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(listContainer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(listContainer, 0, 0);
    lv_obj_set_style_pad_all(listContainer, 10, 0);
    lv_obj_set_style_pad_row(listContainer, 10, 0);
    lv_obj_set_layout(listContainer, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(listContainer, LV_FLEX_FLOW_COLUMN);

    updateLanguage();
}

namespace {
enum EditorAction : int {
    EDIT_ACT_TOGGLE = 1,
    EDIT_ACT_TARGET_MINUS = 2,
    EDIT_ACT_TARGET_PLUS = 3,
    EDIT_ACT_HOLD_MINUS = 4,
    EDIT_ACT_HOLD_PLUS = 5,
    EDIT_ACT_REMOVE = 6,
    EDIT_ACT_ADD = 7,
};

static void editor_btn_cb(lv_event_t *e) {
    const int packed = (int)(intptr_t)lv_event_get_user_data(e);
    const int action = packed & 0xFF;
    int index = (packed >> 8) & 0xFF;
    if (action == EDIT_ACT_ADD) index = -1;
    uiManager.editorAction(index, action);
}
} // namespace

void UIManager::createEditorScreen() {
    screenEditor = lv_obj_create(NULL);
    lv_obj_clear_flag(screenEditor, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screenEditor, lv_color_hex(0x000000), 0);
    lv_obj_set_style_pad_all(screenEditor, 0, 0);
    lv_obj_set_style_text_font(screenEditor, &lv_font_pt_sans_20, 0);
    lv_obj_set_style_text_color(screenEditor, lv_color_hex(0xFFFFFF), 0);

    lv_obj_t *header = lv_obj_create(screenEditor);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(header, 480, 40);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x0B0B0B), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_hor(header, 10, 0);
    lv_obj_set_style_pad_ver(header, 6, 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);

    btnEditorBack = lv_btn_create(header);
    lv_obj_set_size(btnEditorBack, 80, 28);
    lv_obj_align(btnEditorBack, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(btnEditorBack, lv_color_hex(0x27272A), 0);
    lv_obj_set_style_radius(btnEditorBack, 8, 0);
    lv_obj_add_event_cb(btnEditorBack, btn_event_handler, LV_EVENT_CLICKED, this);
    lv_obj_t *lblB = lv_label_create(btnEditorBack);
    lblEditorBack = lblB;
    lv_label_set_text(lblB, isUa ? "Назад" : "BACK");
    lv_obj_set_style_text_font(lblB, &lv_font_pt_sans_14, 0);
    lv_obj_center(lblB);

    btnEditorSave = lv_btn_create(header);
    lv_obj_set_size(btnEditorSave, 80, 28);
    lv_obj_align(btnEditorSave, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(btnEditorSave, lv_color_hex(0x10B981), 0);
    lv_obj_set_style_radius(btnEditorSave, 8, 0);
    lv_obj_add_event_cb(btnEditorSave, btn_event_handler, LV_EVENT_CLICKED, this);
    lv_obj_t *lblS = lv_label_create(btnEditorSave);
    lblEditorSave = lblS;
    lv_label_set_text(lblS, isUa ? "Зберегти" : "SAVE");
    lv_obj_set_style_text_font(lblS, &lv_font_pt_sans_14, 0);
    lv_obj_set_style_text_color(lblS, lv_color_hex(0x000000), 0);
    lv_obj_center(lblS);

    lv_obj_t *title = lv_label_create(header);
    lblEditorTitle = title;
    lv_label_set_text(title, isUa ? "Редактор" : "EDITOR");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_pt_sans_20, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    editorList = lv_obj_create(screenEditor);
    lv_obj_set_size(editorList, 480, 280);
    lv_obj_align(editorList, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(editorList, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(editorList, 0, 0);
    lv_obj_set_style_pad_all(editorList, 10, 0);
    lv_obj_set_style_pad_row(editorList, 10, 0);
    lv_obj_set_layout(editorList, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(editorList, LV_FLEX_FLOW_COLUMN);
}

bool UIManager::loadEditorFromSelectedJson() {
    editorSteps.clear();

    std::string json = selectedProgramJson;
    if (json.empty() && !selectedProgramName.empty()) {
        const std::string file = read_file_to_string("/littlefs/schedules.json");
        if (!file.empty()) {
            cJSON *arr = cJSON_Parse(file.c_str());
            if (arr && cJSON_IsArray(arr)) {
                cJSON *it = nullptr;
                cJSON_ArrayForEach(it, arr) {
                    const cJSON *n = cJSON_GetObjectItem(it, "name");
                    if (cJSON_IsString(n) && n->valuestring && selectedProgramName == n->valuestring) {
                        char *rendered = cJSON_PrintUnformatted(it);
                        if (rendered) {
                            json.assign(rendered);
                            free(rendered);
                        }
                        break;
                    }
                }
            }
            if (arr) cJSON_Delete(arr);
        }
    }

    if (json.empty()) return false;

    cJSON *root = cJSON_Parse(json.c_str());
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        return false;
    }

    cJSON *steps = cJSON_GetObjectItem(root, "steps");
    if (!cJSON_IsArray(steps)) steps = cJSON_GetObjectItem(root, "segments");
    if (!cJSON_IsArray(steps)) {
        cJSON_Delete(root);
        return false;
    }

    cJSON *step = nullptr;
    cJSON_ArrayForEach(step, steps) {
        if (!cJSON_IsObject(step)) continue;
        EditorStep s{};
        const cJSON *typeItem = cJSON_GetObjectItem(step, "type");
        const char *type = (cJSON_IsString(typeItem) && typeItem->valuestring) ? typeItem->valuestring : "ramp";
        s.isHold = (strcmp(type, "hold") == 0);
        if (s.isHold) {
            const cJSON *v = cJSON_GetObjectItem(step, "holdTime");
            s.holdMin = cJSON_IsNumber(v) ? (int)std::lround(v->valuedouble) : 0;
        } else {
            const cJSON *v = cJSON_GetObjectItem(step, "target");
            s.targetC = cJSON_IsNumber(v) ? (int)std::lround(v->valuedouble) : 0;
        }
        editorSteps.push_back(s);
    }

    cJSON_Delete(root);
    return !editorSteps.empty();
}

void UIManager::rebuildEditorList() {
    if (!editorList) return;

    lv_obj_clean(editorList);

    for (size_t i = 0; i < editorSteps.size(); i++) {
        const EditorStep &s = editorSteps[i];

        lv_obj_t *row = lv_obj_create(editorList);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(row, 460, 56);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x0F0F0F), 0);
        lv_obj_set_style_border_color(row, lv_color_hex(0x27272A), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_pad_all(row, 8, 0);

        lv_obj_t *lblIdx = lv_label_create(row);
        lv_label_set_text_fmt(lblIdx, "%u", (unsigned)(i + 1));
        lv_obj_set_style_text_color(lblIdx, lv_color_hex(0xA1A1AA), 0);
        lv_obj_align(lblIdx, LV_ALIGN_LEFT_MID, 6, 0);

        lv_obj_t *btnType = lv_btn_create(row);
        lv_obj_set_size(btnType, 88, 36);
        lv_obj_align(btnType, LV_ALIGN_LEFT_MID, 40, 0);
        lv_obj_set_style_bg_color(btnType, lv_color_hex(0x27272A), 0);
        lv_obj_set_style_radius(btnType, 8, 0);
        lv_obj_add_event_cb(btnType, editor_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(((int)i << 8) | EDIT_ACT_TOGGLE));
        lv_obj_t *lblType = lv_label_create(btnType);
        lv_label_set_text(lblType, s.isHold ? "HOLD" : "RAMP");
        lv_obj_center(lblType);

        lv_obj_t *lblVal = lv_label_create(row);
        lv_obj_set_style_text_color(lblVal, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(lblVal, &lv_font_pt_sans_20, 0);
        if (s.isHold) lv_label_set_text_fmt(lblVal, "%d min", s.holdMin);
        else lv_label_set_text_fmt(lblVal, "%d C", s.targetC);
        lv_obj_align(lblVal, LV_ALIGN_CENTER, 20, 0);

        lv_obj_t *btnMinus = lv_btn_create(row);
        lv_obj_set_size(btnMinus, 36, 36);
        lv_obj_align(btnMinus, LV_ALIGN_RIGHT_MID, -92, 0);
        lv_obj_set_style_bg_color(btnMinus, lv_color_hex(0x27272A), 0);
        lv_obj_set_style_radius(btnMinus, 8, 0);
        lv_obj_add_event_cb(btnMinus, editor_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(((int)i << 8) | (s.isHold ? EDIT_ACT_HOLD_MINUS : EDIT_ACT_TARGET_MINUS)));
        lv_obj_t *lblM = lv_label_create(btnMinus);
        lv_label_set_text(lblM, "-");
        lv_obj_center(lblM);

        lv_obj_t *btnPlus = lv_btn_create(row);
        lv_obj_set_size(btnPlus, 36, 36);
        lv_obj_align(btnPlus, LV_ALIGN_RIGHT_MID, -52, 0);
        lv_obj_set_style_bg_color(btnPlus, lv_color_hex(0x27272A), 0);
        lv_obj_set_style_radius(btnPlus, 8, 0);
        lv_obj_add_event_cb(btnPlus, editor_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(((int)i << 8) | (s.isHold ? EDIT_ACT_HOLD_PLUS : EDIT_ACT_TARGET_PLUS)));
        lv_obj_t *lblP = lv_label_create(btnPlus);
        lv_label_set_text(lblP, "+");
        lv_obj_center(lblP);

        lv_obj_t *btnDel = lv_btn_create(row);
        lv_obj_set_size(btnDel, 36, 36);
        lv_obj_align(btnDel, LV_ALIGN_RIGHT_MID, -10, 0);
        lv_obj_set_style_bg_color(btnDel, lv_color_hex(0x7F1D1D), 0);
        lv_obj_set_style_radius(btnDel, 8, 0);
        lv_obj_add_event_cb(btnDel, editor_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(((int)i << 8) | EDIT_ACT_REMOVE));
        lv_obj_t *lblD = lv_label_create(btnDel);
        lv_label_set_text(lblD, "X");
        lv_obj_center(lblD);
    }

    lv_obj_t *btnAdd = lv_btn_create(editorList);
    lv_obj_set_size(btnAdd, 460, 52);
    lv_obj_set_style_bg_color(btnAdd, lv_color_hex(0x0F0F0F), 0);
    lv_obj_set_style_border_color(btnAdd, lv_color_hex(0x27272A), 0);
    lv_obj_set_style_border_width(btnAdd, 1, 0);
    lv_obj_set_style_radius(btnAdd, 10, 0);
    lv_obj_add_event_cb(btnAdd, editor_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)EDIT_ACT_ADD);
    lv_obj_t *lblA = lv_label_create(btnAdd);
    lv_label_set_text(lblA, "+ ADD STEP");
    lv_obj_center(lblA);
}

bool UIManager::saveEditedSchedule() {
    if (selectedProgramName.empty()) return false;

    const std::string file = read_file_to_string("/littlefs/schedules.json");
    if (file.empty()) return false;

    cJSON *arr = cJSON_Parse(file.c_str());
    if (!arr || !cJSON_IsArray(arr)) {
        if (arr) cJSON_Delete(arr);
        return false;
    }

    cJSON *match = nullptr;
    cJSON *it = nullptr;
    cJSON_ArrayForEach(it, arr) {
        const cJSON *n = cJSON_GetObjectItem(it, "name");
        if (cJSON_IsString(n) && n->valuestring && selectedProgramName == n->valuestring) {
            match = it;
            break;
        }
    }

    if (!match) {
        cJSON_Delete(arr);
        return false;
    }

    cJSON_DeleteItemFromObject(match, "steps");
    cJSON *steps = cJSON_CreateArray();
    for (const EditorStep &s : editorSteps) {
        cJSON *obj = cJSON_CreateObject();
        if (s.isHold) {
            cJSON_AddStringToObject(obj, "type", "hold");
            cJSON_AddNumberToObject(obj, "holdTime", (double)s.holdMin);
        } else {
            cJSON_AddStringToObject(obj, "type", "ramp");
            cJSON_AddNumberToObject(obj, "target", (double)s.targetC);
            cJSON_AddNumberToObject(obj, "rate", 0);
        }
        cJSON_AddItemToArray(steps, obj);
    }
    cJSON_AddItemToObject(match, "steps", steps);

    char *renderedSchedule = cJSON_PrintUnformatted(match);
    if (renderedSchedule) {
        selectedProgramJson = renderedSchedule;
        thermalCtrl.loadSchedule(selectedProgramJson);
        free(renderedSchedule);
    }

    char *renderedAll = cJSON_PrintUnformatted(arr);
    std::string out = renderedAll ? renderedAll : "[]";
    if (renderedAll) free(renderedAll);
    cJSON_Delete(arr);

    const bool ok = write_string_to_file("/littlefs/schedules.json", out);
    if (ok) {
        wifiServer.notifySchedulesChanged("save", selectedProgramName.c_str());
    }
    return ok;
}

void UIManager::updateLanguage() {
    applyLanguageLabels();
    return;
    if (langLabel) lv_label_set_text(langLabel, isUa ? "UA" : "EN");
    if (labelProgramMeta) lv_label_set_text(labelProgramMeta, tr(isUa, "Tap to select", "Торкніться щоб вибрати"));
    if (lblCurrent) lv_label_set_text(lblCurrent, tr(isUa, "CURRENT TEMP", "Поточна температура"));
    if (lblTarget) lv_label_set_text(lblTarget, tr(isUa, "TARGET", "Цільова"));
    if (lblRemain) lv_label_set_text(lblRemain, tr(isUa, "TIME LEFT", "Залишилось часу"));
    if (lblSelectProgram) lv_label_set_text(lblSelectProgram, tr(isUa, "SELECT PROGRAM", "Вибір програми"));
    if (lblStart) lv_label_set_text(lblStart, tr(isUa, LV_SYMBOL_PLAY "\nSTART", LV_SYMBOL_PLAY "\nСтарт"));
    if (lblBack) lv_label_set_text(lblBack, tr(isUa, "BACK", "Назад"));
    if (lblProgramsTitle) lv_label_set_text(lblProgramsTitle, tr(isUa, "LIBRARY", "Бібліотека"));
    if (labelProgramName && selectedProgramName.empty()) lv_label_set_text(labelProgramName, tr(isUa, "None", "Немає"));
    if (lblSchedules) lv_label_set_text(lblSchedules, tr(isUa, LV_SYMBOL_SETTINGS "\nPrograms", LV_SYMBOL_SETTINGS "\nПрограми"));
    if (lblEditorBack) lv_label_set_text(lblEditorBack, tr(isUa, "BACK", "Назад"));
    if (lblEditorSave) lv_label_set_text(lblEditorSave, tr(isUa, "SAVE", "Зберегти"));
    if (lblEditorTitle) lv_label_set_text(lblEditorTitle, tr(isUa, "EDITOR", "Редактор"));
}



void UIManager::toggleLanguage() {
    isUa = !isUa;
    updateLanguage();
}

bool UIManager::isUkrainian() const {
    return isUa;
}

void UIManager::updateStatus(const KilnState &state) {
    updateStatusV2(state);
    return;
    // WiFi indicator (simple)
    if (labelWifi) {
        // In AP mode we consider it "offline" for the main network.
        const bool ok = !wifiServer.isAPMode;
        lv_label_set_text(labelWifi, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_color(labelWifi, ok ? lv_color_hex(0x10B981) : lv_color_hex(0x52525B), 0);
    }

    // Temp
    if (labelTemp) {
        char t[16];
        snprintf(t, sizeof(t), "%.1f", state.currentTemp);
        lv_label_set_text(labelTemp, t);
    }

    // Target
    if (labelTarget) {
        char t[16];
        if (state.targetTemp > 0.0f) snprintf(t, sizeof(t), "%.0fC", state.targetTemp);
        else snprintf(t, sizeof(t), "--");
        lv_label_set_text(labelTarget, t);
    }

    // Status badge
    if (labelStatusBadge) {
        const char *s = "IDLE";
        switch (state.status) {
            case KILN_IDLE: s = "IDLE"; break;
            case KILN_RAMP: s = "RAMP"; break;
            case KILN_HOLD: s = "HOLD"; break;
            case KILN_COMPLETE: s = "COMPLETE"; break;
            case KILN_ERROR: s = "ERROR"; break;
            default: s = state.isFiring ? "FIRING" : "IDLE"; break;
        }
        if (!state.errorMsg.empty() && state.errorMsg != "System Boot" && strcmp(s, "IDLE") == 0) s = "ERROR";
        lv_label_set_text(labelStatusBadge, s);

        lv_color_t bg = lv_color_hex(0x27272A);
        if (strcmp(s, "ERROR") == 0) bg = lv_color_hex(0xEF4444);
        else if (strcmp(s, "IDLE") != 0) bg = lv_color_hex(0x10B981);
        lv_obj_set_style_bg_color(labelStatusBadge, bg, 0);
        lv_obj_set_style_text_color(labelStatusBadge, lv_color_hex(strcmp(s, "IDLE") != 0 && strcmp(s, "ERROR") != 0 ? 0x000000 : 0xFFFFFF), 0);
    }

    // Progress bar (temp vs target)
    if (barProgress) {
        int v = 0;
        if (state.targetTemp > 0.0f && state.currentTemp > 0.0f) {
            v = (int)std::min(100.0f, (state.currentTemp / state.targetTemp) * 100.0f);
        }
        lv_bar_set_value(barProgress, v, LV_ANIM_OFF);
    }

    // Time remaining (minutes -> HH:MM)
    if (labelTimeRemaining) {
        if (state.isFiring && state.timeRemaining >= 0) {
            const int hours = state.timeRemaining / 60;
            const int mins = state.timeRemaining % 60;
            char tr[16];
            snprintf(tr, sizeof(tr), "%02d:%02d", hours, mins);
            lv_label_set_text(labelTimeRemaining, tr);
        } else {
            lv_label_set_text(labelTimeRemaining, "--:--");
        }
    }
    // Program card
    if (state.isFiring) {
        if (lblSelectProgram) lv_label_set_text(lblSelectProgram, tr(isUa, "STEP", "Крок"));
        if (labelProgramName) {
            char stepBuf[24];
            snprintf(stepBuf, sizeof(stepBuf), "%d / %d", state.currentStep, state.totalSteps);
            lv_obj_set_style_text_font(labelProgramName, &lv_font_montserrat_48, 0);
            lv_label_set_text(labelProgramName, stepBuf);
        }
        if (labelProgramMeta) {
            char meta[64];
            if (state.status == KILN_HOLD) {
                snprintf(meta, sizeof(meta), "%s %.0fC", isUa ? "Витримка" : "Hold", state.targetTemp);
            } else {
                snprintf(meta, sizeof(meta), "%s %.0fC", isUa ? "Нагрів до" : "Ramp to", state.targetTemp);
            }
            lv_label_set_text(labelProgramMeta, meta);
        }
    } else {
        if (lblSelectProgram) lv_label_set_text(lblSelectProgram, tr(isUa, "SELECT PROGRAM", "Вибір програми"));
        if (labelProgramName) {
            lv_obj_set_style_text_font(labelProgramName, &lv_font_pt_sans_20, 0);
            if (!selectedProgramName.empty()) {
                lv_label_set_text(labelProgramName, selectedProgramName.c_str());
            } else {
                lv_label_set_text(labelProgramName, tr(isUa, (state.totalSteps > 0) ? "Loaded" : "None",
                                                     (state.totalSteps > 0) ? "Завантажено" : "Немає"));
            }
        }
        if (labelProgramMeta) {
            if (state.totalSteps > 0) {
                char meta[64];
                snprintf(meta, sizeof(meta), "%d %s", state.totalSteps, isUa ? "кроків" : "segments");
                lv_label_set_text(labelProgramMeta, meta);
            } else {
                lv_label_set_text(labelProgramMeta, tr(isUa, "Tap to select", "Торкніться щоб вибрати"));
            }
        }
    }

    // Fault banner
    if (faultBanner && labelFault) {
        const SafetyStats safety = thermalCtrl.getSafetyStats();
        if (safety.faultActive) {
            lv_obj_clear_flag(faultBanner, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(labelFault, safety.faultReason[0] ? safety.faultReason : "FAULT");
        } else {
            lv_obj_add_flag(faultBanner, LV_OBJ_FLAG_HIDDEN);
        }
    }

            // Start/Stop button
    if (btnStart) {
        lv_obj_t *lbl = lv_obj_get_child(btnStart, 0);
        const char *stopLabel = isUa ? "Стоп" : "STOP";
        const char *startLabel = isUa ? "Старт" : "START";
        const char *selectLabel = isUa ? "Вибір" : "SELECT";
        if (state.isFiring) {
            lv_obj_set_size(btnStart, 216, 80);
            lv_obj_set_style_bg_color(btnStart, lv_color_hex(0xDC2626), 0);
            if (lbl) lv_label_set_text(lbl, stopLabel);
            if (btnSchedules) lv_obj_add_flag(btnSchedules, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_set_size(btnStart, 104, 80);
            if (btnSchedules) lv_obj_clear_flag(btnSchedules, LV_OBJ_FLAG_HIDDEN);
            if (state.totalSteps > 0) {
                lv_obj_set_style_bg_color(btnStart, lv_color_hex(0x10B981), 0);
                if (lbl) lv_label_set_text(lbl, startLabel);
            } else {
                lv_obj_set_style_bg_color(btnStart, lv_color_hex(0x27272A), 0);
                if (lbl) lv_label_set_text(lbl, selectLabel);
            }
        }
    }
}

void UIManager::startFiring() {
    if (thermalCtrl.getState().totalSteps > 0) {
        thermalCtrl.start();
    } else {
        showPrograms();
    }
}

void UIManager::stopFiring() {
    ESP_LOGI(TAG, "UI: stop");
    thermalCtrl.stop("User Button");
    hide_stop_modal(this);
}

void UIManager::setTargetTemperature(float temp) { (void)temp; }

void UIManager::showMain() { lv_scr_load(screenMain); }

void UIManager::showPrograms() {
    showProgramsV2();
    return;
    if (!listContainer) return;

    lv_obj_clean(listContainer);

    FILE *f = fopen("/littlefs/schedules.json", "r");
    if (!f) {
        lv_obj_t *lbl = lv_label_create(listContainer);
        lv_label_set_text(lbl, tr(isUa, "No schedules", "Немає програм"));
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
        lv_scr_load(screenPrograms);
        return;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buffer = (char *)malloc(fsize + 1);
    if (!buffer) {
        fclose(f);
        lv_scr_load(screenPrograms);
        return;
    }
    fread(buffer, 1, fsize, f);
    fclose(f);
    buffer[fsize] = 0;

    cJSON *arr = cJSON_Parse(buffer);
    free(buffer);

    if (!arr || !cJSON_IsArray(arr)) {
        if (arr) cJSON_Delete(arr);
        lv_obj_t *lbl = lv_label_create(listContainer);
        lv_label_set_text(lbl, tr(isUa, "JSON error", "Помилка даних"));
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
        lv_scr_load(screenPrograms);
        return;
    }

    int idx = 0;
    cJSON *obj = NULL;
    cJSON_ArrayForEach(obj, arr) {
        const cJSON *nameItem = cJSON_GetObjectItem(obj, "name");
        const char *name = (cJSON_IsString(nameItem) && nameItem->valuestring) ? nameItem->valuestring : tr(isUa, "Unnamed", "Без назви");

        const cJSON *stepsArr = cJSON_GetObjectItem(obj, "steps");
        if (!cJSON_IsArray(stepsArr)) stepsArr = cJSON_GetObjectItem(obj, "segments");
        int steps = cJSON_IsArray(stepsArr) ? cJSON_GetArraySize(stepsArr) : 0;
        const bool isSelected = (selectedProgramName == name);
        lv_obj_t *row = lv_btn_create(listContainer);
        lv_obj_set_size(row, 460, 56);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x0F0F0F), 0);
        lv_obj_set_style_border_color(row, lv_color_hex(0x27272A), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_add_event_cb(row, program_edit_handler, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

        lv_obj_t *lblName = lv_label_create(row);
        lv_label_set_text(lblName, name);
        lv_obj_set_style_text_color(lblName, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(lblName, &lv_font_pt_sans_14, 0);
        lv_obj_align(lblName, LV_ALIGN_LEFT_MID, 12, -8);

        lv_obj_t *lblSteps = lv_label_create(row);
        lv_label_set_text_fmt(lblSteps, "%d %s", steps, isUa ? "кроків" : "segments");
        lv_obj_set_style_text_color(lblSteps, lv_color_hex(0xA1A1AA), 0);
        lv_obj_set_style_text_font(lblSteps, &lv_font_pt_sans_10, 0);
        lv_obj_align(lblSteps, LV_ALIGN_LEFT_MID, 12, 10);

        lv_obj_t *btnSelect = lv_btn_create(row);
        lv_obj_set_size(btnSelect, 38, 38);
        lv_obj_set_style_bg_color(btnSelect, isSelected ? lv_color_hex(0x10B981) : lv_color_hex(0x27272A), 0);
        lv_obj_set_style_radius(btnSelect, 19, 0);
        lv_obj_align(btnSelect, LV_ALIGN_RIGHT_MID, -10, 0);
        lv_obj_add_event_cb(btnSelect, program_select_handler, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
        lv_obj_t *lblSel = lv_label_create(btnSelect);
        lv_label_set_text(lblSel, LV_SYMBOL_PLAY);
        lv_obj_set_style_text_color(lblSel, isSelected ? lv_color_hex(0x000000) : lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(lblSel);

        idx++;

    }

    cJSON_Delete(arr);
    lv_scr_load(screenPrograms);
}

void UIManager::showEditor() {
    if (!screenEditor) return;
    if (!loadEditorFromSelectedJson()) return;
    rebuildEditorList();
    lv_scr_load(screenEditor);
}

void UIManager::editorAction(int index, int action) {
    if (action == EDIT_ACT_ADD) {
        EditorStep s{};
        s.isHold = false;
        s.targetC = 500;
        editorSteps.push_back(s);
        rebuildEditorList();
        return;
    }

    if (index < 0 || (size_t)index >= editorSteps.size()) return;

    EditorStep &s = editorSteps[(size_t)index];

    switch (action) {
        case EDIT_ACT_TOGGLE:
            s.isHold = !s.isHold;
            if (s.isHold) {
                if (s.holdMin <= 0) s.holdMin = 30;
            } else {
                if (s.targetC <= 0) s.targetC = 500;
            }
            break;
        case EDIT_ACT_TARGET_MINUS:
            s.targetC = std::max(0, s.targetC - 10);
            break;
        case EDIT_ACT_TARGET_PLUS:
            s.targetC = std::min(2000, s.targetC + 10);
            break;
        case EDIT_ACT_HOLD_MINUS:
            s.holdMin = std::max(0, s.holdMin - 1);
            break;
        case EDIT_ACT_HOLD_PLUS:
            s.holdMin = std::min(999, s.holdMin + 1);
            break;
        case EDIT_ACT_REMOVE:
            editorSteps.erase(editorSteps.begin() + index);
            break;
        default:
            break;
    }

    rebuildEditorList();
}
