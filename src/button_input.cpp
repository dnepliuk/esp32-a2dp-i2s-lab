#include "button_input.h"

#include <Arduino.h>
#include <OneButton.h>

#include "app_config.h"
#include "bluetooth_audio.h"
#include "media_control.h"

namespace {

OneButton s_previousButton;
OneButton s_nextButton;
OneButton s_bluetoothButton;
bool s_initialized = false;

void onPreviousClick() {
  Serial.println("BUTTONS: previous pressed");
  media_control_previous();
}

void onNextClick() {
  Serial.println("BUTTONS: next pressed");
  media_control_next();
}

void onBluetoothClick() {
  Serial.println("BUTTONS: bluetooth pressed");
  BluetoothAudio::enterPairingMode();
}

void configureButton(OneButton& button, int pin, callbackFunction callback) {
  const uint8_t mode = AppConfig::kMediaButtonsUseInternalPullup
                           ? INPUT_PULLUP
                           : INPUT;
  button.setup(pin, mode, AppConfig::kMediaButtonsActiveLow);
  button.setDebounceMs(AppConfig::kMediaButtonDebounceMs);
  button.attachClick(callback);
}

}  // namespace

bool button_input_init() {
  configureButton(s_previousButton, AppConfig::kPreviousButtonPin,
                  onPreviousClick);
  configureButton(s_nextButton, AppConfig::kNextButtonPin, onNextClick);
  configureButton(s_bluetoothButton, AppConfig::kBluetoothButtonPin,
                  onBluetoothClick);
  s_initialized = true;

  Serial.print("BUTTONS: Previous=GPIO");
  Serial.print(AppConfig::kPreviousButtonPin);
  Serial.print(" Next=GPIO");
  Serial.print(AppConfig::kNextButtonPin);
  Serial.print(" Bluetooth=GPIO");
  Serial.println(AppConfig::kBluetoothButtonPin);
  Serial.print("BUTTONS: active_low pullup=internal debounce=");
  Serial.print(AppConfig::kMediaButtonDebounceMs);
  Serial.println("ms");
  Serial.println("BUTTONS: initialized");
  return true;
}

void button_input_update() {
  if (!s_initialized) {
    return;
  }

  s_previousButton.tick();
  s_nextButton.tick();
  s_bluetoothButton.tick();
}
