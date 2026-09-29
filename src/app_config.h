#pragma once

#include <Adafruit_NeoPixel.h>
#include <Arduino.h>

#ifndef ENCODER_REVERSED
#define ENCODER_REVERSED false
#endif

namespace AppConfig {

constexpr char kBluetoothName[] = "Faital Bluetooth Speaker";
constexpr uint32_t kSerialBaud = 115200;

constexpr int kI2sBckPin = 26;
constexpr int kI2sWsPin = 25;
constexpr int kI2sDataPin = 19;
constexpr uint32_t kInitialSampleRate = 44100;
constexpr int kBitsPerSample = 16;
constexpr int kChannels = 2;

constexpr int kStatusLedPin = 2;
constexpr bool kStatusLedActiveHigh = true;
constexpr uint32_t kStatusLedHeartbeatPeriodMs = 1000;
constexpr uint32_t kStatusLedHeartbeatOnMs = 50;

constexpr int kLedStripPin = 27;
constexpr uint16_t kLedStripCount = 8;
constexpr neoPixelType kLedStripColorOrder = NEO_GRB;
constexpr neoPixelType kLedStripProtocol = NEO_KHZ800;
constexpr uint8_t kLedStripMaxBrightness = 48;
constexpr uint8_t kLedStripMinVisibleBrightness = 12;
constexpr uint32_t kLedStripInitializingFrameMs = 100;
constexpr uint32_t kLedStripWaitingFrameMs = 150;
constexpr uint32_t kLedStripTransitionFrameMs = 40;
constexpr uint32_t kLedStripPlayingFrameMs = 40;
constexpr uint32_t kLedStripConnectedFadeMs = 600;
constexpr uint32_t kLedStripPlayingBreathPeriodMs = 2400;

struct RgbColor {
  uint8_t red;
  uint8_t green;
  uint8_t blue;
};

constexpr RgbColor kLedStripInitializingColor{255, 80, 0};
constexpr RgbColor kLedStripWaitingHeadColor{0, 48, 255};
constexpr RgbColor kLedStripWaitingTailColor{0, 12, 64};
constexpr uint16_t kLedStripWaitingTailOffset = 1;
constexpr RgbColor kLedStripConnectedColor{0, 96, 112};
constexpr RgbColor kLedStripPlayingColor{0, 220, 160};

constexpr int kEncoderAPin = 32;
constexpr int kEncoderBPin = 33;
constexpr int kEncoderSwitchPin = 18;
constexpr int64_t kEncoderCountsPerDetent = 2;
constexpr uint16_t kEncoderPcntFilter = 1023;
constexpr uint16_t kEncoderButtonDebounceMs = 40;

constexpr int kPreviousButtonPin = 13;
constexpr int kNextButtonPin = 14;
constexpr int kBluetoothButtonPin = 23;
constexpr bool kMediaButtonsActiveLow = true;
constexpr bool kMediaButtonsUseInternalPullup = true;
constexpr uint16_t kMediaButtonDebounceMs = 40;

constexpr uint8_t kVolumeMin = 0;
constexpr uint8_t kVolumeMax = 127;
constexpr uint8_t kVolumeStep = 4;
constexpr uint8_t kVolumeFallback = 64;

constexpr int kReconnectAttempts = 5;

// Battery pack measurement (3S2P Li-ion) through a 120 kOhm / 27 kOhm
// divider. GPIO34 is ADC1, so Bluetooth operation does not contend with ADC2.
constexpr int kBatteryAdcPin = 34;
constexpr uint8_t kBatteryAdcResolutionBits = 12;
constexpr adc_attenuation_t kBatteryAdcAttenuation = ADC_11db;
constexpr uint32_t kBatteryDividerHighOhm = 150000;
constexpr uint32_t kBatteryDividerLowOhm = 30000;
constexpr float kBatteryCalibrationGain = 0.9880f;
constexpr float kBatteryCalibrationOffsetV = 0.0f;
constexpr uint8_t kBatterySampleCount = 20;
constexpr uint8_t kBatteryTrimCountPerEnd = 2;
constexpr uint32_t kBatterySampleIntervalMs = 10;
constexpr uint32_t kBatteryPublishIntervalMs = 1000;
constexpr uint32_t kBatteryLogIntervalMs = 5000;
constexpr float kBatteryEmaAlpha = 0.20f;
constexpr float kBatteryValidMinV = 8.0f;
constexpr float kBatteryValidMaxV = 13.2f;
constexpr uint32_t kBatteryAdcMaxMillivolts = 3300;
constexpr float kBatteryLowThresholdV = 10.8f;
constexpr float kBatteryLowHysteresisV = 0.3f;

// Vbat = Vadc * ((Rhigh + Rlow) / Rlow) * calibration_gain
//        + calibration_offset.

constexpr int kOledSdaPin = 21;
constexpr int kOledSclPin = 22;
constexpr uint8_t kOledAddress = 0x3C;
constexpr uint8_t kOledWidth = 128;
constexpr uint8_t kOledHeight = 64;
constexpr uint32_t kDisplayUpdateIntervalMs = 1000;
constexpr uint32_t kDisplayLowBlinkIntervalMs = 500;

}  // namespace AppConfig
