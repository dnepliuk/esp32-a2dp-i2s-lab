#include "led_strip.h"

#include <Adafruit_NeoPixel.h>
#include <Arduino.h>

#include "app_config.h"

namespace {

Adafruit_NeoPixel s_strip(
    AppConfig::kLedStripCount, AppConfig::kLedStripPin,
    AppConfig::kLedStripColorOrder + AppConfig::kLedStripProtocol);
uint32_t s_frame[AppConfig::kLedStripCount] = {};
uint32_t s_displayedFrame[AppConfig::kLedStripCount] = {};
LedStripState s_state = LedStripState::Initializing;
uint32_t s_stateStartedAt = 0;
uint32_t s_lastFrameAt = 0;
bool s_initialized = false;
bool s_forceRender = false;

const char* stateName(LedStripState state) {
  switch (state) {
    case LedStripState::Initializing:
      return "initializing";
    case LedStripState::WaitingForConnection:
      return "waiting";
    case LedStripState::ConnectedIdle:
      return "connected-idle";
    case LedStripState::Playing:
      return "playing";
  }
  return "unknown";
}

uint32_t color(const AppConfig::RgbColor& rgb, uint8_t scale = 255) {
  const uint8_t red =
      static_cast<uint8_t>((static_cast<uint16_t>(rgb.red) * scale) / 255U);
  const uint8_t green = static_cast<uint8_t>(
      (static_cast<uint16_t>(rgb.green) * scale) / 255U);
  const uint8_t blue = static_cast<uint8_t>(
      (static_cast<uint16_t>(rgb.blue) * scale) / 255U);
  return Adafruit_NeoPixel::Color(red, green, blue);
}

void clearFrame() {
  for (uint16_t i = 0; i < AppConfig::kLedStripCount; ++i) {
    s_frame[i] = 0;
  }
}

void renderInitializing(uint32_t elapsed) {
  clearFrame();
  const uint16_t position =
      (elapsed / AppConfig::kLedStripInitializingFrameMs) %
      AppConfig::kLedStripCount;
  s_frame[position] = color(AppConfig::kLedStripInitializingColor);
}

void renderWaiting(uint32_t elapsed) {
  clearFrame();
  const uint16_t position =
      (elapsed / AppConfig::kLedStripWaitingFrameMs) %
      AppConfig::kLedStripCount;
  const uint16_t tail =
      (position + AppConfig::kLedStripCount -
       AppConfig::kLedStripWaitingTailOffset) %
      AppConfig::kLedStripCount;
  s_frame[position] = color(AppConfig::kLedStripWaitingHeadColor);
  s_frame[tail] = color(AppConfig::kLedStripWaitingTailColor);
}

void renderConnectedIdle(uint32_t elapsed) {
  uint8_t scale = 255;
  if (elapsed < AppConfig::kLedStripConnectedFadeMs) {
    const uint32_t range =
        255U - AppConfig::kLedStripMinVisibleBrightness;
    scale = static_cast<uint8_t>(
        AppConfig::kLedStripMinVisibleBrightness +
        (range * elapsed) / AppConfig::kLedStripConnectedFadeMs);
  }

  const uint32_t pixel = color(AppConfig::kLedStripConnectedColor, scale);
  for (uint16_t i = 0; i < AppConfig::kLedStripCount; ++i) {
    s_frame[i] = pixel;
  }
}

void renderPlaying(uint32_t elapsed) {
  const uint32_t halfPeriod =
      AppConfig::kLedStripPlayingBreathPeriodMs / 2U;
  const uint32_t phase =
      elapsed % AppConfig::kLedStripPlayingBreathPeriodMs;
  const uint32_t ramp = phase < halfPeriod
                            ? phase
                            : AppConfig::kLedStripPlayingBreathPeriodMs - phase;
  const uint8_t triangle =
      static_cast<uint8_t>((ramp * 255U) / halfPeriod);
  const uint32_t range =
      255U - AppConfig::kLedStripMinVisibleBrightness;
  const uint8_t scale = static_cast<uint8_t>(
      AppConfig::kLedStripMinVisibleBrightness +
      (range * triangle) / 255U);
  const uint32_t pixel = color(AppConfig::kLedStripPlayingColor, scale);
  for (uint16_t i = 0; i < AppConfig::kLedStripCount; ++i) {
    s_frame[i] = pixel;
  }
}

uint32_t frameInterval() {
  switch (s_state) {
    case LedStripState::Initializing:
      return AppConfig::kLedStripInitializingFrameMs;
    case LedStripState::WaitingForConnection:
      return AppConfig::kLedStripWaitingFrameMs;
    case LedStripState::ConnectedIdle:
      return AppConfig::kLedStripTransitionFrameMs;
    case LedStripState::Playing:
      return AppConfig::kLedStripPlayingFrameMs;
  }
  return AppConfig::kLedStripPlayingFrameMs;
}

void showChangedPixels() {
  bool changed = false;
  for (uint16_t i = 0; i < AppConfig::kLedStripCount; ++i) {
    if (s_displayedFrame[i] != s_frame[i]) {
      s_strip.setPixelColor(i, s_frame[i]);
      s_displayedFrame[i] = s_frame[i];
      changed = true;
    }
  }
  if (changed) {
    s_strip.show();
  }
}

}  // namespace

bool led_strip_init() {
  s_strip.begin();
  s_initialized = s_strip.getPixels() != nullptr;
  s_strip.setBrightness(AppConfig::kLedStripMaxBrightness);
  s_strip.clear();
  s_strip.show();

  if (!s_initialized) {
    return false;
  }

  clearFrame();
  for (uint16_t i = 0; i < AppConfig::kLedStripCount; ++i) {
    s_displayedFrame[i] = 0;
  }
  s_state = LedStripState::Initializing;
  s_stateStartedAt = millis();
  s_lastFrameAt = s_stateStartedAt;
  s_forceRender = true;
  return true;
}

void led_strip_log_startup() {
  Serial.print("LED STRIP: GPIO=");
  Serial.print(AppConfig::kLedStripPin);
  Serial.print(" count=");
  Serial.print(AppConfig::kLedStripCount);
  Serial.print(" order=GRB brightness=");
  Serial.println(AppConfig::kLedStripMaxBrightness);

  if (!s_initialized) {
    Serial.println("LED STRIP: initialization failed; continuing without strip");
    return;
  }

  Serial.println("LED STRIP: Adafruit NeoPixel initialized");
  Serial.println("LED STRIP: state=initializing");
}

void led_strip_set_state(LedStripState state) {
  if (state == s_state) {
    return;
  }
  s_state = state;
  s_stateStartedAt = millis();
  s_forceRender = true;
  Serial.print("LED STRIP: state=");
  Serial.println(stateName(state));
}

LedStripState led_strip_get_state() { return s_state; }

void led_strip_update() {
  if (!s_initialized) {
    return;
  }

  const uint32_t now = millis();
  if (!s_forceRender && now - s_lastFrameAt < frameInterval()) {
    return;
  }
  s_forceRender = false;
  s_lastFrameAt = now;
  const uint32_t elapsed = now - s_stateStartedAt;

  switch (s_state) {
    case LedStripState::Initializing:
      renderInitializing(elapsed);
      break;
    case LedStripState::WaitingForConnection:
      renderWaiting(elapsed);
      break;
    case LedStripState::ConnectedIdle:
      renderConnectedIdle(elapsed);
      break;
    case LedStripState::Playing:
      renderPlaying(elapsed);
      break;
  }

  showChangedPixels();
}
