// bf1943 scene effects: volumetric sun light and height fog applied to the HDR
// scene colour right before the game resolves it for its own bloom, exposure
// and HUD passes. Camera, sun and shadow data come from the constants of the
// game's lit draws (see CaptureDraw).

#ifndef XENIA_GPU_VULKAN_SCENE_FX_H_
#define XENIA_GPU_VULKAN_SCENE_FX_H_

#include <cstdint>

#include "xenia/gpu/register_file.h"
#include "xenia/gpu/shader.h"
#include "xenia/memory.h"

namespace xe {
namespace gpu {
namespace scenefx {

// What one lit draw of the main view tells about the frame.
struct Capture {
  float vs[19][4];      // vertex c0-c18: shadow slices (c0-c2, c4-c6, c8-c10),
                        // world-view-projection (c12-c15), world (c16-c18)
  float ps[8][4];       // pixel c0-c7: exposure, sun direction, up, sky and
                        // ground ambient, sun colour, specular, camera
  uint32_t tf0[6];      // shadow map fetch constant
  float viewport_z_scale, viewport_z_offset;
  const void* depth_target;  // RenderTargetCache::RenderTarget bound for it
  const void* color_target;
  uint64_t vs_hash, ps_hash;
  // The slices take the world position (from c16-c18), not the object's.
  bool slices_world;
};

// Tunables: "key=value,..." after --scene-fx (keys as in ParseSettings).
struct Settings {
  float max_distance = 600.0f;  // metres marched per pixel
  int steps = 32;
  float density = 0.0018f;      // extinction per metre at the base height
  float falloff = 1.0f / 80.0f; // 1 / scale height in metres
  float base_height = 0.0f;     // densest fog, metres over the measured sea
  float shafts = 0.6f;          // how much darker shadowed air is (0...1)
  float sky = 0.35f;            // fog density in front of the sky
  float shadow_bias = 0.002f;
  float eye_height = 20.0f;     // over the sea until the waterline is measured
  // Screen-space sun shadows: every surface pixel marches towards the sun
  // through the world depth, so whatever is on screen casts, whatever the
  // game's shadow map left out.
  float ss_strength = 1.0f;     // 0 off, 1 the sun's whole share removed
  float ss_length = 250.0f;     // longest march in metres
  int ss_steps = 48;
  float ss_map = 0.0f;          // 1: the game's shadow map darkens surfaces again (debugging)
  float ss_thickness = 2.0f;    // metres (plus 2 % of the distance) an occluder is assumed deep
  // Hardware ray-traced sun shadows (scene_fx_rt.h), where the GPU has ray
  // queries: the world's meshes cast at any distance, on screen or not.
  float rt = 1.0f;               // 0 off
  float rt_distance = 3000.0f;   // longest ray in metres
  // 1: every sun shadow is drawn here, the game's own are off: the
  // ray-traced meshes and terrain, and the game's shadow map only for what
  // the rays do not have (soldiers, billboards). 0: the game's, plus the
  // rays'. Key own_shadows.
  float shadows = 1.0f;
  // Ray-traced light (with rt): what the world's meshes hit is coloured from
  // a world-space cache of what was seen on screen (cells of rt_cell metres).
  // Sky light and bounce light: rays over each surface's hemisphere, at half
  // resolution, kept over frames, scale the game's ambient share.
  float gi = 1.0f;               // 0 off
  int gi_rays = 2;               // per half-resolution pixel and frame
  float gi_distance = 120.0f;    // longest bounce ray in metres
  float gi_frames = 24.0f;       // frames a pixel's light is averaged over
  // Contact shading: what rays hit nearer than this (m) gives back less
  // light than its cell's average (a corner, the underside of a plane right
  // above), 0 off.
  float gi_contact = 2.5f;
  // Reflections: the sea (Fresnel of water, waves) and, weaker, everything else.
  float reflect = 1.0f;          // the sea, 0 off
  float gloss = 1.0f;            // other surfaces, by their --pbr material
  float waves = 1.0f;            // the sea's waves in the reflections
  float rt_cell = 0.5f;
  // Volumetric clouds: a layer over the sea, ray-marched for the sky (and
  // anything seen through it from above), lit by the sun and the sky, drifting
  // with the wind; their shadows move over the ground.
  float clouds = 0.45f;          // cover 0...1, 0 off
  float cloud_base = 1400.0f;    // metres over the sea
  float cloud_thickness = 1100.0f;
  int cloud_steps = 32;
  float cloud_density = 0.012f;  // extinction per metre in a full cloud
  float cloud_wind = 10.0f;      // metres a second
  float cloud_shadows = 0.85f;   // 0 off, 1 the sun's whole share under a thick cloud
  // Ambient occlusion (GTAO) from the depth, at half resolution: corners,
  // contact under objects, vegetation. Mostly the ambient share of the light,
  // ao_direct of the sun's.
  float ao = 1.0f;               // strength, 0 off
  float ao_radius = 2.0f;        // metres
  float ao_direct = 0.5f;
  float ao_distance = 250.0f;    // none further away (metres)
  // Water: what is seen through the sea bent by its waves, foam where it meets
  // hulls and shores (traced), and shallow water clearer and turquoise.
  float water_refract = 1.0f;    // 0 off
  float water_foam = 1.0f;       // 0 off (needs rt)
  float water_shallow = 1.0f;    // 0 off
  float foam_width = 3.0f;       // metres from a hull or the shore
  // Procedural surface detail from the world position: pebbles and cracks on
  // bare ground (the traced terrain, not grass), grime on everything, more in
  // corners. Fades out where it would be a few pixels.
  float detail = 1.0f;           // strength, 0 off
  float detail_distance = 60.0f; // metres
  float pebble_size = 0.09f;     // metres
  float crack_size = 0.7f;       // metres between cracks
  // The muzzle flash's light: the game's lights everything near the gun (its
  // light fades only as 1 / distance, unshadowed); with flash > 0 the world
  // draws get it off and this pass lights the world instead, fading with the
  // square of the distance to nothing at flash_radius, shadowed by the traced
  // meshes. The first-person weapon keeps the game's.
  float flash = 1.0f;            // strength, 0 keeps the game's light
  float flash_radius = 6.0f;     // metres
  uint32_t debug = 0;  // 1 distance/height, 2 shadow slices, 3 depth, 4 height, 7 shadow map, 8 screen-space shadows, 9 ray-traced shadows, 10 ray-traced light, 11 reflections, 12 colour cache, 13 materials, 14-16 traced terrain, all meshes, alpha-tested meshes seen from the eye
};

// Before the GPU starts: turns the effects on with the given settings.
void Configure(const char* settings);
bool Enabled();
const Settings& GetSettings();
// The eye (world position) of the main view, from the last lit draw; false
// when no level view was drawn in the last half second.
bool Eye(float out[3]);

// Called for every draw after the render targets are set up; keeps the
// constants of the latest lit main-view draw.
void CaptureDraw(const Shader* vertex_shader, const Shader* pixel_shader,
                 const RegisterFile& regs, const void* depth_target,
                 const void* color_target, Memory& memory);
// The latest capture if there was one since the last Consume (this frame).
bool TakeFresh(Capture& out);
// For a draw about to be issued: true if its pixel shader is the game's
// version with the muzzle flash's point light (c6 its position, near the
// eye, c7 its colour) and the draw is of the world (before the depth copy),
// so c7 should go to the GPU as zero; the light is then kept for this
// frame's pass.
bool TakeOverFlash(const Shader* pixel_shader, const RegisterFile& regs);

}  // namespace scenefx
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_SCENE_FX_H_
