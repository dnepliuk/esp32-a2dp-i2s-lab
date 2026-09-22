#pragma once

#include <stdint.h>

namespace StatusLed {

enum class State : uint8_t {
  Initializing,
  WaitingForConnection,
  ConnectedIdle,
  Playing,
  InitializationError,
};

void begin();
void setState(State state);
void update();

}  // namespace StatusLed
