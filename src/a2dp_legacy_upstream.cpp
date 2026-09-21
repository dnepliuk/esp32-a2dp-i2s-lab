#include <Arduino.h>
#include "BluetoothA2DPSink.h"
#include "esp_system.h"

BluetoothA2DPSink a2dp_sink;

void setup() {
  Serial.begin(115200);

  Serial.println("ARDUINO_A2DP_TEST variant=legacy-upstream");
  Serial.println("Bluetooth name: Faital A2DP Legacy");
  Serial.println("ESP32-A2DP: 1.8.11");
  Serial.println("Output: built-in ESP32-A2DP legacy I2S");
  Serial.println("AudioTools: not used");
  Serial.println("Queued sink: not used");
  Serial.println("Custom PCM pipeline: none");
  Serial.println("I2S pins: BCK=26 WS=25 DATA=22");
  Serial.println("I2S mode: library upstream default");
  Serial.println("Arduino-ESP32: 2.0.17");
  Serial.printf("ESP-IDF: %s\n", esp_get_idf_version());
  Serial.printf("CPU frequency: %u MHz\n", getCpuFrequencyMhz());

  const i2s_pin_config_t pins = {
      .mck_io_num = I2S_PIN_NO_CHANGE,
      .bck_io_num = 26,
      .ws_io_num = 25,
      .data_out_num = 22,
      .data_in_num = I2S_PIN_NO_CHANGE,
  };

  a2dp_sink.set_pin_config(pins);
  a2dp_sink.start("Faital A2DP Legacy");
}

void loop() {}
