// bf1943 blood on hit (--blood): soldiers hit spray a puff of blood; a death
// leaves a pool; a soldier killed by an explosion comes apart: the bones of
// their skinned draws (rigid, one bone per vertex) are frozen and split into
// pieces (head, arms, legs, torso) that fly, tumble, trail blood and stay on
// the ground with blood and chunks around them. The runtime reports hits from
// the soldiers' health in guest memory. The particles live in world space and
// are drawn by the scene effects pass (shaders/scene_fx.cs.glsl) against the
// world depth, before the HUD; splats and chunks on the ground stay.

#ifndef XENIA_GPU_VULKAN_BF_GORE_H_
#define XENIA_GPU_VULKAN_BF_GORE_H_

#include <atomic>
#include <cstdint>

namespace xe {
namespace gpu {
namespace bflook {

inline std::atomic<bool> gore{false};
// Smoke, dust and debris with or without blood, at the game's explosions
// (runtime: ClientExplosionEntity, AddExplosion); on with BF_PARTICLES=1.
inline std::atomic<bool> effects{false};
// Any thread: an explosion at pos (world).
void AddExplosion(const float pos[3]);
// GPU thread: kind 0 a puff of gun smoke at pos, 1 an explosion there (a
// dust cloud and flying debris).
void AddEffect(int kind, const float pos[3]);
// Guest physical memory for the index lists of bodies that came apart (set
// before the GPU starts).
inline uint32_t scratch_physical = 0, scratch_size = 0;

// Any thread. feet: the soldier's position (its feet, y up); damage in health
// points (100 is a full soldier). A death with gib set bursts the body apart;
// push is the direction of the killing blow (0 if unknown).
void AddHit(const float feet[3], float damage);
void AddDeath(const float feet[3], float damage, bool gib, const float push[3]);

// GPU thread, for every lit main-view draw: vertex c12-c15 (object to clip)
// and c16-c18 (object to world).
void SetCamera(const float vs[19][4]);

// GPU thread, once a frame (the scene effects' fog pass): steps the particles
// and body pieces and writes up to max particles for the shader as pairs
// (position, size in metres), (kind, seed, age 0...1, spread 0...1); returns
// how many. Kinds: 0 puff, 1 flying chunk, 2 splat, 3 drop, 4 chunk on the
// ground.
uint32_t BuildGore(float (*out)[4], uint32_t max);

// The first bone constant of a skinned vertex shader (its 3x4 bones read as
// c[base+a0], c[base+1+a0], c[base+2+a0]), or -1 if it is not one. disassembly:
// the shader's microcode disassembly.
int BoneBase(const char* disassembly);

// GPU thread, before a draw of a skinned vertex shader: if its bones belong
// to a soldier who came apart, moves them with the pieces in constants
// (vertex c0-c255, 4 floats each) and returns true; RestoreBones puts the
// game's values back after the draw. key tells the meshes apart (shader and
// vertex buffer); base from BoneBase.
bool OverrideBones(uint64_t key, float* constants, int base);
void RestoreBones(float* constants);
// After OverrideBones returned true: an id of the body and mesh it matched,
// and the piece a bone (its first constant) went with, -1 if none.
uint64_t LastMatch();
int LastPiece(int reg);

// Where a skinned vertex shader fetches each vertex's bone indices (an
// 8_8_8_8 integer fetch, bone * 3 the constant offset): fetch constant vf,
// dword offset and stride in a vertex, and the dword of the four bones'
// weights (a second 8_8_8_8 fetch, -1 for one bone a vertex: the x byte).
// False if it has none.
bool BoneFetch(const char* disassembly, int* vf, int* offset, int* stride, int* weights);

}  // namespace bflook
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_BF_GORE_H_
