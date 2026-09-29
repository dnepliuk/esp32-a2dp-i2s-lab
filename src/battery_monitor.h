#pragma once

#include <Arduino.h>

// Initializes the ADC1 input and resets the non-blocking filter state.
bool battery_monitor_begin();

// Performs at most one ADC conversion when its sampling deadline is due.
void battery_monitor_update(uint32_t now_ms);

// Returns true once for each newly published filtered result.
bool battery_monitor_has_new_sample();

// Accessors return the latest published result. Check validity before using
// voltage, percent, low state, or ADC millivolts as measurements.
float battery_monitor_voltage();
uint8_t battery_monitor_percent();
bool battery_monitor_is_low();
bool battery_monitor_is_valid();
uint32_t battery_monitor_adc_millivolts();
