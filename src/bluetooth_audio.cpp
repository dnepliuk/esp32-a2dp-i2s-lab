#include "bluetooth_audio.h"

#include <Arduino.h>

#include "AudioTools.h"
#include "BluetoothA2DPSink.h"
#include "app_config.h"
#include "media_control.h"

namespace BluetoothAudio {
namespace {

enum class PairingState : uint8_t {
  Normal,
  Requested,
  WaitingForDisconnect,
  Discoverable,
};

portMUX_TYPE s_stateMux = portMUX_INITIALIZER_UNLOCKED;
esp_a2d_connection_state_t s_connectionState =
    ESP_A2D_CONNECTION_STATE_DISCONNECTED;
esp_a2d_audio_state_t s_audioState = ESP_A2D_AUDIO_STATE_STOPPED;
uint16_t s_sampleRate = AppConfig::kInitialSampleRate;
bool s_sampleRateChanged = false;
bool s_disconnectedEventPending = false;

esp_a2d_connection_state_t s_reportedConnectionState =
    ESP_A2D_CONNECTION_STATE_DISCONNECTED;
esp_a2d_audio_state_t s_reportedAudioState = ESP_A2D_AUDIO_STATE_STOPPED;
BluetoothA2DPSink* s_sink = nullptr;
PairingState s_pairingState = PairingState::Normal;
bool s_reconnectNeedsArm = false;

BluetoothA2DPSink& sink(audio_tools::I2SStream& output) {
  static BluetoothA2DPSink instance(output);
  return instance;
}

void onConnectionStateChanged(esp_a2d_connection_state_t state, void*) {
  portENTER_CRITICAL(&s_stateMux);
  s_connectionState = state;
  if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
    s_disconnectedEventPending = true;
  }
  portEXIT_CRITICAL(&s_stateMux);

  media_control_on_connection_state(
      state == ESP_A2D_CONNECTION_STATE_CONNECTED);
}

void onAudioStateChanged(esp_a2d_audio_state_t state, void*) {
  portENTER_CRITICAL(&s_stateMux);
  s_audioState = state;
  portEXIT_CRITICAL(&s_stateMux);

  if (state == ESP_A2D_AUDIO_STATE_STARTED) {
    media_control_on_playback_state(MediaPlaybackState::Started);
  } else if (state == ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND) {
    media_control_on_playback_state(MediaPlaybackState::Suspended);
  } else {
    media_control_on_playback_state(MediaPlaybackState::Stopped);
  }
}

void onSampleRateChanged(uint16_t rate) {
  portENTER_CRITICAL(&s_stateMux);
  s_sampleRate = rate;
  s_sampleRateChanged = true;
  portEXIT_CRITICAL(&s_stateMux);
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

void enterDiscoverableState() {
  if (s_sink == nullptr) {
    return;
  }

  s_sink->set_discoverability(ESP_BT_GENERAL_DISCOVERABLE);
  s_sink->set_connectable(true);
  s_pairingState = PairingState::Discoverable;
  Serial.println("BT: discoverable, waiting for connection");
}

void completeManualPairing() {
  s_pairingState = PairingState::Normal;
  s_reconnectNeedsArm = true;
  Serial.println("BT: manual pairing completed");
}

void updatePairingState(esp_a2d_connection_state_t connectionState,
                        bool disconnectedEvent) {
  if (s_sink == nullptr) {
    return;
  }

  switch (s_pairingState) {
    case PairingState::Normal:
      if (s_reconnectNeedsArm &&
          connectionState == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
        s_reconnectNeedsArm = false;
        if (!s_sink->reconnect()) {
          s_sink->set_discoverability(ESP_BT_GENERAL_DISCOVERABLE);
          s_sink->set_connectable(true);
        }
      }
      break;

    case PairingState::Requested:
      // disconnect() also disables the library's private runtime reconnect
      // latch, including when a retry is currently between connection events.
      s_sink->disconnect();
      if (connectionState == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
        enterDiscoverableState();
      } else {
        s_pairingState = PairingState::WaitingForDisconnect;
        Serial.println("BT: disconnect requested");
        Serial.println("BT: waiting for disconnect");
      }
      break;

    case PairingState::WaitingForDisconnect:
      if (disconnectedEvent &&
          connectionState == ESP_A2D_CONNECTION_STATE_CONNECTED) {
        completeManualPairing();
      } else if (disconnectedEvent ||
                 connectionState == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
        enterDiscoverableState();
      }
      break;

    case PairingState::Discoverable:
      if (connectionState == ESP_A2D_CONNECTION_STATE_CONNECTED) {
        completeManualPairing();
      }
      break;
  }
}

}  // namespace

void begin(audio_tools::I2SStream& output) {
  BluetoothA2DPSink& a2dp = sink(output);
  s_sink = &a2dp;
  s_pairingState = PairingState::Normal;
  s_reconnectNeedsArm = false;
  media_control_init(a2dp);
  a2dp.set_on_connection_state_changed(onConnectionStateChanged);
  a2dp.set_on_audio_state_changed(onAudioStateChanged);
  a2dp.set_sample_rate_callback(onSampleRateChanged);
  a2dp.set_auto_reconnect(true, AppConfig::kReconnectAttempts);
  a2dp.start(AppConfig::kBluetoothName);
}

void update() {
  portENTER_CRITICAL(&s_stateMux);
  const esp_a2d_connection_state_t connectionState = s_connectionState;
  const esp_a2d_audio_state_t audioState = s_audioState;
  const uint16_t sampleRate = s_sampleRate;
  const bool sampleRateChanged = s_sampleRateChanged;
  const bool disconnectedEvent = s_disconnectedEventPending;
  s_sampleRateChanged = false;
  s_disconnectedEventPending = false;
  portEXIT_CRITICAL(&s_stateMux);

  if (connectionState != s_reportedConnectionState) {
    s_reportedConnectionState = connectionState;
    reportConnectionState(connectionState);
  }

  if (audioState != s_reportedAudioState) {
    s_reportedAudioState = audioState;
    reportAudioState(audioState);
  }

  if (sampleRateChanged) {
    Serial.print("Negotiated sample rate: ");
    Serial.print(sampleRate);
    Serial.println(" Hz");
  }

  updatePairingState(connectionState, disconnectedEvent);
}

void enterPairingMode() {
  if (s_pairingState != PairingState::Normal) {
    return;
  }

  s_reconnectNeedsArm = false;
  s_pairingState = PairingState::Requested;
  Serial.println("BT: manual pairing requested");
}

bool isPairingMode() {
  return s_pairingState != PairingState::Normal;
}

bool isConnected() {
  portENTER_CRITICAL(&s_stateMux);
  const bool connected =
      s_connectionState == ESP_A2D_CONNECTION_STATE_CONNECTED;
  portEXIT_CRITICAL(&s_stateMux);
  return connected;
}

bool isPlaying() {
  portENTER_CRITICAL(&s_stateMux);
  const bool playing = s_audioState == ESP_A2D_AUDIO_STATE_STARTED;
  portEXIT_CRITICAL(&s_stateMux);
  return playing;
}

}  // namespace BluetoothAudio
