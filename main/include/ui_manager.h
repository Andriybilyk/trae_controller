#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#include <lvgl.h>
#include "thermal_control.h"

class UIManager {
public:
    UIManager();
    void begin();
    void loop();
    
    // Updates from Control
    void updateStatus(const KilnState& state);
    
    // Callbacks from UI to Control
    void setTargetTemperature(float temp);
    void startFiring();
    void stopFiring();
    
    // Screen Navigation
    void showMain();
    void showPrograms();

    // LVGL Objects
    lv_obj_t* screenMain;
    lv_obj_t* screenPrograms;
    lv_obj_t* labelTemp;
    lv_obj_t* labelTarget;
    lv_obj_t* labelStatus;
    lv_obj_t* chart;
    lv_chart_series_t* serTemp;
    lv_chart_series_t* serTarget;
    
    // Buttons
    lv_obj_t* btnStart;
    lv_obj_t* btnStop;
    lv_obj_t* btnPrograms;
    lv_obj_t* btnBack;

private:
    void initLVGL();
    void createScreens();
    void createProgramsScreen();
    void updateChart(float temp, float target);

    unsigned long lastUpdate;
};

extern UIManager uiManager;

#endif
