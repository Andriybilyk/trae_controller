#ifndef DISPLAY_GUI_H
#define DISPLAY_GUI_H

#include <Arduino.h>

void setupDisplay();
void updateDisplay(float currentTemp, float setpoint, String status, bool isFiring);

#endif // DISPLAY_GUI_H