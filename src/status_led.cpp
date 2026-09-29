#include "status_led.h"

#include <Arduino.h>

#include "app_config.h"

namespace StatusLed {
namespace {

uint32_t s_heartbeatStartedAt = 0;
bool s_outputOn = false;

void writeLed(bool on) {
  if (on == s_outputOn) {
    return;
  }
  s_outputOn = on;
  const bool level = AppConfig::kStatusLedActiveHigh ? on : !on;
  digitalWrite(AppConfig::kStatusLedPin, level ? HIGH : LOW);
}

}  // namespace

void begin() {
  pinMode(AppConfig::kStatusLedPin, OUTPUT);
  s_heartbeatStartedAt = millis();
  s_outputOn = false;
  digitalWrite(AppConfig::kStatusLedPin,
               AppConfig::kStatusLedActiveHigh ? LOW : HIGH);
  Serial.print("STATUS LED: GPIO=");
  Serial.print(AppConfig::kStatusLedPin);
  Serial.print(" mode=heartbeat period=");
  Serial.print(AppConfig::kStatusLedHeartbeatPeriodMs);
  Serial.print(" ms pulse=");
  Serial.print(AppConfig::kStatusLedHeartbeatOnMs);
  Serial.println(" ms");
  update();
}

void update() {
  const uint32_t phase =
      (millis() - s_heartbeatStartedAt) %
      AppConfig::kStatusLedHeartbeatPeriodMs;
  const bool on = phase < AppConfig::kStatusLedHeartbeatOnMs;
  writeLed(on);
}

}  // namespace StatusLed
