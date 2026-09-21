#include <Arduino.h>
#include <esp_idf_version.h>

#include "AudioTools.h"
#include "BluetoothA2DPSink.h"

I2SStream i2s;
BluetoothA2DPSink a2dp_sink(i2s);

namespace {
constexpr char kBluetoothName[] = "Faital A2DP Arduino";
}

void setup() {
  Serial.begin(115200);
  delay(250);

  Serial.println();
  Serial.println("ARDUINO_A2DP_TEST variant=minimal-direct");
  Serial.println("Bluetooth name: Faital A2DP Arduino");
  Serial.println("ESP32-A2DP: 1.8.11");
  Serial.println("AudioTools: 1.2.5");
  Serial.println("I2S: BCK=26 WS=25 DATA=22");
  Serial.printf("Arduino-ESP32: %d.%d.%d\n", ESP_ARDUINO_VERSION_MAJOR,
                ESP_ARDUINO_VERSION_MINOR, ESP_ARDUINO_VERSION_PATCH);
  Serial.printf("ESP-IDF: %s\n", esp_get_idf_version());
  Serial.printf("CPU frequency: %u MHz\n", getCpuFrequencyMhz());

  auto config = i2s.defaultConfig(TX_MODE);
  config.sample_rate = 44100;
  config.bits_per_sample = 16;
  config.channels = 2;
  config.i2s_format = I2S_STD_FORMAT;
  config.pin_bck = 26;
  config.pin_ws = 25;
  config.pin_data = 22;
  config.pin_data_rx = -1;
  i2s.begin(config);

  a2dp_sink.start(kBluetoothName);
  Serial.printf("Free heap after start: %u bytes\n", ESP.getFreeHeap());
}

void loop() {
  delay(1000);
}
