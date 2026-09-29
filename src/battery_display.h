#pragma once

#include <Arduino.h>

// Initializes I2C and the SSD1306. Failure is non-fatal for the application.
bool battery_display_begin();

// Stores and renders battery data. Calls are rate-limited internally; call
// repeatedly while low is true so the non-blocking warning animation advances.
void battery_display_update(float voltage, uint8_t percent, bool low,
                            bool valid);

bool battery_display_is_available();
