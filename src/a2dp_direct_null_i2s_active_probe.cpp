#include "a2dp_null_probe_common.h"

namespace {

using a2dp_null_probe::CountingNullAudioStream;

constexpr uint32_t kInitialSampleRate = 44100;
constexpr uint8_t kChannels = 2;
constexpr uint8_t kBitsPerSample = 16;

struct DeferredDiagnostics {
  bool sample_rate_pending = false;
  uint16_t sample_rate = 0;
  bool audio_state_pending = false;
  esp_a2d_audio_state_t audio_state = ESP_A2D_AUDIO_STATE_STOPPED;
  bool sbc_pending = false;
  uint8_t sbc[4] = {};
};

audio_tools::I2SStream idle_i2s;
CountingNullAudioStream null_output;
DeferredDiagnostics deferred;

void setObservedFormat(uint32_t sample_rate) {
  a2dp_null_probe::counters.sample_rate = sample_rate;
  a2dp_null_probe::counters.channels = kChannels;
  a2dp_null_probe::counters.bits_per_sample = kBitsPerSample;
}

void onSampleRate(uint16_t sample_rate) {
  portENTER_CRITICAL(&a2dp_null_probe::counter_mux);
  setObservedFormat(sample_rate);
  deferred.sample_rate = sample_rate;
  deferred.sample_rate_pending = true;
  portEXIT_CRITICAL(&a2dp_null_probe::counter_mux);
}

void onAudioState(esp_a2d_audio_state_t state, void*) {
  portENTER_CRITICAL(&a2dp_null_probe::counter_mux);
  a2dp_null_probe::counters.audio_state = static_cast<uint32_t>(state);
  deferred.audio_state = state;
  deferred.audio_state_pending = true;
  portEXIT_CRITICAL(&a2dp_null_probe::counter_mux);
}

class DirectNullSink final : public BluetoothA2DPSink {
 public:
  explicit DirectNullSink(CountingNullAudioStream& output)
      : BluetoothA2DPSink(static_cast<audio_tools::AudioStream&>(output)) {}

 protected:
  void handle_audio_cfg(uint16_t event, void* parameter) override {
    BluetoothA2DPSink::handle_audio_cfg(event, parameter);
    if (codec_captured_ || parameter == nullptr) return;

    const auto* a2d = static_cast<const esp_a2d_cb_param_t*>(parameter);
    if (a2d->audio_cfg.mcc.type != ESP_A2D_MCT_SBC) return;

    portENTER_CRITICAL(&a2dp_null_probe::counter_mux);
    for (size_t index = 0; index < sizeof(deferred.sbc); ++index) {
      deferred.sbc[index] = a2d->audio_cfg.mcc.cie.sbc[index];
    }
    deferred.sbc_pending = true;
    portEXIT_CRITICAL(&a2dp_null_probe::counter_mux);
    codec_captured_ = true;
  }

 private:
  bool codec_captured_ = false;
};

void printDeferredDiagnostics() {
  DeferredDiagnostics current;

  portENTER_CRITICAL(&a2dp_null_probe::counter_mux);
  current = deferred;
  deferred.sample_rate_pending = false;
  deferred.audio_state_pending = false;
  deferred.sbc_pending = false;
  portEXIT_CRITICAL(&a2dp_null_probe::counter_mux);

  if (current.sample_rate_pending) {
    const uint32_t expected_pcm_rate =
        static_cast<uint32_t>(current.sample_rate) * kChannels *
        (kBitsPerSample / 8U);
    Serial.printf("A2DP negotiated sample rate: %u Hz\n",
                  static_cast<unsigned int>(current.sample_rate));
    Serial.printf("Expected PCM rate: %lu B/s\n",
                  static_cast<unsigned long>(expected_pcm_rate));
  }

  if (current.sbc_pending) {
    Serial.println("SBC configuration:");
    Serial.printf("sample_rate=%s\n",
                  a2dp_null_probe::sbcSampleRate(current.sbc[0]));
    Serial.printf("channel_mode=%s\n",
                  a2dp_null_probe::sbcChannelMode(current.sbc[0]));
    Serial.printf("block_length=%s\n",
                  a2dp_null_probe::sbcBlockLength(current.sbc[1]));
    Serial.printf("subbands=%s\n",
                  a2dp_null_probe::sbcSubbands(current.sbc[1]));
    Serial.printf("allocation_method=%s\n",
                  a2dp_null_probe::sbcAllocationMethod(current.sbc[1]));
    Serial.printf("min_bitpool=%u\n",
                  static_cast<unsigned int>(current.sbc[2]));
    Serial.printf("max_bitpool=%u\n",
                  static_cast<unsigned int>(current.sbc[3]));
  }

  if (current.audio_state_pending) {
    Serial.printf(
        "audio_state=%s\n",
        a2dp_null_probe::audioStateName(
            static_cast<uint32_t>(current.audio_state)));
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(250);

  Serial.println();
  Serial.println(
      "ARDUINO_A2DP_TEST variant=direct-null-i2s-active-probe");
  Serial.println("Bluetooth name: Faital A2DP Null I2S");
  Serial.println("Bluetooth output: CountingNullAudioStream");
  Serial.println("I2S state: initialized separately, no PCM writes");
  Serial.println("I2S: Philips, 44100 Hz, s16le stereo");
  Serial.println("Pins: BCK=26 WS=25 DATA=22");
  Serial.println(
      "I2S DMA: buffer_count=6 buffer_size=512 auto_clear=true");
  Serial.println("APLL: disabled");
  Serial.println("Additional audio queue: none");
  Serial.println("Stats interval: 5000 ms");

  auto config = idle_i2s.defaultConfig(TX_MODE);
  config.sample_rate = kInitialSampleRate;
  config.bits_per_sample = kBitsPerSample;
  config.channels = kChannels;
  config.i2s_format = I2S_STD_FORMAT;
  config.pin_bck = 26;
  config.pin_ws = 25;
  config.pin_data = 22;
  config.pin_data_rx = -1;
  config.auto_clear = true;
  config.buffer_count = 6;
  config.buffer_size = 512;
  config.use_apll = false;
  config.port_no = 0;

  const bool i2s_started = idle_i2s.begin(config);
  Serial.printf("Idle I2S begin: %s\n", i2s_started ? "success" : "failed");

  portENTER_CRITICAL(&a2dp_null_probe::counter_mux);
  setObservedFormat(kInitialSampleRate);
  portEXIT_CRITICAL(&a2dp_null_probe::counter_mux);

  static DirectNullSink a2dp_sink(null_output);
  a2dp_sink.set_sample_rate_callback(onSampleRate);
  a2dp_sink.set_on_audio_state_changed(onAudioState);
  a2dp_sink.start("Faital A2DP Null I2S");
}

void loop() {
  printDeferredDiagnostics();
  a2dp_null_probe::printDirectStats();
}
