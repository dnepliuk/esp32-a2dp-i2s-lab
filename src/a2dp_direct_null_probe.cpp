#include "a2dp_null_probe_common.h"

namespace {

using a2dp_null_probe::CountingNullAudioStream;

class DirectNullSink final : public BluetoothA2DPSink {
 public:
  explicit DirectNullSink(CountingNullAudioStream& output)
      : BluetoothA2DPSink(static_cast<audio_tools::AudioStream&>(output)) {}

 protected:
  void handle_audio_cfg(uint16_t event, void* parameter) override {
    BluetoothA2DPSink::handle_audio_cfg(event, parameter);
    a2dp_null_probe::logSbcConfiguration(parameter, codec_logged_);
  }

 private:
  bool codec_logged_ = false;
};

CountingNullAudioStream null_output;
DirectNullSink a2dp_sink(null_output);

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(250);

  Serial.println();
  Serial.println("ARDUINO_A2DP_TEST variant=direct-null-probe");
  Serial.println("Bluetooth name: Faital A2DP Direct Null");
  Serial.println(
      "Output: upstream BluetoothA2DPSink -> CountingNullAudioStream");
  Serial.println("I2S: disabled");
  Serial.println("Audio hardware: disabled");
  Serial.println("Additional queue: none");
  Serial.println("Stats interval: 5000 ms");

  a2dp_sink.set_sample_rate_callback(a2dp_null_probe::onSampleRate);
  a2dp_sink.set_on_audio_state_changed(a2dp_null_probe::onAudioState);
  a2dp_sink.start("Faital A2DP Direct Null");
}

void loop() { a2dp_null_probe::printDirectStats(); }
