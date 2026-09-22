#include "status_led.h"

#include <Arduino.h>

#include "app_config.h"

namespace StatusLed {
namespace {

State s_state = State::Initializing;
uint32_t s_stateStartedAt = 0;
bool s_outputOn = false;

void writeLed(bool on) {
  if (on == s_outputOn) {
    return;
  }
  s_outputOn = on;
  const bool level = AppConfig::kStatusLedActiveHigh ? on : !on;
  digitalWrite(AppConfig::kStatusLedPin, level ? HIGH : LOW);
}

bool errorPattern(uint32_t elapsed) {
  const uint32_t phase = elapsed % 1000U;
  return phase < 100U || (phase >= 200U && phase < 300U) ||
         (phase >= 400U && phase < 500U);
}

}  // namespace

void begin() {
  pinMode(AppConfig::kStatusLedPin, OUTPUT);
  s_state = State::Initializing;
  s_stateStartedAt = millis();
  s_outputOn = false;
  digitalWrite(AppConfig::kStatusLedPin,
               AppConfig::kStatusLedActiveHigh ? LOW : HIGH);
  update();
}

void setState(State state) {
  if (state == s_state) {
    return;
  }
  s_state = state;
  s_stateStartedAt = millis();
}

void update() {
  const uint32_t elapsed = millis() - s_stateStartedAt;
  bool on = false;

  switch (s_state) {
    case State::Initializing:
      on = (elapsed % 200U) < 100U;
      break;
    case State::WaitingForConnection:
      on = (elapsed % 1000U) < 500U;
      break;
    case State::ConnectedIdle:
      on = true;
      break;
    case State::Playing:
      on = (elapsed % 2000U) >= 100U;
      break;
    case State::InitializationError:
      on = errorPattern(elapsed);
      break;
  }

  writeLed(on);
}

}  // namespace StatusLed
