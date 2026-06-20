#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#include <lvgl.h>
#include <cstdint>
#include <string>
#include <vector>
#include "kiln_control/thermal_control.h"

class UIManager {
public:
    UIManager();
    void begin();
    void loop();
    void updateLanguage();
    void toggleLanguage();
    bool isUkrainian() const;
    
    // Updates from Control
    void updateStatus(const KilnState& state);
    
    // Callbacks from UI to Control
    void setTargetTemperature(float temp);
    void startFiring();
    void stopFiring();
    
    // Screen Navigation
    void showMain();
    void showPrograms();
    void showEditor();
    void editorAction(int index, int action);
    bool saveEditedSchedule();

    // LVGL Objects
    lv_obj_t* screenMain;
    lv_obj_t* screenPrograms;
    lv_obj_t* screenEditor;
    lv_obj_t* labelTemp;
    lv_obj_t* labelTarget;
    lv_obj_t* labelStatus;
    lv_obj_t* labelStatusBadge;
    lv_obj_t* labelProgramName;
    lv_obj_t* labelProgramMeta;
    lv_obj_t* labelProgramIcon;
    lv_obj_t* labelClock;
    lv_obj_t* labelWifi;
    lv_obj_t* labelTimeRemaining;
    lv_obj_t* faultBanner;
    lv_obj_t* labelFault;
    lv_obj_t* btnFaultReset;
    lv_obj_t* barProgress;
    lv_obj_t* chart;
    lv_chart_series_t* serTemp;
    lv_chart_series_t* serTarget;
    
    // Buttons
    lv_obj_t* btnStart;
    lv_obj_t* btnStop;
    lv_obj_t* btnPrograms;
    lv_obj_t* btnEdit;
    lv_obj_t* btnSchedules;
    lv_obj_t* lblSchedules;
    lv_obj_t* btnBack;
    lv_obj_t* btnLang;
    lv_obj_t* btnEditorSave;
    lv_obj_t* btnEditorBack;
    lv_obj_t* lblEditorSave;
    lv_obj_t* lblEditorBack;
    lv_obj_t* lblEditorTitle;
    lv_obj_t* editorList;

    // Modal
    lv_obj_t* modalStopConfirm;

    std::string selectedProgramName;
    std::string selectedProgramJson;

private:
    void initLVGL();
    void createScreens();
    void createProgramsScreen();
    void createEditorScreen();
    void updateChart(float temp, float target);
    bool loadEditorFromSelectedJson();
    void rebuildEditorList();
    void applyLanguageLabels();
    void updateStatusV2(const KilnState& state);
    void showProgramsV2();

    struct EditorStep {
        bool isHold = false;
        int targetC = 0;   // for RAMP
        int holdMin = 0;   // for HOLD
    };

    struct EditorRowControls {
        lv_obj_t *row;
        lv_obj_t *lblIdx, *lblType;
        lv_obj_t *groupRamp, *lblRampVal;
        lv_obj_t *groupHold, *lblHoldVal;
    };

    std::vector<EditorStep> editorSteps;
    std::vector<EditorRowControls> editorRows;

    struct EditorRowControls {
        lv_obj_t *row, *lblIdx, *btnType, *lblType, *groupRamp, *groupHold, *btnDel;
        // RAMP controls
        lv_obj_t *lblTarget, *btnRampMinus, *lblRampVal, *btnRampPlus;
        // HOLD controls
        lv_obj_t *lblHold, *btnHoldMinus, *lblHoldVal, *btnHoldPlus;
    };

    std::vector<EditorStep> editorSteps;
    std::vector<EditorRowControls> editorRows; {
        lv_obj_t* row;
        lv_obj_t* lblIdx;
        lv_obj_t* btnType;
        lv_obj_t* lblType;
        // ... other pointers
    };

    std::vector<EditorStep> editorSteps;
    std::vector<EditorRow> editorRows;

    bool isUa = true;
    lv_obj_t *langLabel = nullptr;
    lv_obj_t *lblCurrent = nullptr;
    lv_obj_t *lblTarget = nullptr;
    lv_obj_t *lblRemain = nullptr;
    lv_obj_t *lblSelectProgram = nullptr;
    lv_obj_t *lblStart = nullptr;
    lv_obj_t *lblBack = nullptr;
    lv_obj_t *lblProgramsTitle = nullptr;

    uint32_t lastSchedulesRevision = 0;

    unsigned long lastUpdate;
};

extern UIManager uiManager;

#endif
