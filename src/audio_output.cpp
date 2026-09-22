#include "audio_output.h"

#include "app_config.h"

namespace AudioOutput {
namespace {

audio_tools::I2SStream s_i2s;
bool s_ready = false;

}  // namespace

bool begin() {
  auto config = s_i2s.defaultConfig(TX_MODE);
  config.sample_rate = AppConfig::kInitialSampleRate;
  config.bits_per_sample = AppConfig::kBitsPerSample;
  config.channels = AppConfig::kChannels;
  config.i2s_format = I2S_STD_FORMAT;
  config.is_master = true;
  config.pin_bck = AppConfig::kI2sBckPin;
  config.pin_ws = AppConfig::kI2sWsPin;
  config.pin_data = AppConfig::kI2sDataPin;
  config.pin_data_rx = I2S_PIN_NO_CHANGE;

  s_ready = s_i2s.begin(config);
  return s_ready;
}

bool isReady() { return s_ready; }

audio_tools::I2SStream& stream() { return s_i2s; }

}  // namespace AudioOutput
