#include "battery_display.h"

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>
#include <stdio.h>

#include "app_config.h"

namespace {

Adafruit_SSD1306 s_display(AppConfig::kOledWidth, AppConfig::kOledHeight,
                           &Wire, -1);
bool s_available = false;
bool s_hasRendered = false;
bool s_lastValid = false;
bool s_lastLow = false;
bool s_lastBlinkOn = true;
uint8_t s_lastPercent = 0;
uint32_t s_lastRenderAt = 0;

bool oledResponds() {
  Wire.beginTransmission(AppConfig::kOledAddress);
  return Wire.endTransmission() == 0;
}

void drawBatteryIcon(uint8_t percent, bool visible) {
  if (!visible) {
    return;
  }

  constexpr int16_t kX = 91;
  constexpr int16_t kY = 7;
  constexpr int16_t kWidth = 32;
  constexpr int16_t kHeight = 18;
  constexpr int16_t kInnerWidth = kWidth - 4;
  s_display.drawRect(kX, kY, kWidth, kHeight, SSD1306_WHITE);
  s_display.fillRect(kX + kWidth, kY + 5, 3, 8, SSD1306_WHITE);

  const int16_t fillWidth =
      static_cast<int16_t>((static_cast<uint32_t>(percent) * kInnerWidth) /
                           100U);
  if (fillWidth > 0) {
    s_display.fillRect(kX + 2, kY + 2, fillWidth, kHeight - 4,
                       SSD1306_WHITE);
  }
}

void renderInvalid() {
  s_display.clearDisplay();
  s_display.setTextColor(SSD1306_WHITE);
  s_display.setTextSize(2);
  s_display.setCursor(21, 8);
  s_display.print("BATTERY");
  s_display.setCursor(34, 29);
  s_display.print("ERROR");
  s_display.setTextSize(1);
  s_display.setCursor(46, 53);
  s_display.print("--.- V");
  s_display.display();
}

void renderBattery(float voltage, uint8_t percent, bool low, bool blinkOn) {
  char percentText[6];
  char voltageText[12];
  snprintf(percentText, sizeof(percentText), "%u%%", percent);
  snprintf(voltageText, sizeof(voltageText), "%.2f V",
           static_cast<double>(voltage));

  s_display.clearDisplay();
  s_display.setTextColor(SSD1306_WHITE);
  s_display.setTextSize(3);
  s_display.setCursor(0, 2);
  s_display.print(percentText);
  drawBatteryIcon(percent, !low || blinkOn);
  s_display.setTextSize(2);
  s_display.setCursor(0, 31);
  s_display.print(voltageText);
  if (low) {
    s_display.setTextSize(1);
    s_display.setCursor(29, 55);
    s_display.print("LOW BATTERY");
  }
  s_display.display();
}

}  // namespace

bool battery_display_begin() {
  Serial.print("DISPLAY: SSD1306 ");
  Serial.print(AppConfig::kOledWidth);
  Serial.print('x');
  Serial.print(AppConfig::kOledHeight);
  Serial.print(" address=0x");
  Serial.print(AppConfig::kOledAddress, HEX);
  Serial.print(" SDA=");
  Serial.print(AppConfig::kOledSdaPin);
  Serial.print(" SCL=");
  Serial.println(AppConfig::kOledSclPin);

  Wire.begin(AppConfig::kOledSdaPin, AppConfig::kOledSclPin);
  if (!oledResponds() ||
      !s_display.begin(SSD1306_SWITCHCAPVCC, AppConfig::kOledAddress, false,
                       false)) {
    s_available = false;
    Serial.println("DISPLAY: initialization failed, continuing without OLED");
    return false;
  }

  s_available = true;
  s_hasRendered = false;
  s_lastRenderAt = 0;
  renderInvalid();
  s_hasRendered = true;
  s_lastRenderAt = millis();
  Serial.println("DISPLAY: initialized");
  return true;
}

void battery_display_update(float voltage, uint8_t percent, bool low,
                            bool valid) {
  if (!s_available) {
    return;
  }

  const uint32_t now = millis();
  const bool blinkOn =
      !low || ((now / AppConfig::kDisplayLowBlinkIntervalMs) % 2U) == 0U;
  const bool stateChanged = !s_hasRendered || valid != s_lastValid ||
                            low != s_lastLow || percent != s_lastPercent;
  const bool blinkChanged = low && blinkOn != s_lastBlinkOn;
  const bool intervalElapsed =
      static_cast<uint32_t>(now - s_lastRenderAt) >=
      AppConfig::kDisplayUpdateIntervalMs;
  if (!stateChanged && !blinkChanged && !intervalElapsed) {
    return;
  }

  if (valid) {
    renderBattery(voltage, percent, low, blinkOn);
  } else {
    renderInvalid();
  }

  s_hasRendered = true;
  s_lastValid = valid;
  s_lastLow = low;
  s_lastBlinkOn = blinkOn;
  s_lastPercent = percent;
  s_lastRenderAt = now;
}

bool battery_display_is_available() { return s_available; }
