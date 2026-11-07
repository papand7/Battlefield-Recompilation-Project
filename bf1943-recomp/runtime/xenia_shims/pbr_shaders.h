// bf1943: PBR lighting for selected game pixel shaders (--pbr).
//
// The game's lit shaders are Blinn-Phong permutations. For the ones listed in
// pbr_shaders.txt the SPIR-V translator keeps the game's code, copies a few
// registers at given instruction addresses ("taps": normal, view vector,
// albedo, shadow, environment colour), and then overwrites the lit colour
// register with metal/roughness GGX lighting before the game's own exposure
// and export. Roughness and metallic come from the material's normal map
// replacement (tools/texpack install, *.pbr.rtex): blue = roughness in its
// upper half (128-255), in the lower half where metallic; alpha = the height for
// parallax, 191-253 (near 1 for shaders that read alpha as a mask); read through a view without swizzle whose alpha is 1 for
// every other texture; those use the shader's fallback. The cube map is fetched again with a LOD bias that grows with the
// roughness, so rough surfaces get blurred reflections.
// tools/pbr_taps.py writes these lines from the --dump-shaders disassembly.
//
// pbr_shaders.txt, one shader per line:
//   <pixel shader hash> key=value ...
//   n=r3.yzw@47    world normal, after instruction 47 (any length); @s: as
//                  the shader starts (an interpolator)
//   v=r9.xyz@35    vector from the surface to the camera
//   a=r6.yzw@63    albedo
//   lit=r2.x@74    1 = in sunlight, 0 = in shadow
//   env=r5.xyz@55  environment (cube map) colour: must be the tfetchCube
//                  instruction; without it the ambient light is reflected
//   so=r5.w@56     specular occlusion (optional)
//   ao=r2.x@44     ambient occlusion, darkens albedo and reflections (optional)
//   out=r0.xyz@99  lit colour, overwritten after this instruction
//   rm=10          the normal map fetch at this address is repeated to read
//                  roughness/metallic
//   env_scale=8    the game's environment multiplier (or a constant: c250.x)
//   pow=c251.z     the game's specular power n (a constant): roughness without
//                  pack data is sqrt(sqrt(2 / (n + 2)))
//   rough=0.36     roughness without pack data, if there is no pow
//   metal=0        metallic without pack data
//   blur=6         cube map LOD bias at roughness 1 (0: no blur)
//   rough_scale=1  multiplies the pack roughness
//   bump=2         exaggerates the normal map detail (0: off)
//   spec_max=1.5   cap of the sun highlight (times the sun colour)
//   pom=0.03       parallax occlusion: how deep the height map's 0 lies under
//                  its 1, in metres (0: off); the height is the pack normal
//                  map's alpha
//   wp=r3.xyz@s    world position (default; every table shader has it in r3)
//   cam=7          the camera position constant (default c7)
// Lighting constants (pixel float constants, defaults in brackets): l= sun
// direction [1], up= [2], sky= [3], ground= [4], sun= sun colour [5].
#pragma once
#include <cstdint>
#include <filesystem>

namespace xe::gpu::pbr {

constexpr uint32_t kTapAtStart = UINT32_MAX - 1;

struct Tap {
  uint32_t address = UINT32_MAX;  // after this ALU/fetch instruction
  uint32_t reg = 0;
  uint32_t count = 0;  // components
  uint32_t comps[4] = {};
  bool valid() const { return address != UINT32_MAX && count; }
};

// A number, or a component of a float constant.
struct Scalar {
  float value = 0.0f;
  uint32_t constant = UINT32_MAX;
  uint32_t comp = 0;
  bool is_constant() const { return constant != UINT32_MAX; }
};

struct ShaderConfig {
  Tap n, v, albedo, lit, env, so, ao, out;
  // World position for parallax (r3 as the shader starts unless wp= says).
  Tap wp = {kTapAtStart, 3, 3, {0, 1, 2, 0}};
  uint32_t c_cam = 7;
  float pom = 0.03f;
  uint32_t rm_fetch = UINT32_MAX;
  Scalar env_scale{1.0f};
  Scalar pow{0.0f};
  float rough = 0.5f, metal = 0.0f;
  float blur = 6.0f;
  // rough_scale=: multiplies the texture pack's roughness (its generator makes
  // wood and stone nearly matte, which looks like the game's own lighting).
  float rough_scale = 1.0f;
  // bump=: how much the normal map detail is exaggerated (0: as the game,
  // 2: three times as strong); only on flat faces, faded out by 60 m.
  float bump = 0.0f;
  // spec_max=: cap of the sun highlight in units of the sun colour, so glossy
  // surfaces facing the sun do not burn out into the bloom.
  float spec_max = 1.5f;
  // Debug: show=<fetch address> draws that texture at the pixel's screen
  // position (depth as grey) instead of the lighting; show_scale=<w,h> is the
  // screen size in pixels.
  uint32_t show_fetch = UINT32_MAX;
  float show_width = 2560.0f, show_height = 1440.0f, show_y0 = 0.0f, show_x0 = 0.0f;
  uint32_t c_light = 1, c_up = 2, c_sky = 3, c_ground = 4, c_sun = 5;
};

// Reads the shader table. Returns the number of shaders.
size_t Load(const std::filesystem::path& file);
bool Enabled();
const ShaderConfig* Find(uint64_t pixel_shader_hash);

// Material output for the scene effects: each --pbr pixel also writes
// atomicMin(materials[y * kMaterialPitch + x], depth bits | roughness << 4 |
// metallic) (top 24 bits of the float depth, roughness and metallic 4 bits
// each) into the storage buffer in the shared memory and EDRAM set, binding 3;
// the nearest pixel wins. Set by the command processor before translation.
constexpr uint32_t kMaterialPitch = 4096, kMaterialRows = 2560;
void SetMaterialOutput(bool on);
// Parallax depth multiplier for shaders translated from now on (the PC
// settings page: 0 off, 1 the table's pom=).
void SetPomScale(float scale);
float PomScale();
bool MaterialOutput();
// The game's own sun shadows off (the scene effects draw them): every pixel
// shader's shadow map PCF fetches (point-filtered 2D fetches at +-0.5 texel
// offsets from a texture the shader also reads with getWeights2D) return 1,
// the far plane, so the game's depth test always passes. Set before the GPU
// starts.
void SetGameShadowsOff(bool off);
bool GameShadowsOff();

}  // namespace xe::gpu::pbr
