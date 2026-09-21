#include "a2dp_null_probe_common.h"
#include "BluetoothA2DPSinkQueued.h"

namespace {

using a2dp_null_probe::CountingNullAudioStream;

class QueuedNullSink final : public BluetoothA2DPSinkQueued {
 public:
  explicit QueuedNullSink(CountingNullAudioStream& output)
      : BluetoothA2DPSinkQueued(
            static_cast<audio_tools::AudioStream&>(output)) {}

 protected:
  size_t write_audio(const uint8_t* data, size_t size) override {
    const size_t accepted = BluetoothA2DPSinkQueued::write_audio(data, size);

    portENTER_CRITICAL(&a2dp_null_probe::counter_mux);
    ++a2dp_null_probe::counters.producer_calls;
    a2dp_null_probe::counters.producer_offered +=
        static_cast<uint32_t>(size);
    a2dp_null_probe::counters.producer_accepted +=
        static_cast<uint32_t>(accepted);
    a2dp_null_probe::counters.producer_rejected +=
        static_cast<uint32_t>(accepted <= size ? size - accepted : 0);
    portEXIT_CRITICAL(&a2dp_null_probe::counter_mux);

    return accepted;
  }

  void handle_audio_cfg(uint16_t event, void* parameter) override {
    BluetoothA2DPSink::handle_audio_cfg(event, parameter);
    a2dp_null_probe::logSbcConfiguration(parameter, codec_logged_);
  }

 private:
  bool codec_logged_ = false;
};

CountingNullAudioStream null_output;
QueuedNullSink a2dp_sink(null_output);

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(250);

  Serial.println();
  Serial.println("ARDUINO_A2DP_TEST variant=queued-null-probe");
  Serial.println("Bluetooth name: Faital A2DP Queued Null");
  Serial.println(
      "Output: upstream BluetoothA2DPSinkQueued -> CountingNullAudioStream");
  Serial.println("I2S: disabled");
  Serial.println("Audio hardware: disabled");
  Serial.println("Queue configuration: upstream defaults");
  Serial.println("Stats interval: 5000 ms");

  a2dp_sink.set_output_active_by_state(false);
  a2dp_sink.set_sample_rate_callback(a2dp_null_probe::onSampleRate);
  a2dp_sink.set_on_audio_state_changed(a2dp_null_probe::onAudioState);
  a2dp_sink.start("Faital A2DP Queued Null");
}

void loop() { a2dp_null_probe::printQueuedStats(); }
