#pragma once

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
constexpr uint32_t kPairingLedIntervalMs = 150;

}  // namespace AppConfig
