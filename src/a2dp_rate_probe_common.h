#pragma once

#include <Arduino.h>
#include <esp_timer.h>

#include "AudioTools.h"
#include "BluetoothA2DPSinkQueued.h"

namespace a2dp_rate_probe {

constexpr uint32_t kStatsIntervalMs = 5000;

struct ProbeVariant {
  const char* variant;
  const char* bluetooth_name;
  bool use_apll;
};

struct ProbeCounters {
  volatile uint32_t producer_calls = 0;
  volatile uint32_t producer_offered = 0;
  volatile uint32_t producer_accepted = 0;
  volatile uint32_t producer_rejected = 0;
  volatile uint32_t consumer_calls = 0;
  volatile uint32_t consumer_requested = 0;
  volatile uint32_t consumer_written = 0;
  volatile uint32_t consumer_short_writes = 0;
  volatile uint32_t consumer_errors = 0;
  volatile uint32_t consumer_write_us_total = 0;
  volatile uint32_t consumer_write_us_max = 0;
  volatile uint32_t consumer_gap_us_max = 0;
  volatile uint32_t consumer_last_start_us = 0;
};

struct ProbeSnapshot {
  uint32_t producer_calls;
  uint32_t producer_offered;
  uint32_t producer_accepted;
  uint32_t producer_rejected;
  uint32_t consumer_calls;
  uint32_t consumer_requested;
  uint32_t consumer_written;
  uint32_t consumer_short_writes;
  uint32_t consumer_errors;
  uint32_t consumer_write_us_total;
  uint32_t consumer_write_us_max;
  uint32_t consumer_gap_us_max;
};

static portMUX_TYPE counter_mux = portMUX_INITIALIZER_UNLOCKED;
static ProbeCounters counters;

class CountedI2SStream final : public audio_tools::I2SStream {
 public:
  using audio_tools::I2SStream::write;

  size_t write(const uint8_t* data, size_t len) override {
    const uint32_t start_us = static_cast<uint32_t>(esp_timer_get_time());
    const size_t written = audio_tools::I2SStream::write(data, len);
    const uint32_t end_us = static_cast<uint32_t>(esp_timer_get_time());
    const uint32_t write_us = end_us - start_us;

    portENTER_CRITICAL(&counter_mux);
    const uint32_t previous_start_us = counters.consumer_last_start_us;
    if (previous_start_us != 0) {
      const uint32_t gap_us = start_us - previous_start_us;
      if (gap_us > counters.consumer_gap_us_max) {
        counters.consumer_gap_us_max = gap_us;
      }
    }
    counters.consumer_last_start_us = start_us;
    ++counters.consumer_calls;
    counters.consumer_requested += static_cast<uint32_t>(len);
    counters.consumer_written += static_cast<uint32_t>(written);
    counters.consumer_write_us_total += write_us;
    if (write_us > counters.consumer_write_us_max) {
      counters.consumer_write_us_max = write_us;
    }
    if (written != len) {
      ++counters.consumer_short_writes;
    }
    if (len != 0 && written == 0) {
      ++counters.consumer_errors;
    }
    portEXIT_CRITICAL(&counter_mux);

    return written;
  }
};

class ProbedBluetoothA2DPSinkQueued final : public BluetoothA2DPSinkQueued {
 public:
  explicit ProbedBluetoothA2DPSinkQueued(CountedI2SStream& output)
      : BluetoothA2DPSinkQueued(
            static_cast<audio_tools::AudioStream&>(output)) {}

 protected:
  size_t write_audio(const uint8_t* data, size_t size) override {
    const size_t accepted =
        BluetoothA2DPSinkQueued::write_audio(data, size);

    portENTER_CRITICAL(&counter_mux);
    ++counters.producer_calls;
    counters.producer_offered += static_cast<uint32_t>(size);
    counters.producer_accepted += static_cast<uint32_t>(accepted);
    counters.producer_rejected += static_cast<uint32_t>(size - accepted);
    portEXIT_CRITICAL(&counter_mux);

    return accepted;
  }
};

static CountedI2SStream i2s;
static uint32_t last_stats_ms = 0;
static ProbeSnapshot last_snapshot = {};

static ProbeSnapshot takeSnapshot() {
  ProbeSnapshot result;
  portENTER_CRITICAL(&counter_mux);
  result.producer_calls = counters.producer_calls;
  result.producer_offered = counters.producer_offered;
  result.producer_accepted = counters.producer_accepted;
  result.producer_rejected = counters.producer_rejected;
  result.consumer_calls = counters.consumer_calls;
  result.consumer_requested = counters.consumer_requested;
  result.consumer_written = counters.consumer_written;
  result.consumer_short_writes = counters.consumer_short_writes;
  result.consumer_errors = counters.consumer_errors;
  result.consumer_write_us_total = counters.consumer_write_us_total;
  result.consumer_write_us_max = counters.consumer_write_us_max;
  result.consumer_gap_us_max = counters.consumer_gap_us_max;
  portEXIT_CRITICAL(&counter_mux);
  return result;
}

static void onSampleRate(uint16_t sample_rate) {
  const uint32_t expected_pcm_rate =
      static_cast<uint32_t>(sample_rate) * 2U * 2U;

  Serial.printf("A2DP negotiated sample rate: %u Hz\n",
                static_cast<unsigned int>(sample_rate));
  Serial.printf("Expected PCM rate: %lu B/s\n",
                static_cast<unsigned long>(expected_pcm_rate));
}

static void setupProbe(const ProbeVariant& variant) {
  Serial.begin(115200);
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Info);
  delay(250);

  Serial.println();
  Serial.printf("ARDUINO_A2DP_TEST variant=%s\n", variant.variant);
  Serial.printf("Bluetooth name: %s\n", variant.bluetooth_name);
  Serial.println(
      "Output: upstream BluetoothA2DPSinkQueued -> counted I2SStream");
  Serial.println("Output lifecycle: always active");
  if (variant.use_apll) {
    Serial.println("I2S clock: APLL enabled");
  }
  Serial.println("Sample rate: observed, upstream automatic");
  Serial.println("Queue configuration: upstream defaults");
  if (!variant.use_apll) {
    Serial.println("Behavior changes: none");
  }
  Serial.println("Stats interval: 5000 ms");

  auto config = i2s.defaultConfig(TX_MODE);
  config.sample_rate = 44100;
  config.bits_per_sample = 16;
  config.channels = 2;
  config.i2s_format = I2S_STD_FORMAT;
  config.pin_bck = 26;
  config.pin_ws = 25;
  config.pin_data = 22;
  config.pin_data_rx = -1;
  config.use_apll = variant.use_apll;

  if (variant.use_apll) {
    Serial.printf("I2S APLL requested: %s\n",
                  config.use_apll ? "yes" : "no");
  }
  const bool i2s_started = i2s.begin(config);
  if (variant.use_apll) {
    Serial.printf("I2S APLL propagated to driver config: %s\n",
                  config.use_apll && i2s_started ? "yes" : "no");
  }

  static ProbedBluetoothA2DPSinkQueued a2dp_sink(i2s);
  a2dp_sink.set_output_active_by_state(false);
  a2dp_sink.set_sample_rate_callback(onSampleRate);
  a2dp_sink.start(variant.bluetooth_name);

  last_snapshot = takeSnapshot();
  last_stats_ms = millis();
}

static void loopProbe() {
  const uint32_t now_ms = millis();
  const uint32_t elapsed_ms = now_ms - last_stats_ms;
  if (elapsed_ms < kStatsIntervalMs) {
    delay(10);
    return;
  }

  const ProbeSnapshot current = takeSnapshot();
  const uint32_t producer_calls_delta =
      current.producer_calls - last_snapshot.producer_calls;
  const uint32_t offered_delta =
      current.producer_offered - last_snapshot.producer_offered;
  const uint32_t accepted_delta =
      current.producer_accepted - last_snapshot.producer_accepted;
  const uint32_t rejected_delta =
      current.producer_rejected - last_snapshot.producer_rejected;
  const uint32_t consumer_calls_delta =
      current.consumer_calls - last_snapshot.consumer_calls;
  const uint32_t requested_delta =
      current.consumer_requested - last_snapshot.consumer_requested;
  const uint32_t written_delta =
      current.consumer_written - last_snapshot.consumer_written;
  const uint32_t write_us_delta =
      current.consumer_write_us_total - last_snapshot.consumer_write_us_total;

  const uint32_t producer_rate = static_cast<uint32_t>(
      (static_cast<uint64_t>(accepted_delta) * 1000ULL) / elapsed_ms);
  const uint32_t consumer_rate = static_cast<uint32_t>(
      (static_cast<uint64_t>(written_delta) * 1000ULL) / elapsed_ms);
  const int32_t interval_delta =
      static_cast<int32_t>(accepted_delta - written_delta);
  const uint32_t pending_estimate =
      current.producer_accepted - current.consumer_written;
  const uint32_t write_us_avg =
      consumer_calls_delta == 0 ? 0 : write_us_delta / consumer_calls_delta;

  Serial.printf(
      "RATE stats: elapsed_ms=%lu, producer_calls=%lu (+%lu), offered=%lu "
      "(+%lu), accepted=%lu (+%lu), rejected=%lu (+%lu), producer_rate=%lu "
      "B/s, consumer_calls=%lu (+%lu), requested=%lu (+%lu), written=%lu "
      "(+%lu), consumer_rate=%lu B/s, interval_delta=%ld B, "
      "pending_estimate=%lu B, short=%lu, errors=%lu, write_us_avg=%lu, "
      "write_us_max=%lu, consumer_gap_ms_max=%lu, free_heap=%lu\n",
      static_cast<unsigned long>(elapsed_ms),
      static_cast<unsigned long>(current.producer_calls),
      static_cast<unsigned long>(producer_calls_delta),
      static_cast<unsigned long>(current.producer_offered),
      static_cast<unsigned long>(offered_delta),
      static_cast<unsigned long>(current.producer_accepted),
      static_cast<unsigned long>(accepted_delta),
      static_cast<unsigned long>(current.producer_rejected),
      static_cast<unsigned long>(rejected_delta),
      static_cast<unsigned long>(producer_rate),
      static_cast<unsigned long>(current.consumer_calls),
      static_cast<unsigned long>(consumer_calls_delta),
      static_cast<unsigned long>(current.consumer_requested),
      static_cast<unsigned long>(requested_delta),
      static_cast<unsigned long>(current.consumer_written),
      static_cast<unsigned long>(written_delta),
      static_cast<unsigned long>(consumer_rate),
      static_cast<long>(interval_delta),
      static_cast<unsigned long>(pending_estimate),
      static_cast<unsigned long>(current.consumer_short_writes),
      static_cast<unsigned long>(current.consumer_errors),
      static_cast<unsigned long>(write_us_avg),
      static_cast<unsigned long>(current.consumer_write_us_max),
      static_cast<unsigned long>(current.consumer_gap_us_max / 1000U),
      static_cast<unsigned long>(ESP.getFreeHeap()));

  last_snapshot = current;
  last_stats_ms = now_ms;
}

}  // namespace a2dp_rate_probe
