#include <Arduino.h>

#include "app_config.h"
#include "audio_output.h"
#include "bluetooth_audio.h"
#include "button_input.h"
#include "media_control.h"
#include "rotary_input.h"
#include "status_led.h"

namespace {

void printStartupBanner() {
  Serial.println("FAITAL_SPEAKER firmware=arduino-production");
  Serial.print("Bluetooth name: ");
  Serial.println(AppConfig::kBluetoothName);
  Serial.println("ESP32-A2DP: 1.8.11");
  Serial.println("AudioTools: 1.2.5");
  Serial.println("I2S: Philips, 44100 Hz initial, s16le stereo");
  Serial.println("Pins: BCK=26 WS=25 DATA=19");
  Serial.println("Status LED: GPIO2 active-high");
  Serial.println("Auto reconnect: enabled");
}

StatusLed::State currentLedState() {
  if (BluetoothAudio::isPairingMode()) {
    return StatusLed::State::Pairing;
  }
  if (BluetoothAudio::isPlaying()) {
    return StatusLed::State::Playing;
  }
  if (BluetoothAudio::isConnected()) {
    return StatusLed::State::ConnectedIdle;
  }
  return StatusLed::State::WaitingForConnection;
}

}  // namespace

void setup() {
  Serial.begin(AppConfig::kSerialBaud);
  StatusLed::begin();
  printStartupBanner();

  if (!AudioOutput::begin()) {
    Serial.println("Initialization error: I2S output failed");
    StatusLed::setState(StatusLed::State::InitializationError);
    return;
  }

  BluetoothAudio::begin(AudioOutput::stream());
  rotary_input_init();
  button_input_init();
  StatusLed::setState(StatusLed::State::WaitingForConnection);
  Serial.println("Initialization successful; waiting for Bluetooth connection");
}

void loop() {
  button_input_update();
  BluetoothAudio::update();
  rotary_input_update();
  media_control_update();

  if (AudioOutput::isReady()) {
    StatusLed::setState(currentLedState());
  }
  StatusLed::update();
  yield();
}
