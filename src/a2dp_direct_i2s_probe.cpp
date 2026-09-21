#include <Arduino.h>
#include <AudioTools.h>
#include <BluetoothA2DPSink.h>
#include <esp_timer.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace {

constexpr uint32_t kInitialSampleRate = 44100;
constexpr uint8_t kChannels = 2;
constexpr uint8_t kBitsPerSample = 16;
constexpr uint32_t kStatsIntervalMs = 5000;

struct ProbeMetrics {
  uint64_t callback_calls = 0;
  uint64_t offered_bytes = 0;
  uint64_t returned_bytes = 0;
  uint64_t rejected_bytes = 0;
  uint32_t last_size = 0;
  uint64_t callback_last_entry_us = 0;
  uint64_t callback_entry_gap_us_max = 0;
  uint64_t callback_gaps_gt_20ms = 0;
  uint64_t callback_gaps_gt_30ms = 0;
  uint64_t callback_gaps_gt_50ms = 0;
  uint64_t callback_gaps_gt_75ms = 0;
  uint64_t callback_gaps_gt_100ms = 0;
  uint64_t base_write_calls = 0;
  uint64_t base_write_us_total = 0;
  uint64_t base_write_us_max = 0;
  uint64_t base_write_gt_5ms = 0;
  uint64_t base_write_gt_10ms = 0;
  uint64_t base_write_gt_20ms = 0;
  uint64_t base_write_gt_30ms = 0;
  uint64_t base_write_gt_50ms = 0;
  uint64_t base_write_gt_100ms = 0;
  uint64_t i2s_calls = 0;
  uint64_t i2s_requested_bytes = 0;
  uint64_t i2s_written_bytes = 0;
  uint64_t i2s_short_writes = 0;
  uint64_t i2s_errors = 0;
  uint64_t i2s_write_us_total = 0;
  uint64_t i2s_write_us_max = 0;
  uint64_t i2s_last_entry_us = 0;
  uint64_t i2s_entry_gap_us_max = 0;
};

struct SharedProbeState {
  ProbeMetrics metrics;
  ProbeMetrics session_anchor;
  uint64_t session_start_us = 0;
  uint32_t session_id = 0;
  esp_a2d_audio_state_t audio_state = ESP_A2D_AUDIO_STATE_STOPPED;
  bool awaiting_first_pcm = true;
};

struct ProbeSnapshot {
  ProbeMetrics metrics;
  ProbeMetrics session_anchor;
  uint64_t session_start_us;
  uint32_t session_id;
  esp_a2d_audio_state_t audio_state;
};

portMUX_TYPE probe_mux = portMUX_INITIALIZER_UNLOCKED;
SharedProbeState probe_state;

void recordCallbackGapBuckets(ProbeMetrics& metrics, uint64_t gap_us) {
  if (gap_us > 20000) ++metrics.callback_gaps_gt_20ms;
  if (gap_us > 30000) ++metrics.callback_gaps_gt_30ms;
  if (gap_us > 50000) ++metrics.callback_gaps_gt_50ms;
  if (gap_us > 75000) ++metrics.callback_gaps_gt_75ms;
  if (gap_us > 100000) ++metrics.callback_gaps_gt_100ms;
}

void recordBaseWriteBuckets(ProbeMetrics& metrics, uint64_t duration_us) {
  if (duration_us > 5000) ++metrics.base_write_gt_5ms;
  if (duration_us > 10000) ++metrics.base_write_gt_10ms;
  if (duration_us > 20000) ++metrics.base_write_gt_20ms;
  if (duration_us > 30000) ++metrics.base_write_gt_30ms;
  if (duration_us > 50000) ++metrics.base_write_gt_50ms;
  if (duration_us > 100000) ++metrics.base_write_gt_100ms;
}

class CountedI2SStream final : public audio_tools::I2SStream {
 public:
  using audio_tools::I2SStream::write;

  size_t write(const uint8_t* data, size_t len) override {
    const uint64_t entry_us = static_cast<uint64_t>(esp_timer_get_time());
    const size_t written = audio_tools::I2SStream::write(data, len);
    const uint64_t duration_us =
        static_cast<uint64_t>(esp_timer_get_time()) - entry_us;

    portENTER_CRITICAL(&probe_mux);
    ProbeMetrics& metrics = probe_state.metrics;
    if (metrics.i2s_last_entry_us != 0) {
      const uint64_t gap_us = entry_us - metrics.i2s_last_entry_us;
      metrics.i2s_entry_gap_us_max =
          std::max(metrics.i2s_entry_gap_us_max, gap_us);
    }
    metrics.i2s_last_entry_us = entry_us;
    ++metrics.i2s_calls;
    metrics.i2s_requested_bytes += len;
    metrics.i2s_written_bytes += written;
    if (written != len) ++metrics.i2s_short_writes;
    if (len != 0 && written == 0) ++metrics.i2s_errors;
    metrics.i2s_write_us_total += duration_us;
    metrics.i2s_write_us_max =
        std::max(metrics.i2s_write_us_max, duration_us);
    portEXIT_CRITICAL(&probe_mux);

    return written;
  }
};

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
  if (value & 0x02) return "snr";
  if (value & 0x01) return "loudness";
  return "unknown";
}

void logSbcConfiguration(void* parameter, bool& already_logged) {
  if (already_logged || parameter == nullptr) return;
  const auto* a2d = static_cast<const esp_a2d_cb_param_t*>(parameter);
  if (a2d->audio_cfg.mcc.type != ESP_A2D_MCT_SBC) return;

  const uint8_t* sbc = a2d->audio_cfg.mcc.cie.sbc;
  Serial.println("SBC configuration:");
  Serial.printf("sample_rate=%s\n", sbcSampleRate(sbc[0]));
  Serial.printf("channel_mode=%s\n", sbcChannelMode(sbc[0]));
  Serial.printf("block_length=%s\n", sbcBlockLength(sbc[1]));
  Serial.printf("subbands=%s\n", sbcSubbands(sbc[1]));
  Serial.printf("allocation_method=%s\n", sbcAllocationMethod(sbc[1]));
  Serial.printf("min_bitpool=%u\n", static_cast<unsigned int>(sbc[2]));
  Serial.printf("max_bitpool=%u\n", static_cast<unsigned int>(sbc[3]));
  already_logged = true;
}

class DirectProbeSink final : public BluetoothA2DPSink {
 public:
  explicit DirectProbeSink(CountedI2SStream& output)
      : BluetoothA2DPSink(static_cast<audio_tools::AudioStream&>(output)) {}

 protected:
  size_t write_audio(const uint8_t* data, size_t size) override {
    const uint64_t entry_us = static_cast<uint64_t>(esp_timer_get_time());

    portENTER_CRITICAL(&probe_mux);
    ProbeMetrics& metrics = probe_state.metrics;
    if (probe_state.awaiting_first_pcm) {
      probe_state.session_anchor = metrics;
      probe_state.session_start_us = entry_us;
      ++probe_state.session_id;
      probe_state.awaiting_first_pcm = false;
      metrics.callback_last_entry_us = 0;
      metrics.i2s_last_entry_us = 0;
    }
    if (metrics.callback_last_entry_us != 0) {
      const uint64_t gap_us = entry_us - metrics.callback_last_entry_us;
      metrics.callback_entry_gap_us_max =
          std::max(metrics.callback_entry_gap_us_max, gap_us);
      recordCallbackGapBuckets(metrics, gap_us);
    }
    metrics.callback_last_entry_us = entry_us;
    ++metrics.callback_calls;
    metrics.offered_bytes += size;
    metrics.last_size = static_cast<uint32_t>(size);
    portEXIT_CRITICAL(&probe_mux);

    const size_t returned = BluetoothA2DPSink::write_audio(data, size);
    const uint64_t duration_us =
        static_cast<uint64_t>(esp_timer_get_time()) - entry_us;
    const uint64_t rejected = returned < size ? size - returned : 0;

    portENTER_CRITICAL(&probe_mux);
    ProbeMetrics& updated = probe_state.metrics;
    updated.returned_bytes += returned;
    updated.rejected_bytes += rejected;
    ++updated.base_write_calls;
    updated.base_write_us_total += duration_us;
    updated.base_write_us_max =
        std::max(updated.base_write_us_max, duration_us);
    recordBaseWriteBuckets(updated, duration_us);
    portEXIT_CRITICAL(&probe_mux);

    return returned;
  }

  void handle_audio_cfg(uint16_t event, void* parameter) override {
    BluetoothA2DPSink::handle_audio_cfg(event, parameter);
    logSbcConfiguration(parameter, codec_logged_);
  }

 private:
  bool codec_logged_ = false;
};

CountedI2SStream i2s;

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

void onAudioState(esp_a2d_audio_state_t state, void*) {
  portENTER_CRITICAL(&probe_mux);
  probe_state.audio_state = state;
  probe_state.awaiting_first_pcm = true;
  probe_state.metrics.callback_last_entry_us = 0;
  probe_state.metrics.i2s_last_entry_us = 0;
  portEXIT_CRITICAL(&probe_mux);

  Serial.printf("audio_state=%s\n", audioStateName(state));
}

void onSampleRate(uint16_t sample_rate) {
  static bool sample_rate_logged = false;
  if (sample_rate_logged) return;

  const uint32_t expected_pcm_rate =
      static_cast<uint32_t>(sample_rate) * kChannels *
      (kBitsPerSample / 8U);
  Serial.printf("A2DP negotiated sample rate: %u Hz\n",
                static_cast<unsigned int>(sample_rate));
  Serial.printf("Expected PCM rate: %lu B/s\n",
                static_cast<unsigned long>(expected_pcm_rate));
  Serial.println("Sample-rate handling: upstream automatic");
  sample_rate_logged = true;
}

ProbeSnapshot takeSnapshot() {
  ProbeSnapshot snapshot;
  portENTER_CRITICAL(&probe_mux);
  snapshot.metrics = probe_state.metrics;
  snapshot.session_anchor = probe_state.session_anchor;
  snapshot.session_start_us = probe_state.session_start_us;
  snapshot.session_id = probe_state.session_id;
  snapshot.audio_state = probe_state.audio_state;
  portEXIT_CRITICAL(&probe_mux);
  return snapshot;
}

uint64_t rateBytesPerSecond(uint64_t bytes, uint64_t elapsed_us) {
  return elapsed_us == 0 ? 0 : (bytes * 1000000ULL) / elapsed_us;
}

void printStats(const ProbeSnapshot& snapshot, const ProbeMetrics& previous,
                uint64_t elapsed_us, bool partial) {
  const ProbeMetrics& current = snapshot.metrics;
  const uint64_t callback_delta =
      current.callback_calls - previous.callback_calls;
  const uint64_t offered_delta =
      current.offered_bytes - previous.offered_bytes;
  const uint64_t returned_delta =
      current.returned_bytes - previous.returned_bytes;
  const uint64_t rejected_delta =
      current.rejected_bytes - previous.rejected_bytes;
  const uint64_t base_calls_delta =
      current.base_write_calls - previous.base_write_calls;
  const uint64_t base_time_delta =
      current.base_write_us_total - previous.base_write_us_total;
  const uint64_t i2s_calls_delta = current.i2s_calls - previous.i2s_calls;
  const uint64_t i2s_requested_delta =
      current.i2s_requested_bytes - previous.i2s_requested_bytes;
  const uint64_t i2s_written_delta =
      current.i2s_written_bytes - previous.i2s_written_bytes;
  const uint64_t i2s_time_delta =
      current.i2s_write_us_total - previous.i2s_write_us_total;
  const uint64_t base_write_us_avg =
      base_calls_delta == 0 ? 0 : base_time_delta / base_calls_delta;
  const uint64_t i2s_write_us_avg =
      i2s_calls_delta == 0 ? 0 : i2s_time_delta / i2s_calls_delta;

  Serial.printf(
      "DIRECT I2S stats: elapsed_ms=%llu, partial=%s, "
      "callback_calls=%llu (+%llu), offered=%llu (+%llu), "
      "returned=%llu (+%llu), rejected=%llu (+%llu), "
      "callback_rate=%llu B/s, last_size=%u, "
      "entry_gap_ms_max=%llu, gaps_gt_20ms=%llu, "
      "gaps_gt_30ms=%llu, gaps_gt_50ms=%llu, "
      "gaps_gt_75ms=%llu, gaps_gt_100ms=%llu, "
      "base_write_us_avg=%llu, base_write_us_max=%llu, "
      "base_write_gt_5ms=%llu, base_write_gt_10ms=%llu, "
      "base_write_gt_20ms=%llu, base_write_gt_30ms=%llu, "
      "base_write_gt_50ms=%llu, base_write_gt_100ms=%llu, "
      "i2s_calls=%llu (+%llu), i2s_requested=%llu (+%llu), "
      "i2s_written=%llu (+%llu), i2s_rate=%llu B/s, "
      "i2s_short=%llu, i2s_errors=%llu, "
      "i2s_write_us_avg=%llu, i2s_write_us_max=%llu, "
      "i2s_entry_gap_ms_max=%llu, free_heap=%u\n",
      static_cast<unsigned long long>(elapsed_us / 1000ULL),
      partial ? "yes" : "no",
      static_cast<unsigned long long>(current.callback_calls),
      static_cast<unsigned long long>(callback_delta),
      static_cast<unsigned long long>(current.offered_bytes),
      static_cast<unsigned long long>(offered_delta),
      static_cast<unsigned long long>(current.returned_bytes),
      static_cast<unsigned long long>(returned_delta),
      static_cast<unsigned long long>(current.rejected_bytes),
      static_cast<unsigned long long>(rejected_delta),
      static_cast<unsigned long long>(
          rateBytesPerSecond(returned_delta, elapsed_us)),
      current.last_size,
      static_cast<unsigned long long>(current.callback_entry_gap_us_max /
                                      1000ULL),
      static_cast<unsigned long long>(current.callback_gaps_gt_20ms),
      static_cast<unsigned long long>(current.callback_gaps_gt_30ms),
      static_cast<unsigned long long>(current.callback_gaps_gt_50ms),
      static_cast<unsigned long long>(current.callback_gaps_gt_75ms),
      static_cast<unsigned long long>(current.callback_gaps_gt_100ms),
      static_cast<unsigned long long>(base_write_us_avg),
      static_cast<unsigned long long>(current.base_write_us_max),
      static_cast<unsigned long long>(current.base_write_gt_5ms),
      static_cast<unsigned long long>(current.base_write_gt_10ms),
      static_cast<unsigned long long>(current.base_write_gt_20ms),
      static_cast<unsigned long long>(current.base_write_gt_30ms),
      static_cast<unsigned long long>(current.base_write_gt_50ms),
      static_cast<unsigned long long>(current.base_write_gt_100ms),
      static_cast<unsigned long long>(current.i2s_calls),
      static_cast<unsigned long long>(i2s_calls_delta),
      static_cast<unsigned long long>(current.i2s_requested_bytes),
      static_cast<unsigned long long>(i2s_requested_delta),
      static_cast<unsigned long long>(current.i2s_written_bytes),
      static_cast<unsigned long long>(i2s_written_delta),
      static_cast<unsigned long long>(
          rateBytesPerSecond(i2s_written_delta, elapsed_us)),
      static_cast<unsigned long long>(current.i2s_short_writes),
      static_cast<unsigned long long>(current.i2s_errors),
      static_cast<unsigned long long>(i2s_write_us_avg),
      static_cast<unsigned long long>(current.i2s_write_us_max),
      static_cast<unsigned long long>(current.i2s_entry_gap_us_max /
                                      1000ULL),
      static_cast<unsigned int>(ESP.getFreeHeap()));
}

}  // namespace

void setup() {
  Serial.begin(115200);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Info);
  delay(250);

  Serial.println();
  Serial.println("ARDUINO_A2DP_TEST variant=direct-i2s-probe");
  Serial.println("Bluetooth name: Faital A2DP Direct I2S");
  Serial.println("Sink: upstream BluetoothA2DPSink");
  Serial.println("Output: direct -> CountedI2SStream");
  Serial.println("I2S: Philips, 44100 Hz, s16le stereo");
  Serial.println("Pins: BCK=26 WS=25 DATA=22");
  Serial.println("Output lifecycle: always active");
  Serial.println("APLL: disabled");
  Serial.println("Additional queue: none");
  Serial.println("Stats interval: 5000 ms");

  auto config = i2s.defaultConfig(TX_MODE);
  config.sample_rate = kInitialSampleRate;
  config.bits_per_sample = kBitsPerSample;
  config.channels = kChannels;
  config.i2s_format = I2S_STD_FORMAT;
  config.pin_bck = 26;
  config.pin_ws = 25;
  config.pin_data = 22;
  config.pin_data_rx = -1;
  config.use_apll = false;
  config.auto_clear = true;
  config.buffer_count = 6;
  config.buffer_size = 512;
  config.port_no = 0;
  const bool i2s_started = i2s.begin(config);
  (void)i2s_started;

  static DirectProbeSink a2dp_sink(i2s);
  a2dp_sink.set_output_active_by_state(false);
  a2dp_sink.set_sample_rate_callback(onSampleRate);
  a2dp_sink.set_on_audio_state_changed(onAudioState);
  a2dp_sink.start("Faital A2DP Direct I2S");
}

void loop() {
  static uint32_t observed_session_id = 0;
  static uint64_t last_report_us = 0;
  static ProbeMetrics previous;
  static bool first_report = true;

  const ProbeSnapshot snapshot = takeSnapshot();
  if (snapshot.audio_state != ESP_A2D_AUDIO_STATE_STARTED ||
      snapshot.session_id == 0) {
    delay(10);
    return;
  }

  if (snapshot.session_id != observed_session_id) {
    observed_session_id = snapshot.session_id;
    previous = snapshot.session_anchor;
    last_report_us = snapshot.session_start_us;
    first_report = true;
  }

  const uint64_t now_us = static_cast<uint64_t>(esp_timer_get_time());
  const uint64_t elapsed_us = now_us - last_report_us;
  if (elapsed_us < static_cast<uint64_t>(kStatsIntervalMs) * 1000ULL) {
    delay(10);
    return;
  }

  printStats(snapshot, previous, elapsed_us, first_report);
  previous = snapshot.metrics;
  last_report_us = now_us;
  first_report = false;
}
