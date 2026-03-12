#include "ui_manager.h"
#include "LGFX_Setup.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include <ArduinoJson.h>
#include <vector>

static const char *TAG = "UI";

UIManager uiManager;
extern LGFX tft;

// LVGL Buffer
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[480 * 10]; // 10 lines buffer
static lv_obj_t* listContainer;

// Flush Callback
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.writePixels((uint16_t *)&color_p->full, w * h, true);
    tft.endWrite();

    lv_disp_flush_ready(disp);
}

// Read Touch Callback
void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data) {
    uint16_t touchX, touchY;
    bool touched = tft.getTouch(&touchX, &touchY);

    if (!touched) {
        data->state = LV_INDEV_STATE_REL;
    } else {
        data->state = LV_INDEV_STATE_PR;
        data->point.x = touchX;
        data->point.y = touchY;
    }
}

UIManager::UIManager() {
    lastUpdate = 0;
}

void UIManager::begin() {
    ESP_LOGI(TAG, "UIManager::begin() LVGL");
    
    // Init LGFX
    tft.init();
    tft.setRotation(1); // Landscape
    
    // Init LVGL
    lv_init();
    
    // Init Buffer
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, 480 * 10);

    // Init Display Driver
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = 480;
    disp_drv.ver_res = 320;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    
    lv_disp_drv_register(&disp_drv);

    // Init Touch Driver
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    // Create UI
    createScreens();
    showMain();
}

void UIManager::loop() {
    lv_timer_handler();
}

static void btn_event_handler(lv_event_t * e) {
    lv_obj_t * obj = lv_event_get_target(e);
    UIManager* ui = (UIManager*)lv_event_get_user_data(e);
    
    if(obj == ui->btnStart) {
        // We rely on updateStatus() to maintain state, but we need to handle the click event
        // based on the *current* state of the kiln, not just the button state which might be lagging slightly
        KilnState state = thermalCtrl.getState();
        
        if (state.isFiring) {
             ui->stopFiring();
        } else {
             if (state.totalSteps > 0) {
                 ui->startFiring();
             } else {
                 ui->showPrograms();
             }
        }
    } else if(obj == ui->btnPrograms) {
        ui->showPrograms();
    } else if(obj == ui->btnBack) {
        ui->showMain();
    }
}

static void program_item_handler(lv_event_t * e) {
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    
    // Read schedule file
    FILE* f = fopen("/littlefs/schedules.json", "r");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open schedules.json");
        return;
    }
    
    // Read into buffer
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char* buffer = (char*)malloc(fsize + 1);
    if (!buffer) {
        fclose(f);
        return;
    }
    fread(buffer, 1, fsize, f);
    fclose(f);
    buffer[fsize] = 0;
    
    // Parse
    DynamicJsonDocument doc(4096); 
    DeserializationError error = deserializeJson(doc, buffer);
    free(buffer);
    
    if (error) {
        ESP_LOGE(TAG, "Failed to parse schedules.json");
        return;
    }
    
    JsonArray arr = doc.as<JsonArray>();
    if (index >= 0 && index < arr.size()) {
        std::string progJson;
        serializeJson(arr[index], progJson);
        thermalCtrl.loadSchedule(progJson);
        ESP_LOGI(TAG, "Loaded schedule index %d", index);
        uiManager.showMain();
    }
}

void UIManager::createScreens() {
    screenMain = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screenMain, lv_color_hex(0x0A0A0A), 0);

    labelTemp = lv_label_create(screenMain);
    lv_label_set_text(labelTemp, "25.0 C");
    lv_obj_set_style_text_color(labelTemp, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(labelTemp, &lv_font_montserrat_48, 0);
    lv_obj_align(labelTemp, LV_ALIGN_CENTER, -100, -50);

    labelStatus = lv_label_create(screenMain);
    lv_label_set_text(labelStatus, "Ready");
    lv_obj_set_style_text_color(labelStatus, lv_color_hex(0x00E676), 0);
    lv_obj_align(labelStatus, LV_ALIGN_TOP_RIGHT, -10, 10);

    btnStart = lv_btn_create(screenMain);
    lv_obj_align(btnStart, LV_ALIGN_BOTTOM_RIGHT, -20, -20);
    lv_obj_set_size(btnStart, 200, 50); // Increased width
    lv_obj_set_style_bg_color(btnStart, lv_color_hex(0x2979FF), 0); // Default Blue
    // Removed LV_OBJ_FLAG_CHECKABLE to avoid auto-toggle logic
    lv_obj_add_event_cb(btnStart, btn_event_handler, LV_EVENT_CLICKED, this); 
    lv_obj_t* l = lv_label_create(btnStart);
    lv_label_set_text(l, "ВИБРАТИ ПРОГРАМУ"); // SELECT PROGRAM
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_center(l);

    btnPrograms = lv_btn_create(screenMain);
    lv_obj_align(btnPrograms, LV_ALIGN_BOTTOM_LEFT, 20, -20);
    lv_obj_set_size(btnPrograms, 120, 50);
    lv_obj_add_event_cb(btnPrograms, btn_event_handler, LV_EVENT_CLICKED, this);
    l = lv_label_create(btnPrograms);
    lv_label_set_text(l, "ПРОГРАМИ"); // PROGRAMS
    lv_obj_center(l);

    createProgramsScreen();
}

void UIManager::createProgramsScreen() {
    screenPrograms = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screenPrograms, lv_color_hex(0x141414), 0);
    lv_obj_set_layout(screenPrograms, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(screenPrograms, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(screenPrograms, 0, 0);

    // Header container
    lv_obj_t* header = lv_obj_create(screenPrograms);
    lv_obj_set_size(header, LV_PCT(100), 60);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x202020), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    
    btnBack = lv_btn_create(header);
    lv_obj_align(btnBack, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_set_size(btnBack, 80, 40);
    lv_obj_add_event_cb(btnBack, btn_event_handler, LV_EVENT_CLICKED, this);
    lv_obj_t* l = lv_label_create(btnBack);
    lv_label_set_text(l, "Назад"); // Back
    lv_obj_center(l);
    
    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, "Вибір Програми"); // Select Program
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    // List Container
    listContainer = lv_obj_create(screenPrograms);
    lv_obj_set_size(listContainer, LV_PCT(100), LV_PCT(100)); 
    lv_obj_set_flex_grow(listContainer, 1); // Fill remaining space
    lv_obj_set_flex_flow(listContainer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(listContainer, lv_color_hex(0x141414), 0);
    lv_obj_set_style_border_width(listContainer, 0, 0);
    lv_obj_set_style_pad_all(listContainer, 10, 0);
    lv_obj_set_style_pad_row(listContainer, 10, 0);
}

void UIManager::updateStatus(const KilnState& state) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f C", state.currentTemp);
    if(labelTemp) lv_label_set_text(labelTemp, buf);
    
    if(state.isFiring && state.totalSteps > 0) { // Only show firing if there are steps AND firing is active
        if(labelStatus) lv_label_set_text(labelStatus, "ВИПАЛ"); // FIRING
        
        // Firing state -> Red "STOP"
        if (btnStart) {
            // Force Red color and STOP text if firing
            lv_obj_set_style_bg_color(btnStart, lv_color_hex(0xFF1744), 0);
            lv_obj_t* label = lv_obj_get_child(btnStart, 0);
            if (label) lv_label_set_text(label, "ЗУПИНИТИ"); // STOP
        }
    } else {
        // IDLE or ERROR or COMPLETE state
        if(labelStatus) lv_label_set_text(labelStatus, "Готовий"); // Ready
        
        if (btnStart) {
             lv_obj_t* label = lv_obj_get_child(btnStart, 0);
             
             // Check if a program is loaded
             if (state.totalSteps > 0) {
                 // Program loaded but NOT firing -> Green "START"
                 lv_obj_set_style_bg_color(btnStart, lv_color_hex(0x00E676), 0); // Green
                 if (label) lv_label_set_text(label, "ПОЧАТИ ВИПАЛ"); // START
                 lv_obj_clear_state(btnStart, LV_STATE_DISABLED);
             } else {
                 // No program -> Blue "SELECT PROGRAM"
                 // This is the DEFAULT state
                 lv_obj_set_style_bg_color(btnStart, lv_color_hex(0x2979FF), 0); // Blue
                 if (label) lv_label_set_text(label, "ВИБРАТИ ПРОГРАМУ"); // SELECT PROGRAM
             }
        }
    }
}

void UIManager::startFiring() {
    // If schedule is already loaded, just start
    if (thermalCtrl.getState().totalSteps > 0) {
        thermalCtrl.start();
    } else {
        // If no schedule is loaded, clicking "SELECT PROGRAM" should go to programs screen
        showPrograms();
    }
}

void UIManager::stopFiring() {
    ESP_LOGI(TAG, "UI: Stop Firing Clicked");
    thermalCtrl.stop("User Button");
}

void UIManager::setTargetTemperature(float temp) {
}

void UIManager::showMain() {
    lv_scr_load(screenMain);
}

void UIManager::showPrograms() {
    // Clear list
    if (listContainer) {
        lv_obj_clean(listContainer);
        
        // Read JSON
        FILE* f = fopen("/littlefs/schedules.json", "r");
        if (!f) {
            lv_obj_t* lbl = lv_label_create(listContainer);
            lv_label_set_text(lbl, "Програм не знайдено"); // No schedules found
            lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        } else {
            fseek(f, 0, SEEK_END);
            long fsize = ftell(f);
            fseek(f, 0, SEEK_SET);
            
            char* buffer = (char*)malloc(fsize + 1);
            if (buffer) {
                fread(buffer, 1, fsize, f);
                buffer[fsize] = 0;
                
                DynamicJsonDocument doc(4096);
                DeserializationError error = deserializeJson(doc, buffer);
                free(buffer);
                
                if (!error) {
                    JsonArray arr = doc.as<JsonArray>();
                    int idx = 0;
                    for (JsonObject obj : arr) {
                        const char* name = obj["name"] | "Unnamed";
                        int steps = obj["steps"].size();
                        
                        lv_obj_t* btn = lv_btn_create(listContainer);
                        lv_obj_set_size(btn, LV_PCT(100), 60);
                        lv_obj_add_event_cb(btn, program_item_handler, LV_EVENT_CLICKED, (void*)(intptr_t)idx);
                        
                        lv_obj_t* lblName = lv_label_create(btn);
                        lv_label_set_text(lblName, name);
                        lv_obj_align(lblName, LV_ALIGN_LEFT_MID, 10, 0);
                        
                        lv_obj_t* lblSteps = lv_label_create(btn);
                        lv_label_set_text_fmt(lblSteps, "%d кроків", steps); // steps
                        lv_obj_align(lblSteps, LV_ALIGN_RIGHT_MID, -10, 0);
                        
                        idx++;
                    }
                } else {
                    lv_obj_t* lbl = lv_label_create(listContainer);
                    lv_label_set_text(lbl, "Помилка JSON"); // JSON Error
                    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
                }
            }
            fclose(f);
        }
    }
    
    lv_scr_load(screenPrograms);
}
