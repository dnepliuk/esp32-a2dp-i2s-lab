#include "media_control.h"

#include <Arduino.h>
#include <BluetoothA2DPSink.h>

#include "app_config.h"

namespace {

portMUX_TYPE s_stateMux = portMUX_INITIALIZER_UNLOCKED;
BluetoothA2DPSink* s_sink = nullptr;
bool s_connected = false;
MediaPlaybackState s_playbackState = MediaPlaybackState::Stopped;
uint8_t s_volume = AppConfig::kVolumeFallback;
bool s_volumeKnown = false;
uint8_t s_lastRemoteVolume = AppConfig::kVolumeFallback;
bool s_remoteVolumeSeen = false;
bool s_remoteVolumeChanged = false;
bool s_disconnectedCommandRequested = false;

uint8_t clampVolume(int volume) {
  if (volume < AppConfig::kVolumeMin) {
    return AppConfig::kVolumeMin;
  }
  if (volume > AppConfig::kVolumeMax) {
    return AppConfig::kVolumeMax;
  }
  return static_cast<uint8_t>(volume);
}

void onRemoteVolumeChanged(int volume) {
  const uint8_t clamped = clampVolume(volume);

  portENTER_CRITICAL(&s_stateMux);
  s_volume = clamped;
  s_volumeKnown = true;
  if (!s_remoteVolumeSeen || clamped != s_lastRemoteVolume) {
    s_lastRemoteVolume = clamped;
    s_remoteVolumeSeen = true;
    s_remoteVolumeChanged = true;
  }
  portEXIT_CRITICAL(&s_stateMux);
}

BluetoothA2DPSink* connectedSink() {
  portENTER_CRITICAL(&s_stateMux);
  BluetoothA2DPSink* sink = s_connected ? s_sink : nullptr;
  portEXIT_CRITICAL(&s_stateMux);
  return sink;
}

void noteDisconnectedCommand() {
  portENTER_CRITICAL(&s_stateMux);
  s_disconnectedCommandRequested = true;
  portEXIT_CRITICAL(&s_stateMux);
}

}  // namespace

void media_control_init(BluetoothA2DPSink& sink) {
  portENTER_CRITICAL(&s_stateMux);
  s_sink = &sink;
  s_connected = false;
  s_playbackState = MediaPlaybackState::Stopped;
  s_volume = AppConfig::kVolumeFallback;
  s_volumeKnown = false;
  s_remoteVolumeSeen = false;
  s_remoteVolumeChanged = false;
  s_disconnectedCommandRequested = false;
  portEXIT_CRITICAL(&s_stateMux);

  sink.set_on_volumechange(onRemoteVolumeChanged);

  Serial.println("MEDIA: AVRCP controls initialized");
  Serial.print("MEDIA: volume range=");
  Serial.print(AppConfig::kVolumeMin);
  Serial.print("..");
  Serial.print(AppConfig::kVolumeMax);
  Serial.print(" step=");
  Serial.println(AppConfig::kVolumeStep);
}

void media_control_update() {
  bool reportRemoteVolume = false;
  bool reportDisconnectedCommand = false;
  uint8_t remoteVolume = AppConfig::kVolumeFallback;

  portENTER_CRITICAL(&s_stateMux);
  if (s_remoteVolumeChanged) {
    remoteVolume = s_lastRemoteVolume;
    s_remoteVolumeChanged = false;
    reportRemoteVolume = true;
  }
  if (s_disconnectedCommandRequested) {
    s_disconnectedCommandRequested = false;
    reportDisconnectedCommand = true;
  }
  portEXIT_CRITICAL(&s_stateMux);

  if (reportRemoteVolume) {
    Serial.print("MEDIA: remote volume confirmed=");
    Serial.println(remoteVolume);
  }
  if (reportDisconnectedCommand) {
    Serial.println("MEDIA: command ignored; Bluetooth disconnected");
  }
}

void media_control_volume_up() {
  media_control_set_volume(media_control_get_volume() +
                           AppConfig::kVolumeStep);
}

void media_control_volume_down() {
  media_control_set_volume(media_control_get_volume() -
                           AppConfig::kVolumeStep);
}

void media_control_set_volume(int volume) {
  BluetoothA2DPSink* sink = connectedSink();
  if (sink == nullptr) {
    return;
  }

  const uint8_t clamped = clampVolume(volume);
  bool changed = false;

  portENTER_CRITICAL(&s_stateMux);
  if (!s_volumeKnown || s_volume != clamped) {
    s_volume = clamped;
    s_volumeKnown = true;
    changed = true;
  }
  portEXIT_CRITICAL(&s_stateMux);

  if (!changed) {
    return;
  }

  sink->set_volume(clamped);
  Serial.print("MEDIA: local volume request=");
  Serial.println(clamped);
}

int media_control_get_volume() {
  portENTER_CRITICAL(&s_stateMux);
  const uint8_t volume = s_volumeKnown ? s_volume : AppConfig::kVolumeFallback;
  portEXIT_CRITICAL(&s_stateMux);
  return volume;
}

void media_control_play() {
  BluetoothA2DPSink* sink = connectedSink();
  if (sink == nullptr) {
    noteDisconnectedCommand();
    return;
  }
  sink->play();
  Serial.println("MEDIA: play requested");
}

void media_control_pause() {
  BluetoothA2DPSink* sink = connectedSink();
  if (sink == nullptr) {
    noteDisconnectedCommand();
    return;
  }
  sink->pause();
  Serial.println("MEDIA: pause requested");
}

void media_control_toggle_play_pause() {
  portENTER_CRITICAL(&s_stateMux);
  const bool connected = s_connected;
  const MediaPlaybackState playbackState = s_playbackState;
  portEXIT_CRITICAL(&s_stateMux);

  if (!connected) {
    noteDisconnectedCommand();
    return;
  }

  if (playbackState == MediaPlaybackState::Started) {
    media_control_pause();
  } else {
    media_control_play();
  }
}

void media_control_next() {
  BluetoothA2DPSink* sink = connectedSink();
  if (sink == nullptr) {
    noteDisconnectedCommand();
    return;
  }
  sink->next();
  Serial.println("MEDIA: next requested");
}

void media_control_previous() {
  BluetoothA2DPSink* sink = connectedSink();
  if (sink == nullptr) {
    noteDisconnectedCommand();
    return;
  }
  sink->previous();
  Serial.println("MEDIA: previous requested");
}

MediaPlaybackState media_control_get_playback_state() {
  portENTER_CRITICAL(&s_stateMux);
  const MediaPlaybackState state = s_playbackState;
  portEXIT_CRITICAL(&s_stateMux);
  return state;
}

bool media_control_is_playing() {
  return media_control_get_playback_state() == MediaPlaybackState::Started;
}

void media_control_on_connection_state(bool connected) {
  portENTER_CRITICAL(&s_stateMux);
  s_connected = connected;
  if (!connected) {
    s_playbackState = MediaPlaybackState::Stopped;
  }
  portEXIT_CRITICAL(&s_stateMux);
}

void media_control_on_playback_state(MediaPlaybackState state) {
  portENTER_CRITICAL(&s_stateMux);
  s_playbackState = state;
  portEXIT_CRITICAL(&s_stateMux);
}
