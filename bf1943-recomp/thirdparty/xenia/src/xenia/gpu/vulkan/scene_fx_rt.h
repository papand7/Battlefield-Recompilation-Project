// bf1943 scene effects: hardware ray-traced sun shadows. The geometry of the
// game's lit main-view draws (static meshes, buildings, vehicles) goes into
// bottom-level acceleration structures, built once per mesh from guest memory;
// every frame a top-level one places them with the draws' world matrices.
// Static objects seen before stay in it while they are off screen, so casters
// behind the camera or outside the view still shadow what is on screen, at
// any distance.

#ifndef XENIA_GPU_VULKAN_SCENE_FX_RT_H_
#define XENIA_GPU_VULKAN_SCENE_FX_RT_H_

#include <cstdint>

#include "xenia/gpu/register_file.h"
#include "xenia/gpu/shader.h"
#include "xenia/memory.h"
#include "xenia/ui/vulkan/vulkan_api.h"
#include "xenia/ui/vulkan/vulkan_device.h"

namespace xe {
namespace gpu {
namespace vulkan {
class VulkanCommandProcessor;
}  // namespace vulkan
namespace scenefx {
namespace rt {

// A main-view draw (see CaptureDraw), in any of the depth slices the game
// splits the world into by distance: keeps its mesh and world matrix for this
// frame's acceleration structure.
void RecordDraw(const Shader& vertex_shader, const Shader& pixel_shader,
                const RegisterFile& regs, const void* depth_target,
                Memory& memory);
// Draws after the world depth copy (BuildFrame: the first-person weapon, sky,
// particles) are left out until the scene resolve turns recording back on.
void ResumeRecording();

// A terrain patch draw (a 16-bit grid lifted by a heightmap fetched in the
// vertex shader, see IsTerrainVertexShader): the whole heightmap becomes
// meshes once, so hills out of view cast too.
bool IsTerrainVertexShader(const Shader& vertex_shader);
void RecordTerrain(const RegisterFile& regs, Memory& memory);

// Creates the Vulkan objects; false if ray queries are unavailable.
bool Init(const ui::vulkan::VulkanDevice* device);
// The top-level acceleration structure (valid after Init).
VkAccelerationStructureKHR Tlas();
// The meshes' triangle normals (two 16-bit octahedral coordinates each, in
// object space): an instance's custom index is its mesh's first, 0xFFFFFF if
// it has none; the primitive index counts from there. Then the alpha-tested
// triangles' texture coordinates and masks (scene_fx_rt.cc kAlphaBase).
VkBuffer Normals(VkDeviceSize* size);

// At the world depth copy, outside a render pass: records the builds of new
// meshes and of this frame's top-level structure from the draws into
// depth_target. view_proj maps world positions to clip space (row-major).
// Returns the number of instances.
// world_from_clip: (clip x, clip y, clip w, 1) to world, for draws without a
// world matrix.
uint32_t BuildFrame(vulkan::VulkanCommandProcessor& command_processor,
                    const void* depth_target, const double view_proj[4][4],
                    const double world_from_clip[4][4]);

}  // namespace rt
}  // namespace scenefx
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_SCENE_FX_RT_H_
