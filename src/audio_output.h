#pragma once

#include "AudioTools.h"

namespace AudioOutput {

bool begin();
bool isReady();
audio_tools::I2SStream& stream();

}  // namespace AudioOutput
