#pragma once

namespace audio_tools {
class I2SStream;
}

namespace BluetoothAudio {

void begin(audio_tools::I2SStream& output);
void update();
void enterPairingMode();
bool isPairingMode();
bool isConnected();
bool isPlaying();

}  // namespace BluetoothAudio
