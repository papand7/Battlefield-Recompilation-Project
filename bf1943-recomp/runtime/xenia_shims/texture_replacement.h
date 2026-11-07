// bf1943: replacement textures (tools/texpack "install" output).
//
// A game texture is identified by its size and a hash of the first 4 KB of its
// base level as it sits in guest memory (Xenos-tiled, as stored in the .res
// file). A match is uploaded as an RGBA8 image (usually 2x the guest size)
// instead of the guest data, with the channels where the game's own host
// format puts them.
#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace xe::gpu::texrep {

struct Replacement {
  uint32_t width, height, levels;
  std::filesystem::path file;
  // A normal map with --pbr data (file name *.pbr.rtex): red = X, green = Y,
  // blue = roughness, alpha = 254/255 (not metal) down to 0 (metal), whatever
  // the guest format; the texture cache swizzles X to where the game reads it.
  bool pbr = false;
};

constexpr uint32_t kHashBytes = 4096;
uint64_t Hash(const uint8_t* data, size_t size);

// Reads <dir>/replacements.txt. Returns the number of entries.
size_t SetDirectory(const std::filesystem::path& dir);
bool Enabled();
void SetLogMisses(bool log);
// Logs the shaders that draw with replacements whose file name contains this.
void SetTrace(const std::string& name_part);
const std::string& Trace();

// guest: the base level in guest memory (at least kHashBytes readable).
const Replacement* Find(const uint8_t* guest, uint32_t width, uint32_t height,
                        uint32_t guest_format);

// RGBA8 pixels of each level, tightly packed; level_offsets[i] is the byte
// offset of level i in data.
struct Pixels {
  std::vector<uint8_t> data;
  std::vector<size_t> level_offsets;
};
// A background thread started by SetDirectory reads the pack into memory, so
// this usually returns at once instead of reading the file on the GPU thread.
// Null if the file can't be read.
std::shared_ptr<const Pixels> Load(const Replacement& r);

}  // namespace xe::gpu::texrep
