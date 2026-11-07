// bf1943 shim: no Xenia audio system.
#pragma once
#include <string>
namespace xe { namespace apu {
class AudioSystem { public: std::string name() const { return {}; } };
} }
