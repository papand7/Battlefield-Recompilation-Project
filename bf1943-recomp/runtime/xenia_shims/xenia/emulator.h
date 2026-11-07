// bf1943 shim: there is no Xenia Emulator object.
#pragma once
#include "xenia/apu/audio_system.h"
namespace xe {
class Emulator {
 public:
  apu::AudioSystem* audio_system() const { return nullptr; }
};
}  // namespace xe
