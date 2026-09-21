#include <Arduino.h>
#include <AudioTools.h>
#include <BluetoothA2DPSinkQueued.h>
#include <esp_timer.h>

#include <cstddef>
#include <cstdint>

#ifndef A2DP_POST_WRITE_DELAY_MS
#error "A2DP_POST_WRITE_DELAY_MS must be defined as 0 or 5"
#endif

#if A2DP_POST_WRITE_DELAY_MS != 0 && A2DP_POST_WRITE_DELAY_MS != 5
#error "This A/B test permits only A2DP_POST_WRITE_DELAY_MS=0 or 5"
#endif

namespace {

constexpr uint32_t kInitialSampleRate = 44100;
constexpr uint8_t kChannels = 2;
constexpr uint8_t kBitsPerSample = 16;
constexpr uint32_t kStatsIntervalMs = 5000;

#if A2DP_POST_WRITE_DELAY_MS == 5
constexpr char kVariant[] = "queued-fork-delay5";
constexpr char kBluetoothName[] = "Faital A2DP Fork D5";
constexpr char kPostWriteDelayLine[] = "Post-write delay: 5 ms";
constexpr char kExpectedBehavior[] = "upstream-compatible control";
#else
constexpr char kVariant[] = "queued-fork-delay0";
constexpr char kBluetoothName[] = "Faital A2DP Fork D0";
constexpr char kPostWriteDelayLine[] = "Post-write delay: 0 ms";
constexpr char kExpectedBehavior[] = "no artificial consumer pause";
#endif

struct Counters {
  uint64_t producer_calls = 0;
  uint64_t producer_offered = 0;
  uint64_t producer_accepted = 0;
  uint64_t producer_rejected = 0;
  uint64_t consumer_calls = 0;
  uint64_t consumer_requested = 0;
  uint64_t consumer_written = 0;
  uint64_t consumer_short_writes = 0;
  uint64_t consumer_errors = 0;
  uint64_t consumer_write_us_total = 0;
  uint64_t consumer_write_us_max = 0;
  uint64_t consumer_gap_us_max = 0;
  uint64_t consumer_last_start_us = 0;
};

struct DeferredDiagnostics {
  esp_a2d_audio_state_t audio_state = ESP_A2D_AUDIO_STATE_STOPPED;
  uint16_t sample_rate = 0;
  uint8_t sbc[4] = {};
  bool audio_state_pending = false;
  bool sample_rate_pending = false;
  bool sbc_pending = false;
};

portMUX_TYPE probe_mux = portMUX_INITIALIZER_UNLOCKED;
Counters counters;
DeferredDiagnostics diagnostics;

class CountedI2SStream final : public audio_tools::I2SStream {
 public:
  using audio_tools::I2SStream::write;

  size_t write(const uint8_t* data, size_t len) override {
    const uint64_t start_us = static_cast<uint64_t>(esp_timer_get_time());
    const size_t written = audio_tools::I2SStream::write(data, len);
    const uint64_t end_us = static_cast<uint64_t>(esp_timer_get_time());
    const uint64_t write_us = end_us - start_us;

    portENTER_CRITICAL(&probe_mux);
    if (counters.consumer_last_start_us != 0) {
      const uint64_t gap_us = start_us - counters.consumer_last_start_us;
      if (gap_us > counters.consumer_gap_us_max) {
        counters.consumer_gap_us_max = gap_us;
      }
    }
    counters.consumer_last_start_us = start_us;
    ++counters.consumer_calls;
    counters.consumer_requested += len;
    counters.consumer_written += written;
    counters.consumer_write_us_total += write_us;
    if (write_us > counters.consumer_write_us_max) {
      counters.consumer_write_us_max = write_us;
    }
    if (written != len) ++counters.consumer_short_writes;
    if (len != 0 && written == 0) ++counters.consumer_errors;
    portEXIT_CRITICAL(&probe_mux);

    return written;
  }
};

class ProbedQueuedSink final : public BluetoothA2DPSinkQueued {
 public:
  explicit ProbedQueuedSink(CountedI2SStream& output)
      : BluetoothA2DPSinkQueued(
            static_cast<audio_tools::AudioStream&>(output)) {}

 protected:
  size_t write_audio(const uint8_t* data, size_t size) override {
    const size_t accepted = BluetoothA2DPSinkQueued::write_audio(data, size);

    portENTER_CRITICAL(&probe_mux);
    ++counters.producer_calls;
    counters.producer_offered += size;
    counters.producer_accepted += accepted;
    counters.producer_rejected += accepted <= size ? size - accepted : 0;
    portEXIT_CRITICAL(&probe_mux);

    return accepted;
  }

  void handle_audio_cfg(uint16_t event, void* parameter) override {
    BluetoothA2DPSink::handle_audio_cfg(event, parameter);
    if (codec_seen_ || parameter == nullptr) return;

    const auto* a2d = static_cast<const esp_a2d_cb_param_t*>(parameter);
    if (a2d->audio_cfg.mcc.type != ESP_A2D_MCT_SBC) return;

    portENTER_CRITICAL(&probe_mux);
    for (size_t i = 0; i < sizeof(diagnostics.sbc); ++i) {
      diagnostics.sbc[i] = a2d->audio_cfg.mcc.cie.sbc[i];
    }
    diagnostics.sbc_pending = true;
    portEXIT_CRITICAL(&probe_mux);
    codec_seen_ = true;
  }

 private:
  bool codec_seen_ = false;
};

CountedI2SStream i2s;
ProbedQueuedSink a2dp_sink(i2s);

Counters takeCountersSnapshot() {
  Counters result;
  portENTER_CRITICAL(&probe_mux);
  result = counters;
  portEXIT_CRITICAL(&probe_mux);
  return result;
}

const char* audioStateName(esp_a2d_audio_state_t state) {
  switch (state) {
    case ESP_A2D_AUDIO_STATE_STARTED:
      return "started";
    case ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND:
      return "suspended";
    case ESP_A2D_AUDIO_STATE_STOPPED:
      return "stopped";
    default:
      return "unknown";
  }
}

const char* sbcSampleRate(uint8_t value) {
  if (value & 0x80) return "16000 Hz";
  if (value & 0x40) return "32000 Hz";
  if (value & 0x20) return "44100 Hz";
  if (value & 0x10) return "48000 Hz";
  return "unknown";
}

const char* sbcChannelMode(uint8_t value) {
  if (value & 0x08) return "mono";
  if (value & 0x04) return "dual-channel";
  if (value & 0x02) return "stereo";
  if (value & 0x01) return "joint-stereo";
  return "unknown";
}

const char* sbcBlockLength(uint8_t value) {
  if (value & 0x80) return "4";
  if (value & 0x40) return "8";
  if (value & 0x20) return "12";
  if (value & 0x10) return "16";
  return "unknown";
}

const char* sbcSubbands(uint8_t value) {
  if (value & 0x08) return "4";
  if (value & 0x04) return "8";
  return "unknown";
}

const char* sbcAllocationMethod(uint8_t value) {
  if (value & 0x02) return "SNR";
  if (value & 0x01) return "loudness";
  return "unknown";
}

void onSampleRate(uint16_t sample_rate) {
  portENTER_CRITICAL(&probe_mux);
  diagnostics.sample_rate = sample_rate;
  diagnostics.sample_rate_pending = true;
  portEXIT_CRITICAL(&probe_mux);
}

void onAudioState(esp_a2d_audio_state_t state, void*) {
  portENTER_CRITICAL(&probe_mux);
  diagnostics.audio_state = state;
  diagnostics.audio_state_pending = true;
  portEXIT_CRITICAL(&probe_mux);
}

void printDeferredDiagnostics() {
  DeferredDiagnostics pending;
  portENTER_CRITICAL(&probe_mux);
  pending = diagnostics;
  diagnostics.audio_state_pending = false;
  diagnostics.sample_rate_pending = false;
  diagnostics.sbc_pending = false;
  portEXIT_CRITICAL(&probe_mux);

  if (pending.sample_rate_pending) {
    const uint32_t expected_rate =
        static_cast<uint32_t>(pending.sample_rate) * kChannels *
        (kBitsPerSample / 8U);
    Serial.printf("A2DP negotiated sample rate: %u Hz\n",
                  static_cast<unsigned int>(pending.sample_rate));
    Serial.printf("Expected PCM rate: %lu B/s\n",
                  static_cast<unsigned long>(expected_rate));
    Serial.println("Sample-rate handling: upstream automatic");
  }

  if (pending.audio_state_pending) {
    Serial.printf("audio_state=%s\n", audioStateName(pending.audio_state));
  }

  if (pending.sbc_pending) {
    Serial.println("SBC configuration:");
    Serial.printf("sample_rate=%s\n", sbcSampleRate(pending.sbc[0]));
    Serial.printf("channel_mode=%s\n", sbcChannelMode(pending.sbc[0]));
    Serial.printf("block_length=%s\n", sbcBlockLength(pending.sbc[1]));
    Serial.printf("subbands=%s\n", sbcSubbands(pending.sbc[1]));
    Serial.printf("allocation_method=%s\n",
                  sbcAllocationMethod(pending.sbc[1]));
    Serial.printf("min_bitpool=%u\n",
                  static_cast<unsigned int>(pending.sbc[2]));
    Serial.printf("max_bitpool=%u\n",
                  static_cast<unsigned int>(pending.sbc[3]));
  }
}

uint64_t byteRate(uint64_t bytes, uint32_t elapsed_ms) {
  return elapsed_ms == 0 ? 0 : (bytes * 1000ULL) / elapsed_ms;
}

void printStats(const Counters& current, const Counters& previous,
                uint32_t elapsed_ms) {
  const uint64_t producer_calls_delta =
      current.producer_calls - previous.producer_calls;
  const uint64_t offered_delta =
      current.producer_offered - previous.producer_offered;
  const uint64_t accepted_delta =
      current.producer_accepted - previous.producer_accepted;
  const uint64_t rejected_delta =
      current.producer_rejected - previous.producer_rejected;
  const uint64_t consumer_calls_delta =
      current.consumer_calls - previous.consumer_calls;
  const uint64_t requested_delta =
      current.consumer_requested - previous.consumer_requested;
  const uint64_t written_delta =
      current.consumer_written - previous.consumer_written;
  const uint64_t write_us_delta =
      current.consumer_write_us_total - previous.consumer_write_us_total;
  const uint64_t write_us_avg = consumer_calls_delta == 0
                                    ? 0
                                    : write_us_delta / consumer_calls_delta;
  const int64_t pending_estimate =
      static_cast<int64_t>(current.producer_accepted) -
      static_cast<int64_t>(current.consumer_written);

  Serial.printf(
      "RATE stats: elapsed_ms=%lu, producer_calls=%llu (+%llu), "
      "offered=%llu (+%llu), accepted=%llu (+%llu), rejected=%llu (+%llu), "
      "producer_rate=%llu B/s, consumer_calls=%llu (+%llu), "
      "requested=%llu (+%llu), written=%llu (+%llu), "
      "consumer_rate=%llu B/s, pending_estimate=%lld B (approximate), "
      "short=%llu, errors=%llu, write_us_avg=%llu, write_us_max=%llu, "
      "consumer_gap_ms_max=%llu, free_heap=%u\n",
      static_cast<unsigned long>(elapsed_ms),
      static_cast<unsigned long long>(current.producer_calls),
      static_cast<unsigned long long>(producer_calls_delta),
      static_cast<unsigned long long>(current.producer_offered),
      static_cast<unsigned long long>(offered_delta),
      static_cast<unsigned long long>(current.producer_accepted),
      static_cast<unsigned long long>(accepted_delta),
      static_cast<unsigned long long>(current.producer_rejected),
      static_cast<unsigned long long>(rejected_delta),
      static_cast<unsigned long long>(byteRate(accepted_delta, elapsed_ms)),
      static_cast<unsigned long long>(current.consumer_calls),
      static_cast<unsigned long long>(consumer_calls_delta),
      static_cast<unsigned long long>(current.consumer_requested),
      static_cast<unsigned long long>(requested_delta),
      static_cast<unsigned long long>(current.consumer_written),
      static_cast<unsigned long long>(written_delta),
      static_cast<unsigned long long>(byteRate(written_delta, elapsed_ms)),
      static_cast<long long>(pending_estimate),
      static_cast<unsigned long long>(current.consumer_short_writes),
      static_cast<unsigned long long>(current.consumer_errors),
      static_cast<unsigned long long>(write_us_avg),
      static_cast<unsigned long long>(current.consumer_write_us_max),
      static_cast<unsigned long long>(current.consumer_gap_us_max / 1000ULL),
      static_cast<unsigned int>(ESP.getFreeHeap()));
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(250);

  Serial.println();
  Serial.printf("ARDUINO_A2DP_TEST variant=%s\n", kVariant);
  Serial.printf("Bluetooth name: %s\n", kBluetoothName);
  Serial.println("ESP32-A2DP upstream: v1.8.11");
  Serial.println(
      "ESP32-A2DP upstream commit: "
      "b6da4744286ca15b6f1dee607c077a4747294dd4");
  Serial.println("ESP32-A2DP source: local delay-configurable fork");
  Serial.println("Fork modification: configurable post-write delay only");
  Serial.println(kPostWriteDelayLine);
  Serial.printf("Expected behavior: %s\n", kExpectedBehavior);
  Serial.println("Queued implementation: upstream fork");
  Serial.println("I2S: Philips, 44100 Hz initial, s16le stereo");
  Serial.println("Pins: BCK=26 WS=25 DATA=22");
  Serial.println("APLL: disabled");
  Serial.println("Stats interval: 5000 ms");

  auto config = i2s.defaultConfig(TX_MODE);
  config.port_no = 0;
  config.is_master = true;
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
  const bool i2s_started = i2s.begin(config);
  Serial.printf("I2S begin: %s\n", i2s_started ? "success" : "failed");

  a2dp_sink.set_i2s_post_write_delay_ms(A2DP_POST_WRITE_DELAY_MS);
  a2dp_sink.set_output_active_by_state(false);
  a2dp_sink.set_sample_rate_callback(onSampleRate);
  a2dp_sink.set_on_audio_state_changed(onAudioState);
  a2dp_sink.start(kBluetoothName);
}

void loop() {
  static uint32_t last_stats_ms = millis();
  static Counters previous = takeCountersSnapshot();

  printDeferredDiagnostics();

  const uint32_t now_ms = millis();
  const uint32_t elapsed_ms = now_ms - last_stats_ms;
  if (elapsed_ms >= kStatsIntervalMs) {
    const Counters current = takeCountersSnapshot();
    printStats(current, previous, elapsed_ms);
    previous = current;
    last_stats_ms = now_ms;
  }

  delay(10);
}
