#include "a2dp_null_probe_common.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cmath>
#include <cstddef>
#include <cstdint>

#ifndef A2DP_I2S_LOAD_TONE
#error "A2DP_I2S_LOAD_TONE must be defined as 0 or 1"
#endif

#if A2DP_I2S_LOAD_TONE != 0 && A2DP_I2S_LOAD_TONE != 1
#error "A2DP_I2S_LOAD_TONE must be 0 (silence) or 1 (1000 Hz tone)"
#endif

namespace {

using a2dp_null_probe::CountingNullAudioStream;

constexpr uint32_t kSampleRate = 44100;
constexpr uint8_t kChannels = 2;
constexpr uint8_t kBitsPerSample = 16;
constexpr size_t kBytesPerFrame = 4;
constexpr size_t kBlockFrames = 441;
constexpr size_t kBlockSamples = kBlockFrames * kChannels;
constexpr size_t kBlockBytes = kBlockFrames * kBytesPerFrame;
constexpr uint32_t kStatsIntervalUs = 5000000U;
constexpr uint32_t kWriterStackBytes = 4096;
constexpr UBaseType_t kWriterPriority = 2;
constexpr BaseType_t kWriterCore = 1;
constexpr uint32_t kToneFrequency = 1000;
constexpr int16_t kTonePeak = 29490;
constexpr double kTwoPi = 6.28318530717958647692;

static_assert(kBlockBytes == 1764, "PCM block must be exactly 10 ms");
static_assert(kBlockBytes % kBytesPerFrame == 0,
              "PCM block must contain complete stereo frames");
static_assert(kBlockSamples * sizeof(int16_t) == kBlockBytes,
              "PCM block must be signed 16-bit interleaved stereo");

#if A2DP_I2S_LOAD_TONE
constexpr char kVariant[] = "direct-null-i2s-tone-load-probe";
constexpr char kBluetoothName[] = "Faital A2DP Null I2S Tone";
constexpr char kI2SLoadLine[] = "I2S load: independent continuous 1000 Hz PCM";
constexpr char kStatsLabel[] = "TONE";
constexpr char kWriterLabel[] = "tone";
#else
constexpr char kVariant[] = "direct-null-i2s-silence-load-probe";
constexpr char kBluetoothName[] = "Faital A2DP Null I2S Load";
constexpr char kI2SLoadLine[] = "I2S load: independent continuous zero PCM";
constexpr char kStatsLabel[] = "SILENCE";
constexpr char kWriterLabel[] = "silence";
#endif

struct WriterCounters {
  uint64_t written_bytes = 0;
  uint64_t write_calls = 0;
  uint64_t write_us_total = 0;
  uint32_t partial_writes = 0;
  uint32_t zero_writes = 0;
  uint32_t errors = 0;
  uint32_t write_us_max = 0;
  uint32_t gap_us_max = 0;
  uint32_t last_start_us = 0;
  uint32_t task_start_us = 0;
};

struct DeferredDiagnostics {
  bool sample_rate_pending = false;
  uint16_t sample_rate = 0;
  bool audio_state_pending = false;
  esp_a2d_audio_state_t audio_state = ESP_A2D_AUDIO_STATE_STOPPED;
  bool sbc_pending = false;
  uint8_t sbc[4] = {};
};

portMUX_TYPE writer_mux = portMUX_INITIALIZER_UNLOCKED;
WriterCounters writer_counters;
TaskHandle_t writer_task_handle = nullptr;
alignas(4) int16_t pcm_block[kBlockSamples] = {};

audio_tools::I2SStream silence_i2s;
CountingNullAudioStream null_output;
DeferredDiagnostics deferred;

void preparePcmBlock() {
#if A2DP_I2S_LOAD_TONE
  for (size_t frame = 0; frame < kBlockFrames; ++frame) {
    const double phase =
        kTwoPi * static_cast<double>(kToneFrequency) *
        static_cast<double>(frame) / static_cast<double>(kSampleRate);
    const int16_t sample = static_cast<int16_t>(
        std::lround(static_cast<double>(kTonePeak) * std::sin(phase)));
    pcm_block[frame * kChannels] = sample;
    pcm_block[frame * kChannels + 1] = sample;
  }
#endif
}

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

void i2sSilenceWriter(void*) {
  portENTER_CRITICAL(&writer_mux);
  writer_counters.task_start_us =
      static_cast<uint32_t>(esp_timer_get_time());
  portEXIT_CRITICAL(&writer_mux);

  for (;;) {
    size_t offset = 0;
    while (offset < kBlockBytes) {
      const size_t remaining = kBlockBytes - offset;
      const uint32_t start_us =
          static_cast<uint32_t>(esp_timer_get_time());
      const auto* block_bytes =
          reinterpret_cast<const uint8_t*>(pcm_block);
      const size_t written = silence_i2s.write(block_bytes + offset, remaining);
      const uint32_t write_us =
          static_cast<uint32_t>(esp_timer_get_time()) - start_us;

      portENTER_CRITICAL(&writer_mux);
      const uint32_t previous_start_us = writer_counters.last_start_us;
      if (previous_start_us != 0) {
        const uint32_t gap_us = start_us - previous_start_us;
        if (gap_us > writer_counters.gap_us_max) {
          writer_counters.gap_us_max = gap_us;
        }
      }
      writer_counters.last_start_us = start_us;
      ++writer_counters.write_calls;
      writer_counters.write_us_total += write_us;
      if (write_us > writer_counters.write_us_max) {
        writer_counters.write_us_max = write_us;
      }

      if (written == 0) {
        ++writer_counters.zero_writes;
        ++writer_counters.errors;
      } else if (written > remaining) {
        ++writer_counters.errors;
      } else {
        if (written < remaining) {
          ++writer_counters.partial_writes;
        }
        writer_counters.written_bytes += written;
        offset += written;
      }
      portEXIT_CRITICAL(&writer_mux);
    }
  }
}

WriterCounters takeWriterSnapshot() {
  WriterCounters snapshot;
  portENTER_CRITICAL(&writer_mux);
  snapshot = writer_counters;
  portEXIT_CRITICAL(&writer_mux);
  return snapshot;
}

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

void printWriterStats() {
  static WriterCounters previous;
  static uint32_t previous_report_us = 0;
  static bool reporting_started = false;

  const uint32_t now_us = static_cast<uint32_t>(esp_timer_get_time());
  const WriterCounters current = takeWriterSnapshot();
  if (current.task_start_us == 0) return;

  if (!reporting_started) {
    previous = {};
    previous_report_us = current.task_start_us;
    reporting_started = true;
  }

  const uint32_t elapsed_us = now_us - previous_report_us;
  if (elapsed_us < kStatsIntervalUs) return;

  const uint64_t interval_written =
      current.written_bytes - previous.written_bytes;
  const uint64_t interval_calls =
      current.write_calls - previous.write_calls;
  const uint64_t interval_write_us =
      current.write_us_total - previous.write_us_total;
  const uint32_t byte_rate = static_cast<uint32_t>(
      (interval_written * 1000000ULL) / elapsed_us);
  const uint32_t write_us_avg =
      interval_calls == 0
          ? 0
          : static_cast<uint32_t>(interval_write_us / interval_calls);
  const UBaseType_t stack_min =
      writer_task_handle == nullptr
          ? 0
          : uxTaskGetStackHighWaterMark(writer_task_handle);

  Serial.printf(
      "%s I2S: elapsed_ms=%lu, written=%llu, interval=%llu, "
      "rate=%lu B/s, calls=%llu, partial=%lu, zero=%lu, errors=%lu, "
      "write_us_avg=%lu, write_us_max=%lu, gap_ms_max=%lu, "
      "stack_min=%lu, free_heap=%lu\n",
      kStatsLabel, static_cast<unsigned long>(elapsed_us / 1000U),
      static_cast<unsigned long long>(current.written_bytes),
      static_cast<unsigned long long>(interval_written),
      static_cast<unsigned long>(byte_rate),
      static_cast<unsigned long long>(current.write_calls),
      static_cast<unsigned long>(current.partial_writes),
      static_cast<unsigned long>(current.zero_writes),
      static_cast<unsigned long>(current.errors),
      static_cast<unsigned long>(write_us_avg),
      static_cast<unsigned long>(current.write_us_max),
      static_cast<unsigned long>(current.gap_us_max / 1000U),
      static_cast<unsigned long>(stack_min),
      static_cast<unsigned long>(ESP.getFreeHeap()));

  previous = current;
  previous_report_us = now_us;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(250);

  Serial.println();
  Serial.printf("ARDUINO_A2DP_TEST variant=%s\n", kVariant);
  Serial.printf("Bluetooth name: %s\n", kBluetoothName);
  Serial.println("Bluetooth output: CountingNullAudioStream");
  Serial.println("Bluetooth PCM to I2S: no");
  Serial.println(kI2SLoadLine);
  Serial.println("I2S writer: AudioTools I2SStream");
#if A2DP_I2S_LOAD_TONE
  Serial.println("Tone: 1000 Hz");
  Serial.println("Tone amplitude: 90%");
  Serial.println("Tone channels: left=right");
  Serial.println("Tone block: 441 frames / 1764 bytes / 10 ms");
#endif
  Serial.println("I2S: Philips, 44100 Hz, s16le stereo");
  Serial.println("Pins: BCK=26 WS=25 DATA=22");
  Serial.println("DMA: buffer_count=6 buffer_size=512 auto_clear=true");
#if !A2DP_I2S_LOAD_TONE
  Serial.println("Silence block: 441 frames / 1764 bytes / 10 ms");
#endif
  Serial.println("Writer task: core=1 priority=2 stack=4096");
  Serial.println("APLL: disabled");
  Serial.println("Additional A2DP queue: none");
  Serial.println("Stats interval: 5000 ms");

  preparePcmBlock();

  auto config = silence_i2s.defaultConfig(TX_MODE);
  config.sample_rate = kSampleRate;
  config.bits_per_sample = kBitsPerSample;
  config.channels = kChannels;
  config.i2s_format = I2S_STD_FORMAT;
  config.is_master = true;
  config.pin_bck = 26;
  config.pin_ws = 25;
  config.pin_data = 22;
  config.pin_data_rx = -1;
  config.auto_clear = true;
  config.buffer_count = 6;
  config.buffer_size = 512;
  config.use_apll = false;
  config.port_no = 0;

  const bool i2s_started = silence_i2s.begin(config);
  Serial.printf("I2S begin: %s\n", i2s_started ? "success" : "failed");

  BaseType_t writer_result = pdFAIL;
  if (i2s_started) {
    writer_result = xTaskCreatePinnedToCore(
        i2sSilenceWriter, "i2s_silence_writer", kWriterStackBytes, nullptr,
        kWriterPriority, &writer_task_handle, kWriterCore);
  }
  Serial.printf("I2S %s writer: %s\n", kWriterLabel,
                writer_result == pdPASS ? "started" : "failed");

  portENTER_CRITICAL(&a2dp_null_probe::counter_mux);
  setObservedFormat(kSampleRate);
  portEXIT_CRITICAL(&a2dp_null_probe::counter_mux);

  static DirectNullSink a2dp_sink(null_output);
  a2dp_sink.set_sample_rate_callback(onSampleRate);
  a2dp_sink.set_on_audio_state_changed(onAudioState);
  a2dp_sink.start(kBluetoothName);
}

void loop() {
  printDeferredDiagnostics();
  printWriterStats();
  a2dp_null_probe::printDirectStats();
}
