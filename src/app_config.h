#pragma once

#include <Arduino.h>

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

constexpr int kReconnectAttempts = 5;

}  // namespace AppConfig
