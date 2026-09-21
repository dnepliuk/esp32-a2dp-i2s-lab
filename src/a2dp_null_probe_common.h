#pragma once

#include <Arduino.h>
#include <esp_timer.h>

#include "AudioTools.h"
#include "BluetoothA2DPSink.h"

namespace a2dp_null_probe {

constexpr uint32_t kStatsIntervalUs = 5000000U;

struct Counters {
  volatile uint32_t write_calls = 0;
  volatile uint32_t pcm_bytes = 0;
  volatile uint32_t last_write_size = 0;
  volatile uint32_t first_pcm_us = 0;
  volatile uint32_t last_pcm_us = 0;
  volatile uint32_t current_gap_us = 0;
  volatile uint32_t max_gap_us = 0;
  volatile uint32_t gaps_gt_20ms = 0;
  volatile uint32_t gaps_gt_30ms = 0;
  volatile uint32_t gaps_gt_50ms = 0;
  volatile uint32_t gaps_gt_75ms = 0;
  volatile uint32_t gaps_gt_100ms = 0;
  volatile uint32_t gaps_gt_200ms = 0;
  volatile uint32_t sample_rate = 0;
  volatile uint32_t channels = 0;
  volatile uint32_t bits_per_sample = 0;
  volatile uint32_t audio_state = ESP_A2D_AUDIO_STATE_STOPPED;
  volatile uint32_t producer_calls = 0;
  volatile uint32_t producer_offered = 0;
  volatile uint32_t producer_accepted = 0;
  volatile uint32_t producer_rejected = 0;
};

struct Snapshot {
  uint32_t write_calls;
  uint32_t pcm_bytes;
  uint32_t last_write_size;
  uint32_t first_pcm_us;
  uint32_t last_pcm_us;
  uint32_t current_gap_us;
  uint32_t max_gap_us;
  uint32_t gaps_gt_20ms;
  uint32_t gaps_gt_30ms;
  uint32_t gaps_gt_50ms;
  uint32_t gaps_gt_75ms;
  uint32_t gaps_gt_100ms;
  uint32_t gaps_gt_200ms;
  uint32_t sample_rate;
  uint32_t channels;
  uint32_t bits_per_sample;
  uint32_t audio_state;
  uint32_t producer_calls;
  uint32_t producer_offered;
  uint32_t producer_accepted;
  uint32_t producer_rejected;
};

static portMUX_TYPE counter_mux = portMUX_INITIALIZER_UNLOCKED;
static Counters counters;
static Snapshot previous_snapshot = {};
static uint32_t previous_report_us = 0;
static bool reporting_started = false;
static bool first_report = true;

class CountingNullAudioStream final : public audio_tools::AudioStream {
 public:
  size_t write(const uint8_t* data, size_t len) override {
    (void)data;
    const uint32_t now_us = static_cast<uint32_t>(esp_timer_get_time());

    portENTER_CRITICAL(&counter_mux);
    const uint32_t prior_us = counters.last_pcm_us;
    if (counters.first_pcm_us == 0) {
      counters.first_pcm_us = now_us;
    }
    if (prior_us != 0) {
      const uint32_t gap_us = now_us - prior_us;
      counters.current_gap_us = gap_us;
      if (gap_us > counters.max_gap_us) counters.max_gap_us = gap_us;
      if (gap_us > 20000U) ++counters.gaps_gt_20ms;
      if (gap_us > 30000U) ++counters.gaps_gt_30ms;
      if (gap_us > 50000U) ++counters.gaps_gt_50ms;
      if (gap_us > 75000U) ++counters.gaps_gt_75ms;
      if (gap_us > 100000U) ++counters.gaps_gt_100ms;
      if (gap_us > 200000U) ++counters.gaps_gt_200ms;
    }
    counters.last_pcm_us = now_us;
    counters.last_write_size = static_cast<uint32_t>(len);
    ++counters.write_calls;
    counters.pcm_bytes += static_cast<uint32_t>(len);
    portEXIT_CRITICAL(&counter_mux);

    return len;
  }

  size_t write(uint8_t value) override { return write(&value, 1); }

  void setAudioInfo(audio_tools::AudioInfo info) override {
    audio_tools::AudioStream::setAudioInfo(info);
    portENTER_CRITICAL(&counter_mux);
    counters.sample_rate = static_cast<uint32_t>(info.sample_rate);
    counters.channels = static_cast<uint32_t>(info.channels);
    counters.bits_per_sample = static_cast<uint32_t>(info.bits_per_sample);
    portEXIT_CRITICAL(&counter_mux);
  }

  int available() override { return 0; }
  int availableForWrite() override { return 0x7fffffff; }
  operator bool() override { return true; }
};

static Snapshot takeSnapshot() {
  Snapshot result;
  portENTER_CRITICAL(&counter_mux);
  result.write_calls = counters.write_calls;
  result.pcm_bytes = counters.pcm_bytes;
  result.last_write_size = counters.last_write_size;
  result.first_pcm_us = counters.first_pcm_us;
  result.last_pcm_us = counters.last_pcm_us;
  result.current_gap_us = counters.current_gap_us;
  result.max_gap_us = counters.max_gap_us;
  result.gaps_gt_20ms = counters.gaps_gt_20ms;
  result.gaps_gt_30ms = counters.gaps_gt_30ms;
  result.gaps_gt_50ms = counters.gaps_gt_50ms;
  result.gaps_gt_75ms = counters.gaps_gt_75ms;
  result.gaps_gt_100ms = counters.gaps_gt_100ms;
  result.gaps_gt_200ms = counters.gaps_gt_200ms;
  result.sample_rate = counters.sample_rate;
  result.channels = counters.channels;
  result.bits_per_sample = counters.bits_per_sample;
  result.audio_state = counters.audio_state;
  result.producer_calls = counters.producer_calls;
  result.producer_offered = counters.producer_offered;
  result.producer_accepted = counters.producer_accepted;
  result.producer_rejected = counters.producer_rejected;
  portEXIT_CRITICAL(&counter_mux);
  return result;
}

static const char* audioStateName(uint32_t state) {
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

static void onAudioState(esp_a2d_audio_state_t state, void*) {
  portENTER_CRITICAL(&counter_mux);
  counters.audio_state = static_cast<uint32_t>(state);
  portEXIT_CRITICAL(&counter_mux);
  Serial.printf("audio_state=%s\n", audioStateName(state));
}

static void onSampleRate(uint16_t sample_rate) {
  const uint32_t expected_pcm_rate =
      static_cast<uint32_t>(sample_rate) * 2U * 2U;
  Serial.printf("A2DP negotiated sample rate: %u Hz\n",
                static_cast<unsigned int>(sample_rate));
  Serial.printf("Expected PCM rate: %lu B/s\n",
                static_cast<unsigned long>(expected_pcm_rate));
}

static const char* sbcSampleRate(uint8_t value) {
  if (value & 0x80) return "16000 Hz";
  if (value & 0x40) return "32000 Hz";
  if (value & 0x20) return "44100 Hz";
  if (value & 0x10) return "48000 Hz";
  return "unknown";
}

static const char* sbcChannelMode(uint8_t value) {
  if (value & 0x08) return "mono";
  if (value & 0x04) return "dual-channel";
  if (value & 0x02) return "stereo";
  if (value & 0x01) return "joint-stereo";
  return "unknown";
}

static const char* sbcBlockLength(uint8_t value) {
  if (value & 0x80) return "4";
  if (value & 0x40) return "8";
  if (value & 0x20) return "12";
  if (value & 0x10) return "16";
  return "unknown";
}

static const char* sbcSubbands(uint8_t value) {
  if (value & 0x08) return "4";
  if (value & 0x04) return "8";
  return "unknown";
}

static const char* sbcAllocationMethod(uint8_t value) {
  if (value & 0x02) return "SNR";
  if (value & 0x01) return "loudness";
  return "unknown";
}

static void logSbcConfiguration(void* parameter, bool& already_logged) {
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

static uint32_t expectedRate(const Snapshot& snapshot) {
  const uint32_t bytes_per_sample = (snapshot.bits_per_sample + 7U) / 8U;
  return snapshot.sample_rate * snapshot.channels * bytes_per_sample;
}

static bool reportReady(uint32_t now_us, Snapshot& current,
                        uint32_t& interval_us) {
  current = takeSnapshot();
  if (current.first_pcm_us == 0) return false;

  if (!reporting_started) {
    previous_snapshot = {};
    previous_report_us = current.first_pcm_us;
    reporting_started = true;
  }

  interval_us = now_us - previous_report_us;
  return interval_us >= kStatsIntervalUs;
}

static void finishReport(const Snapshot& current, uint32_t now_us) {
  previous_snapshot = current;
  previous_report_us = now_us;
  first_report = false;
}

static void printDirectStats() {
  const uint32_t now_us = static_cast<uint32_t>(esp_timer_get_time());
  Snapshot current;
  uint32_t interval_us;
  if (!reportReady(now_us, current, interval_us)) {
    delay(10);
    return;
  }

  const uint32_t calls_delta =
      current.write_calls - previous_snapshot.write_calls;
  const uint32_t bytes_delta = current.pcm_bytes - previous_snapshot.pcm_bytes;
  const uint32_t pcm_rate = static_cast<uint32_t>(
      (static_cast<uint64_t>(bytes_delta) * 1000000ULL) / interval_us);
  const uint32_t expected_rate = expectedRate(current);
  const uint32_t rate_percent = expected_rate == 0
                                    ? 0
                                    : static_cast<uint32_t>(
                                          (static_cast<uint64_t>(pcm_rate) *
                                           100ULL) /
                                          expected_rate);

  Serial.printf(
      "NULL direct: elapsed_ms=%lu, partial=%s, calls=%lu (+%lu), bytes=%lu "
      "(+%lu), pcm_rate=%lu B/s, expected_rate=%lu B/s, rate_percent=%lu%%, "
      "last_size=%lu, no_pcm_now_ms=%lu, current_gap_ms=%lu, max_gap_ms=%lu, "
      "gaps_gt_20ms=%lu, gaps_gt_30ms=%lu, gaps_gt_50ms=%lu, "
      "gaps_gt_75ms=%lu, gaps_gt_100ms=%lu, gaps_gt_200ms=%lu, "
      "format=%luHz/%luch/%lubit, audio_state=%s, free_heap=%lu\n",
      static_cast<unsigned long>((now_us - current.first_pcm_us) / 1000U),
      first_report ? "yes" : "no",
      static_cast<unsigned long>(current.write_calls),
      static_cast<unsigned long>(calls_delta),
      static_cast<unsigned long>(current.pcm_bytes),
      static_cast<unsigned long>(bytes_delta),
      static_cast<unsigned long>(pcm_rate),
      static_cast<unsigned long>(expected_rate),
      static_cast<unsigned long>(rate_percent),
      static_cast<unsigned long>(current.last_write_size),
      static_cast<unsigned long>((now_us - current.last_pcm_us) / 1000U),
      static_cast<unsigned long>(current.current_gap_us / 1000U),
      static_cast<unsigned long>(current.max_gap_us / 1000U),
      static_cast<unsigned long>(current.gaps_gt_20ms),
      static_cast<unsigned long>(current.gaps_gt_30ms),
      static_cast<unsigned long>(current.gaps_gt_50ms),
      static_cast<unsigned long>(current.gaps_gt_75ms),
      static_cast<unsigned long>(current.gaps_gt_100ms),
      static_cast<unsigned long>(current.gaps_gt_200ms),
      static_cast<unsigned long>(current.sample_rate),
      static_cast<unsigned long>(current.channels),
      static_cast<unsigned long>(current.bits_per_sample),
      audioStateName(current.audio_state),
      static_cast<unsigned long>(ESP.getFreeHeap()));

  finishReport(current, now_us);
}

static void printQueuedStats() {
  const uint32_t now_us = static_cast<uint32_t>(esp_timer_get_time());
  Snapshot current;
  uint32_t interval_us;
  if (!reportReady(now_us, current, interval_us)) {
    delay(10);
    return;
  }

  const uint32_t producer_calls_delta =
      current.producer_calls - previous_snapshot.producer_calls;
  const uint32_t offered_delta =
      current.producer_offered - previous_snapshot.producer_offered;
  const uint32_t accepted_delta =
      current.producer_accepted - previous_snapshot.producer_accepted;
  const uint32_t rejected_delta =
      current.producer_rejected - previous_snapshot.producer_rejected;
  const uint32_t consumer_calls_delta =
      current.write_calls - previous_snapshot.write_calls;
  const uint32_t consumed_delta =
      current.pcm_bytes - previous_snapshot.pcm_bytes;
  const uint32_t producer_rate = static_cast<uint32_t>(
      (static_cast<uint64_t>(accepted_delta) * 1000000ULL) / interval_us);
  const uint32_t consumer_rate = static_cast<uint32_t>(
      (static_cast<uint64_t>(consumed_delta) * 1000000ULL) / interval_us);
  const uint32_t pending_estimate =
      current.producer_accepted >= current.pcm_bytes
          ? current.producer_accepted - current.pcm_bytes
          : 0;

  Serial.printf(
      "NULL queued: elapsed_ms=%lu, partial=%s, producer_calls=%lu (+%lu), "
      "offered=%lu (+%lu), accepted=%lu (+%lu), rejected=%lu (+%lu), "
      "producer_rate=%lu B/s, consumer_calls=%lu (+%lu), consumed=%lu (+%lu), "
      "consumer_rate=%lu B/s, pending_estimate=%lu B (approximate), "
      "last_size=%lu, no_pcm_now_ms=%lu, current_gap_ms=%lu, max_gap_ms=%lu, "
      "gaps_gt_20ms=%lu, gaps_gt_30ms=%lu, gaps_gt_50ms=%lu, "
      "gaps_gt_75ms=%lu, gaps_gt_100ms=%lu, gaps_gt_200ms=%lu, "
      "format=%luHz/%luch/%lubit, audio_state=%s, free_heap=%lu\n",
      static_cast<unsigned long>((now_us - current.first_pcm_us) / 1000U),
      first_report ? "yes" : "no",
      static_cast<unsigned long>(current.producer_calls),
      static_cast<unsigned long>(producer_calls_delta),
      static_cast<unsigned long>(current.producer_offered),
      static_cast<unsigned long>(offered_delta),
      static_cast<unsigned long>(current.producer_accepted),
      static_cast<unsigned long>(accepted_delta),
      static_cast<unsigned long>(current.producer_rejected),
      static_cast<unsigned long>(rejected_delta),
      static_cast<unsigned long>(producer_rate),
      static_cast<unsigned long>(current.write_calls),
      static_cast<unsigned long>(consumer_calls_delta),
      static_cast<unsigned long>(current.pcm_bytes),
      static_cast<unsigned long>(consumed_delta),
      static_cast<unsigned long>(consumer_rate),
      static_cast<unsigned long>(pending_estimate),
      static_cast<unsigned long>(current.last_write_size),
      static_cast<unsigned long>((now_us - current.last_pcm_us) / 1000U),
      static_cast<unsigned long>(current.current_gap_us / 1000U),
      static_cast<unsigned long>(current.max_gap_us / 1000U),
      static_cast<unsigned long>(current.gaps_gt_20ms),
      static_cast<unsigned long>(current.gaps_gt_30ms),
      static_cast<unsigned long>(current.gaps_gt_50ms),
      static_cast<unsigned long>(current.gaps_gt_75ms),
      static_cast<unsigned long>(current.gaps_gt_100ms),
      static_cast<unsigned long>(current.gaps_gt_200ms),
      static_cast<unsigned long>(current.sample_rate),
      static_cast<unsigned long>(current.channels),
      static_cast<unsigned long>(current.bits_per_sample),
      audioStateName(current.audio_state),
      static_cast<unsigned long>(ESP.getFreeHeap()));

  finishReport(current, now_us);
}

}  // namespace a2dp_null_probe
