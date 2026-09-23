#pragma once

#include <stdint.h>

class BluetoothA2DPSink;

enum class MediaPlaybackState : uint8_t {
  Stopped,
  Started,
  Suspended,
};

// Binds the application-level controls to the single sink owned by
// bluetooth_audio. Must be called before BluetoothA2DPSink::start().
void media_control_init(BluetoothA2DPSink& sink);

// Reports deferred callback events from normal application context.
void media_control_update();

void media_control_volume_up();
void media_control_volume_down();
void media_control_set_volume(int volume);
int media_control_get_volume();

void media_control_play();
void media_control_pause();
void media_control_toggle_play_pause();
void media_control_next();
void media_control_previous();
MediaPlaybackState media_control_get_playback_state();
bool media_control_is_playing();

// These short, thread-safe state updates are intended for Bluetooth callbacks.
void media_control_on_connection_state(bool connected);
void media_control_on_playback_state(MediaPlaybackState state);
