#include "display_gui.h"
#include <TFT_eSPI.h> // Hardware-specific library

TFT_eSPI tft = TFT_eSPI(); 

// Colors
#define KILN_BG TFT_BLACK
#define KILN_TEXT TFT_WHITE
#define KILN_ACCENT TFT_ORANGE

void setupDisplay() {
    tft.init();
    tft.setRotation(1); // Landscape
    tft.fillScreen(KILN_BG);
    
    // Draw Header
    tft.fillRect(0, 0, 320, 30, TFT_DARKGREY);
    tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
    tft.drawString("TAP II Clone Controller", 10, 5, 2);
}

void updateDisplay(float currentTemp, float setpoint, String status, bool isFiring) {
    static float lastTemp = -999;
    static String lastStatus = "";

    // Only redraw if changed to prevent flicker
    if (abs(lastTemp - currentTemp) > 0.5) {
        // Clear old temp area
        tft.fillRect(20, 60, 140, 60, KILN_BG);
        
        tft.setTextColor(TFT_WHITE, KILN_BG);
        tft.setTextSize(2);
        tft.setCursor(20, 40);
        tft.print("Current Temp:");
        
        tft.setTextSize(4);
        tft.setCursor(20, 70);
        tft.print(currentTemp, 1);
        tft.print(" C");
        
        lastTemp = currentTemp;
    }

    if (lastStatus != status) {
        tft.fillRect(180, 60, 120, 60, KILN_BG);
        tft.setTextSize(2);
        tft.setCursor(180, 40);
        tft.print("Status:");
        
        tft.setTextColor(isFiring ? TFT_RED : TFT_GREEN, KILN_BG);
        tft.setCursor(180, 70);
        tft.print(status);
        lastStatus = status;
    }
}