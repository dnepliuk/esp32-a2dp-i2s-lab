#include <Arduino.h>

#include "app_config.h"
#include "audio_output.h"
#include "battery_display.h"
#include "battery_monitor.h"
#include "bluetooth_audio.h"
#include "button_input.h"
#include "led_strip.h"
#include "media_control.h"
#include "rotary_input.h"
#include "status_led.h"

namespace {

bool s_bluetoothInitialized = false;

void printStartupBanner() {
  Serial.println("FAITAL_SPEAKER firmware=arduino-production");
  Serial.print("Bluetooth name: ");
  Serial.println(AppConfig::kBluetoothName);
  Serial.println("ESP32-A2DP: 1.8.11");
  Serial.println("AudioTools: 1.2.5");
  Serial.println("I2S: Philips, 44100 Hz initial, s16le stereo");
  Serial.println("Pins: BCK=26 WS=25 DATA=19");
  Serial.println("Auto reconnect: enabled");
}

LedStripState currentLedStripState() {
  if (!s_bluetoothInitialized) {
    return LedStripState::Initializing;
  }
  if (!BluetoothAudio::isConnected()) {
    return LedStripState::WaitingForConnection;
  }
  if (BluetoothAudio::isPlaying()) {
    return LedStripState::Playing;
  }
  return LedStripState::ConnectedIdle;
}

}  // namespace

void setup() {
  pinMode(AppConfig::kLedStripPin, OUTPUT);
  digitalWrite(AppConfig::kLedStripPin, LOW);
  led_strip_init();

  Serial.begin(AppConfig::kSerialBaud);
  printStartupBanner();
  led_strip_log_startup();
  StatusLed::begin();
  led_strip_update();

  battery_monitor_begin();
  battery_display_begin();

  if (!AudioOutput::begin()) {
    Serial.println("Initialization error: I2S output failed");
    return;
  }

  BluetoothAudio::begin(AudioOutput::stream());
  s_bluetoothInitialized = true;
  rotary_input_init();
  button_input_init();
  led_strip_set_state(currentLedStripState());
  Serial.println("Initialization successful; waiting for Bluetooth connection");
}

void loop() {
  const uint32_t now = millis();
  battery_monitor_update(now);

  const bool newBatterySample = battery_monitor_has_new_sample();
  if (newBatterySample || battery_monitor_is_low()) {
    battery_display_update(
        battery_monitor_voltage(), battery_monitor_percent(),
        battery_monitor_is_low(), battery_monitor_is_valid());
  }

  button_input_update();
  BluetoothAudio::update();
  rotary_input_update();
  media_control_update();

  led_strip_set_state(currentLedStripState());
  led_strip_update();
  StatusLed::update();
  yield();
}
