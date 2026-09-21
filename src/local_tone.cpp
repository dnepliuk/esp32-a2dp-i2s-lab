#include <Arduino.h>

#include <cmath>
#include <cstdint>

#include "AudioTools.h"

namespace {

constexpr uint32_t kSampleRate = 44100;
constexpr uint32_t kToneFrequency = 1000;
constexpr size_t kChannels = 2;
constexpr size_t kBytesPerSample = sizeof(int16_t);
constexpr size_t kBytesPerFrame = kChannels * kBytesPerSample;
constexpr size_t kPeriodFrames = 441;
constexpr size_t kToneBufferBytes = kPeriodFrames * kBytesPerFrame;
constexpr int16_t kAmplitude = 29490;
constexpr uint32_t kStatsIntervalMs = 5000;
constexpr float kTwoPi = 6.28318530717958647692f;

static_assert(kToneBufferBytes % kBytesPerFrame == 0,
              "PCM buffer must contain complete stereo frames");
static_assert(kToneFrequency * kPeriodFrames == 10 * kSampleRate,
              "The block must span exactly 10 cycles at 1000 Hz");

I2SStream i2s;
uint8_t tone_buffer[kToneBufferBytes];
size_t write_offset = 0;
uint64_t total_written = 0;
uint64_t interval_written = 0;
uint32_t partial_writes = 0;
uint32_t write_errors = 0;
uint32_t stats_started_ms = 0;
bool i2s_ready = false;

void prepareToneBuffer() {
  for (size_t frame = 0; frame < kPeriodFrames; ++frame) {
    const float phase =
        kTwoPi * static_cast<float>(kToneFrequency) * frame / kSampleRate;
    const int16_t sample =
        static_cast<int16_t>(lroundf(kAmplitude * sinf(phase)));
    const uint16_t encoded = static_cast<uint16_t>(sample);
    const uint8_t low = static_cast<uint8_t>(encoded & 0xffu);
    const uint8_t high = static_cast<uint8_t>(encoded >> 8);
    const size_t byte = frame * kBytesPerFrame;

    tone_buffer[byte] = low;
    tone_buffer[byte + 1] = high;
    tone_buffer[byte + 2] = low;
    tone_buffer[byte + 3] = high;
  }
}

void writeToneBlock() {
  while (write_offset < kToneBufferBytes) {
    const size_t remaining = kToneBufferBytes - write_offset;
    const size_t written = i2s.write(tone_buffer + write_offset, remaining);

    if (written == 0 || written > remaining) {
      ++write_errors;
      return;
    }
    if (written < remaining) {
      ++partial_writes;
    }

    write_offset += written;
    total_written += written;
    interval_written += written;
  }

  write_offset = 0;
}

void printStatsIfDue() {
  const uint32_t now = millis();
  const uint32_t elapsed_ms = now - stats_started_ms;
  if (elapsed_ms < kStatsIntervalMs) {
    return;
  }

  const uint32_t bytes_per_second = static_cast<uint32_t>(
      (interval_written * 1000ULL) / static_cast<uint64_t>(elapsed_ms));
  Serial.printf(
      "Tone stats: written=%llu, interval=%llu, rate=%lu B/s, partial=%lu, "
      "errors=%lu\n",
      static_cast<unsigned long long>(total_written),
      static_cast<unsigned long long>(interval_written),
      static_cast<unsigned long>(bytes_per_second),
      static_cast<unsigned long>(partial_writes),
      static_cast<unsigned long>(write_errors));

  interval_written = 0;
  stats_started_ms = now;
}

}  // namespace

void setup() {
  Serial.begin(115200);

  Serial.println("I2S_LOCAL_TONE_TEST");
  Serial.println("Tone: 1000 Hz");
  Serial.println("Sample rate: 44100 Hz");
  Serial.println("Format: s16le stereo Philips I2S");
  Serial.println("Pins: BCK=26 WS=25 DATA=22");
  Serial.println("Amplitude: 90%");
  Serial.println("Bluetooth: disabled");

  prepareToneBuffer();

  auto config = i2s.defaultConfig(TX_MODE);
  config.sample_rate = kSampleRate;
  config.bits_per_sample = 16;
  config.channels = kChannels;
  config.i2s_format = I2S_STD_FORMAT;
  config.pin_bck = 26;
  config.pin_ws = 25;
  config.pin_data = 22;
  config.pin_data_rx = -1;
  i2s_ready = i2s.begin(config);
  if (!i2s_ready) {
    Serial.println("I2S error: begin failed");
  }

  stats_started_ms = millis();
}

void loop() {
  if (!i2s_ready) {
    return;
  }

  writeToneBlock();
  printStatsIfDue();
}
