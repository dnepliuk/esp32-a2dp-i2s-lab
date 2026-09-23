#include "rotary_input.h"

#include <Arduino.h>
#include <ESP32Encoder.h>
#include <OneButton.h>

#include "app_config.h"
#include "media_control.h"

namespace {

ESP32Encoder s_encoder;
OneButton s_button;
int64_t s_processedCount = 0;
bool s_initialized = false;

void onButtonClick() { media_control_toggle_play_pause(); }

}  // namespace

void rotary_input_init() {
  ESP32Encoder::useInternalWeakPullResistors = puType::up;
  s_encoder.attachHalfQuad(AppConfig::kEncoderAPin, AppConfig::kEncoderBPin);
  s_encoder.setFilter(AppConfig::kEncoderPcntFilter);
  s_encoder.setCount(0);
  s_processedCount = 0;

  s_button.setup(AppConfig::kEncoderSwitchPin, INPUT_PULLUP, true);
  s_button.setDebounceMs(AppConfig::kEncoderButtonDebounceMs);
  s_button.attachClick(onButtonClick);
  s_initialized = true;

  Serial.print("ROTARY: A=GPIO");
  Serial.print(AppConfig::kEncoderAPin);
  Serial.print(" B=GPIO");
  Serial.print(AppConfig::kEncoderBPin);
  Serial.println(" COMMON=GND");
  Serial.print("ROTARY: button=GPIO");
  Serial.print(AppConfig::kEncoderSwitchPin);
  Serial.println(" active_low pullup=internal");
  Serial.println("ROTARY: PCNT encoder initialized");
}

void rotary_input_update() {
  if (!s_initialized) {
    return;
  }

  s_button.tick();

  const int64_t count = s_encoder.getCount();
  const int64_t delta = count - s_processedCount;
  const int64_t detents = delta / AppConfig::kEncoderCountsPerDetent;
  if (detents == 0) {
    return;
  }

  s_processedCount += detents * AppConfig::kEncoderCountsPerDetent;
  int64_t directedDetents = ENCODER_REVERSED ? -detents : detents;
  while (directedDetents > 0) {
    media_control_volume_up();
    --directedDetents;
  }
  while (directedDetents < 0) {
    media_control_volume_down();
    ++directedDetents;
  }
}
