#pragma once

#include <stdint.h>

enum class LedStripState : uint8_t {
  Initializing,
  WaitingForConnection,
  ConnectedIdle,
  Playing,
};

bool led_strip_init();
void led_strip_log_startup();
void led_strip_set_state(LedStripState state);
LedStripState led_strip_get_state();
void led_strip_update();
