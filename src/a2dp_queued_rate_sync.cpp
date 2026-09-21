#include <Arduino.h>

#include "AudioTools.h"
#include "BluetoothA2DPSinkQueued.h"

I2SStream i2s;
BluetoothA2DPSinkQueued a2dp_sink(i2s);

namespace {

constexpr char kBluetoothName[] = "Faital A2DP Rate Sync";

void onSampleRate(uint16_t sample_rate) {
  const uint32_t expected_pcm_rate =
      static_cast<uint32_t>(sample_rate) * 2U * 2U;

  Serial.printf("A2DP negotiated sample rate: %u Hz\n",
                static_cast<unsigned int>(sample_rate));
  Serial.printf("Expected PCM rate: %lu B/s\n",
                static_cast<unsigned long>(expected_pcm_rate));
  Serial.println("Waiting for upstream automatic I2S update");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Info);
  delay(250);

  Serial.println();
  Serial.println("ARDUINO_A2DP_TEST variant=queued-rate-observe");
  Serial.println("Bluetooth name: Faital A2DP Rate Sync");
  Serial.println("Initial I2S sample rate: 44100 Hz");
  Serial.println("Sample-rate handling: upstream automatic");
  Serial.println("Queue configuration: upstream defaults");

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

  a2dp_sink.set_sample_rate_callback(onSampleRate);
  a2dp_sink.start(kBluetoothName);
}

void loop() {
  delay(1000);
}
