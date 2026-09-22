#include "bluetooth_audio.h"

#include <Arduino.h>

#include "AudioTools.h"
#include "BluetoothA2DPSink.h"
#include "app_config.h"

namespace BluetoothAudio {
namespace {

volatile esp_a2d_connection_state_t s_connectionState =
    ESP_A2D_CONNECTION_STATE_DISCONNECTED;
volatile esp_a2d_audio_state_t s_audioState = ESP_A2D_AUDIO_STATE_STOPPED;
volatile uint16_t s_sampleRate = AppConfig::kInitialSampleRate;
volatile bool s_sampleRateChanged = false;

esp_a2d_connection_state_t s_reportedConnectionState =
    ESP_A2D_CONNECTION_STATE_DISCONNECTED;
esp_a2d_audio_state_t s_reportedAudioState = ESP_A2D_AUDIO_STATE_STOPPED;

BluetoothA2DPSink& sink(audio_tools::I2SStream& output) {
  static BluetoothA2DPSink instance(output);
  return instance;
}

void onConnectionStateChanged(esp_a2d_connection_state_t state, void*) {
  s_connectionState = state;
}

void onAudioStateChanged(esp_a2d_audio_state_t state, void*) {
  s_audioState = state;
}

void onSampleRateChanged(uint16_t rate) {
  s_sampleRate = rate;
  s_sampleRateChanged = true;
}

void reportConnectionState(esp_a2d_connection_state_t state) {
  if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
    Serial.println("Bluetooth connected");
  } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
    Serial.println("Bluetooth disconnected; waiting for connection");
  }
}

void reportAudioState(esp_a2d_audio_state_t state) {
  if (state == ESP_A2D_AUDIO_STATE_STARTED) {
    Serial.println("Audio started");
  } else if (state == ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND) {
    Serial.println("Audio suspended");
  } else if (state == ESP_A2D_AUDIO_STATE_STOPPED) {
    Serial.println("Audio stopped");
  }
}

}  // namespace

void begin(audio_tools::I2SStream& output) {
  BluetoothA2DPSink& a2dp = sink(output);
  a2dp.set_on_connection_state_changed(onConnectionStateChanged);
  a2dp.set_on_audio_state_changed(onAudioStateChanged);
  a2dp.set_sample_rate_callback(onSampleRateChanged);
  a2dp.set_auto_reconnect(true, AppConfig::kReconnectAttempts);
  a2dp.start(AppConfig::kBluetoothName);
}

void update() {
  const auto connectionState = s_connectionState;
  if (connectionState != s_reportedConnectionState) {
    s_reportedConnectionState = connectionState;
    reportConnectionState(connectionState);
  }

  const auto audioState = s_audioState;
  if (audioState != s_reportedAudioState) {
    s_reportedAudioState = audioState;
    reportAudioState(audioState);
  }

  if (s_sampleRateChanged) {
    const uint16_t sampleRate = s_sampleRate;
    s_sampleRateChanged = false;
    Serial.print("Negotiated sample rate: ");
    Serial.print(sampleRate);
    Serial.println(" Hz");
  }
}

bool isConnected() {
  return s_connectionState == ESP_A2D_CONNECTION_STATE_CONNECTED;
}

bool isPlaying() { return s_audioState == ESP_A2D_AUDIO_STATE_STARTED; }

}  // namespace BluetoothAudio
