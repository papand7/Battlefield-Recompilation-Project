// bf1943 scene effects (gpu/vulkan/scene_fx.cc): volumetric sun light and
// height fog on the HDR scene colour, before the game's bloom and exposure.
// Pass 0 saves the world depth when the game copies it out (before it clears
// it for the weapon). On the finished HDR scene, passes 3 and 2 measure the sky
// colour just above the horizon in 64 screen columns (kept by world
// azimuth, so turning around keeps what was seen), and pass 1 applies the
// fog, which fades towards that colour, so fogged surfaces meet the horizon
// exactly whatever the game's exposure and tone mapping do.
// The sun light is ray-marched through the game's own shadow map, which is
// read straight from guest memory (three 1600x1600 tiled 24_8 slices).
// Screen-space sun shadows: each surface pixel marches towards the sun
// through the saved world depth; where something on screen is in the way and
// the game's own shadow map says lit, the sun's share of the light is taken
// out. Everything drawn casts, at any distance.
// With SCENE_FX_RT (scene_fx_rt_cs, on GPUs with ray queries) each surface
// pixel also traces a ray to the sun through the world's meshes
// (gpu/vulkan/scene_fx_rt.h), so casters off screen shadow it too.
#version 460
#extension GL_EXT_samplerless_texture_functions : require
#ifdef SCENE_FX_RT
#extension GL_EXT_ray_query : require
#endif

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0, rgba16f) uniform image2D scene;
layout(set = 0, binding = 1) uniform texture2D scene_depth;
layout(set = 0, binding = 2, std430) readonly buffer ShadowMemory {
  uint shadow_words[];
};
layout(set = 0, binding = 4, r32f) uniform image2D saved_depth;
// The clouds at half resolution (pass 5): rgb light, a how much they hide.
layout(set = 0, binding = 14, rgba16f) uniform image2D cloud_image;
// The ambient occlusion at half resolution (pass 6): r visibility, g distance.
layout(set = 0, binding = 15, rgba16f) uniform image2D ao_image;  // AO, distance, traced sun shadow
// The scene with what is seen through the sea bent by its waves (pass 7).
layout(set = 0, binding = 16, rgba16f) uniform image2D refract_image;
// The haze at half resolution (pass 8): x in-scattered, y optical depth, z the
// distance (-1 sky).
layout(set = 0, binding = 17, rgba16f) uniform image2D haze_image;
// The world's colour at the depth copy (pass 0), before the game draws its
// particles (impact dust, sparks, smoke): pass 1 lights that and puts what
// the game drew over it back on top unchanged.
layout(set = 0, binding = 18, rgba16f) uniform image2D world_color;
// FSR 3's motion vectors (pass 9): where each pixel was last frame, in
// pixels, previous minus current (the camera's motion; moving objects not).
layout(set = 0, binding = 19, rg16f) uniform image2D motion_image;
layout(set = 0, binding = 5, std430) buffer Horizon {
  vec4 horizon[64];     // rgb sky colour above the horizon by world azimuth
                        // (64 bins over 360 degrees), w: 1 once measured
  vec4 sea;             // x: measured sea level, w: 1 once measured
  vec4 average;         // rgb mean of the measured bins, w: 1 once measured
  vec4 columns[64];     // pass 3 per screen column: rgb sky colour, w: 1 if found
  vec4 column_info[64]; // pass 3 per screen column: x azimuth bin, y waterline
};
#ifdef SCENE_FX_RT
layout(set = 0, binding = 6) uniform accelerationStructureEXT world_meshes;
// Per triangle of every mesh: its object-space normal, two 16-bit octahedral
// coordinates; an instance's custom index is its mesh's first (0xFFFFFF none).
layout(set = 0, binding = 7, std430) readonly buffer TriangleNormals {
  uint triangle_normals[];
};
// After the normals (scene_fx_rt.cc kAlphaBase): per alpha-tested triangle
// its corners' texture coordinates (half2) and its alpha mask (word offset |
// log2 width << 24 | log2 height << 28), from the instance's shader binding
// table record offset; then the masks' bits.
const uint kAlphaBase = 16777216u, kMaskBase = 20971520u;

// Whether a candidate hit on an alpha-tested triangle is where its texture
// passes the draw's alpha test.
bool AlphaSolid(uint first, uint primitive, vec2 bary) {
  uint at = kAlphaBase + (first + primitive) * 4u;
  vec2 uv = unpackHalf2x16(triangle_normals[at]) * (1.0 - bary.x - bary.y) +
            unpackHalf2x16(triangle_normals[at + 1u]) * bary.x +
            unpackHalf2x16(triangle_normals[at + 2u]) * bary.y;
  uint mask = triangle_normals[at + 3u];
  uint log2_w = (mask >> 24) & 15u, log2_h = mask >> 28;
  uvec2 texel = min(uvec2(fract(uv) * vec2(1u << log2_w, 1u << log2_h)),
                    uvec2((1u << log2_w) - 1u, (1u << log2_h) - 1u));
  uint bit = (texel.y << log2_w) + texel.x;
  return ((triangle_normals[kMaskBase + (mask & 0xFFFFFFu) + (bit >> 5)] >> (bit & 31u)) & 1u) != 0u;
}

// Runs a ray query, confirming the alpha-tested candidates that are solid.
#define TRACE_ALPHA_TESTED(query) \
  while (rayQueryProceedEXT(query)) { \
    if (AlphaSolid(rayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetEXT(query, false), \
                   uint(rayQueryGetIntersectionPrimitiveIndexEXT(query, false)), \
                   rayQueryGetIntersectionBarycentricsEXT(query, false))) { \
      rayQueryConfirmIntersectionEXT(query); \
    } \
  }
// What surfaces looked like on screen, by world cell and facing: x check (0
// empty), y-z half rgb and weight, w the frame it was last written.
layout(set = 0, binding = 8, std430) buffer ColorCache {
  uvec4 cache[];
};
// The ray-traced light at half resolution: rgb the ambient light over the
// open sky's, a frames averaged; read last frame's, write this frame's, with
// their distances from the camera (0 for none).
layout(set = 0, binding = 9, rgba16f) uniform image2D light_history;
layout(set = 0, binding = 10, rgba16f) uniform image2D light_out;
layout(set = 0, binding = 11, r32f) uniform image2D light_history_dist;
layout(set = 0, binding = 12, r32f) uniform image2D light_out_dist;
// The --pbr pixels' materials (runtime/xenia_shims/pbr_shaders.h): per pixel
// the nearest one's distance (top 24 bits), roughness << 4 and metallic (4
// bits each); all ones for none.
layout(set = 0, binding = 13, std430) buffer Materials {
  uint materials[];
};
const uint kMaterialPitch = 4096u, kMaterialRows = 2560u;
// The scene's view depth (clip w) for the soft particles, after the materials.
const uint kMaterialDepthOffset = kMaterialPitch * kMaterialRows;
// The camera for the lit shaders without a view vector (pbr camv=1).
const uint kMaterialCameraOffset = kMaterialDepthOffset * 2u;
#endif
layout(push_constant) uniform Push {
  uint pass_mode;
};
layout(set = 0, binding = 3, std140) uniform Params {
  // NDC (x, y, depth) -> world minus the camera: camera-relative, so float
  // precision is not lost to world coordinates hundreds of metres out (that
  // stepped the view directions and the normals from the depth in blocks,
  // and the sun rays self-hit there, flickering as the camera swayed).
  mat4 inv_view_proj;
  vec4 slices[9];       // world -> shadow (x, y in -0.5...0.5, z depth), 3 rows each
  vec4 camera;          // xyz, w: the lit shaders' exposure
  vec4 sun_dir;         // xyz towards the sun, w: fog density at the base height
  vec4 sun_color;       // rgb, w: fog height falloff (1 / scale height)
  vec4 ambient;         // rgb sky ambient, w: fog base height over the sea
  vec4 screen;          // pixel offset xy, 1 / size zw
  vec4 march;           // x: max distance, y: steps, w: frame
  vec4 strength;        // x: shaft darkness, y: shadow map in the surface
                        // shadows, z: shadow bias
  uvec4 shadow_info;    // x: slice size, y: slice stride (words), z: endian swap,
                        // w: debug view
  vec4 water;           // x: eye height over the sea until it is measured,
                        // y: 1 with reverse Z, z: fog density
                        // in front of the sky
  mat4 view_proj;       // world -> (NDC x, y, depth value)
  vec4 ss;              // screen-space shadows: x strength, y length (m),
                        // z steps, w thickness (m)
  vec4 rt;              // ray-traced shadows: x on, y length (m), z instances, w sun radius (rad)
  mat4 prev_view_proj;  // last frame's view_proj
  vec4 prev_camera;     // last frame's eye, w: 1 if last frame's light is usable
  vec4 gi;              // ray-traced light: x strength (0 off), y rays,
                        // z colour cache cell (m), w frame
  vec4 gi2;             // x longest bounce ray (m), y frames averaged, z all sun shadows here,
                        // w contact shading reach (m)
  vec4 refl;            // reflections: x the sea, y other surfaces, z seconds,
                        // w waves
  vec4 gore_info;       // blood (gpu/vulkan/bf_gore.cc): x count, y brightness
  vec4 gore[128];       // per particle: (position, size in metres),
                        // (kind, seed, age 0...1, spread 0...1); kinds: 0 puff,
                        // 1 flying chunk, 2 splat, 3 drop, 4 chunk on the ground
  vec4 cloud;           // volumetric clouds: x cover (0 off), y base over the
                        // sea (m), z thickness (m), w seconds
  vec4 cloud2;          // x steps, y extinction per metre, z wind (m/s),
                        // w shadows on the ground
  vec4 ao;              // ambient occlusion: x strength (0 off), y radius (m),
                        // z the sun's share it darkens, w furthest (m)
  vec4 wfx;             // water: x foam (0 off), y refraction, z shallow,
                        // w foam width (m)
  vec4 flash;           // muzzle flash light: xyz world position, w strength (0 none)
  vec4 flash2;          // rgb its colour (the game's, like ambient), w radius (m)
  vec4 detail;          // surface detail: x strength (0 off), y furthest (m),
                        // z pebble size (m), w crack cell size (m)
};

shared vec4 found[64];
shared float found_bin[64];
shared float waterline[64];
shared int lowest_sky_row;

// Xenos 2D tiled addressing (texture_address.h Tiled2D), 4 bytes per texel.
uint TiledOffset2D(uint x, uint y, uint pitch) {
  uint outer_blocks = (((y >> 5u) * (pitch >> 5u)) + (x >> 5u)) << 6u;
  uint inner_blocks = (((y >> 1u) & 7u) << 3u) | (x & 7u);
  uint b = (outer_blocks | inner_blocks) << 2u;
  uint bank = (y >> 4u) & 1u;
  uint pipe = ((x >> 3u) & 3u) ^ (((y >> 3u) & 1u) << 1u);
  return ((y & 1u) << 4u) | (pipe << 6u) | (bank << 11u) | (b & 15u) |
         (((b >> 4u) & 1u) << 5u) | (((b >> 5u) & 7u) << 8u) |
         ((b >> 8u) << 12u);
}

float ShadowDepth(uint slice, uvec2 texel) {
  uint size = shadow_info.x;
  uint word = shadow_words[slice * shadow_info.y +
                           (TiledOffset2D(texel.x, texel.y, size) >> 2u)];
  if (shadow_info.z != 0u) {
    word = (word >> 24u) | ((word >> 8u) & 0xFF00u) | ((word << 8u) & 0xFF0000u) |
           (word << 24u);
  }
  return float(word >> 8u) * (1.0 / 16777215.0);
}

// 1 where the sun reaches the point, 0 in shadow; points outside the shadow
// slices count as lit.
float SunVisibility(vec3 p, out int slice_out) {
  vec4 pw = vec4(p, 1.0);
  for (int i = 0; i < 3; ++i) {
    vec3 s = vec3(dot(slices[i * 3], pw), dot(slices[i * 3 + 1], pw),
                  dot(slices[i * 3 + 2], pw));
    if (all(lessThan(abs(s.xy), vec2(0.499))) && s.z > 0.0 && s.z < 1.0) {
      slice_out = i;
      float size = float(shadow_info.x);
      uvec2 texel = uvec2(clamp((s.xy + 0.5) * size, vec2(0.0), vec2(size - 1.0)));
      return s.z - strength.z <= ShadowDepth(uint(i), texel) ? 1.0 : 0.0;
    }
  }
  slice_out = 3;
  return 1.0;
}

// SunVisibility with, in shadow, the distance towards the sun to the map's
// occluder and the size of a map texel there (metres).
float SunVisibilityAt(vec3 p, out float t_occ, out float texel_m) {
  vec4 pw = vec4(p, 1.0);
  vec3 to_sun = normalize(sun_dir.xyz);
  t_occ = 0.0;
  texel_m = 0.0;
  for (int i = 0; i < 3; ++i) {
    vec3 s = vec3(dot(slices[i * 3], pw), dot(slices[i * 3 + 1], pw),
                  dot(slices[i * 3 + 2], pw));
    if (all(lessThan(abs(s.xy), vec2(0.499))) && s.z > 0.0 && s.z < 1.0) {
      float size = float(shadow_info.x);
      uvec2 texel = uvec2(clamp((s.xy + 0.5) * size, vec2(0.0), vec2(size - 1.0)));
      float d = ShadowDepth(uint(i), texel);
      if (s.z - strength.z <= d) return 1.0;
      float dz = -dot(slices[i * 3 + 2].xyz, to_sun);
      t_occ = dz > 1e-9 ? (s.z - d) / dz : 0.0;
      texel_m = 1.0 / (size * length(slices[i * 3].xyz));
      return 0.0;
    }
  }
  return 1.0;
}

// SunVisibility filtered (percentage closer, 4x4 texels with bilinear
// weights): 0...1, smooth over about two texels instead of the map's steps.
float SunVisibilitySoft(vec3 p) {
  vec4 pw = vec4(p, 1.0);
  for (int i = 0; i < 3; ++i) {
    vec3 s = vec3(dot(slices[i * 3], pw), dot(slices[i * 3 + 1], pw),
                  dot(slices[i * 3 + 2], pw));
    if (all(lessThan(abs(s.xy), vec2(0.495))) && s.z > 0.0 && s.z < 1.0) {
      float size = float(shadow_info.x);
      vec2 g = (s.xy + 0.5) * size - 0.5;
      vec2 base = floor(g) - 1.0;
      vec2 f = g - floor(g);
      // Weights of the 4 columns (rows): a tent over two texels.
      vec4 wx = vec4(1.0 - f.x, 2.0 - f.x, 1.0 + f.x, f.x) * 0.25;
      vec4 wy = vec4(1.0 - f.y, 2.0 - f.y, 1.0 + f.y, f.y) * 0.25;
      float lit = 0.0;
      for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
          uvec2 texel = uvec2(clamp(base + vec2(x, y), vec2(0.0), vec2(size - 1.0)));
          float d = ShadowDepth(uint(i), texel);
          lit += (s.z - strength.z <= d ? 1.0 : 0.0) * wx[x] * wy[y];
        }
      }
      return lit;
    }
  }
  return 1.0;
}

// SunVisibility for p = origin + dir * t, with the slice coordinates of the
// origin (base) and their change per metre (delta) worked out once per ray.
float SunVisibilityAlong(float t, vec3 base[3], vec3 delta[3]) {
  for (int i = 0; i < 3; ++i) {
    vec3 s = base[i] + delta[i] * t;
    if (all(lessThan(abs(s.xy), vec2(0.499))) && s.z > 0.0 && s.z < 1.0) {
      float size = float(shadow_info.x);
      uvec2 texel = uvec2(clamp((s.xy + 0.5) * size, vec2(0.0), vec2(size - 1.0)));
      return s.z - strength.z <= ShadowDepth(uint(i), texel) ? 1.0 : 0.0;
    }
  }
  return 1.0;
}

// Under DLSS / FSR 3 (march.w >= 64): fewer samples, and the noise that was
// fixed per pixel changes every frame (the upscaler averages it away).
bool Temporal() { return march.w >= 64.0; }
float TemporalFrame() { return Temporal() ? march.w : 0.0; }

float InterleavedGradientNoise(vec2 pixel, float frame) {
  pixel += 5.588238 * mod(frame, 64.0);
  return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

bool IsSky(float depth) {
  // Nothing drawn: the depth clear value (0 with reverse Z).
  return water.y != 0.0 ? depth <= 0.0 : depth >= 1.0;
}

vec3 ViewDir(vec2 local_pixel) {
  vec2 uv = (local_pixel + 0.5) * screen.zw;
  vec4 f = inv_view_proj * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.5, 1.0);
  return normalize(f.xyz / f.w);
}

float AzimuthBin(vec3 dir) {
  return fract(atan(dir.z, dir.x) * 0.159154943 + 1.0) * 64.0;
}

#ifdef SCENE_FX_RT
float TraceMeshes(vec3 origin, vec3 dir, float range, out vec3 normal);
#endif

// Pass 3: one workgroup per screen column, its 64 threads splitting the rows.
void ScanColumn() {
  uint column = gl_WorkGroupID.x;
  uint lane = gl_LocalInvocationIndex;
  ivec2 size = ivec2(1.0 / screen.zw + 0.5);
  int x = int((float(column) + 0.5) / 64.0 * float(size.x));
  if (lane == 0u) lowest_sky_row = -1;
  barrier();
  // The lowest sky pixel of the column that looks up (not the sea beyond the
  // drawn water), and the waterline: the sea is drawn after the depth copy, so
  // it has no depth; where an object with depth (a hull, a beach) sits right
  // above sea pixels, that object pixel is at the waterline.
  int best = -1;
  float lowest = 1e9;
  for (int y = size.y - 1 - int(lane); y >= 0; y -= 64) {
    float d = imageLoad(saved_depth, ivec2(x, y)).r;
    if (IsSky(d)) {
      if (y > best && ViewDir(vec2(x, y)).y >= 0.0) best = y;
      continue;
    }
    if (y + 1 >= size.y) continue;
    vec2 uv = (vec2(x, y) + 0.5) * screen.zw;
    vec4 w4 = inv_view_proj * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0);
    if (w4.w <= 1e-6) continue;  // counts as empty
    float db = imageLoad(saved_depth, ivec2(x, y + 1)).r;
    vec2 uvb = (vec2(x, y + 1) + 0.5) * screen.zw;
    vec4 w4b = inv_view_proj * vec4(uvb.x * 2.0 - 1.0, 1.0 - uvb.y * 2.0, db, 1.0);
    bool below_empty = IsSky(db) || w4b.w <= 1e-6;
    if (below_empty && ViewDir(vec2(x, y + 1)).y < 0.0) {
      bool at_water = true;
#ifdef SCENE_FX_RT
      // Only where the object goes on under the water (a hull, a beach): the
      // ray just below it hits it again nearby. Below a deck's far edge it
      // misses (the sea seen beyond), which made the deck the sea level: the
      // deck then got the sea's waves and reflections and lost its shadows.
      if (rt.x > 0.0) {
        float t_edge = length(w4.xyz / w4.w);
        vec3 n;
        float t = TraceMeshes(camera.xyz, ViewDir(vec2(x, y + 1)), t_edge * 1.5 + 30.0, n);
        at_water = t > 0.0 && t < t_edge + 5.0 + 0.05 * t_edge;
      }
#endif
      if (at_water) lowest = min(lowest, camera.y + w4.y / w4.w);
    }
  }
  atomicMax(lowest_sky_row, best);
  waterline[lane] = lowest;
  barrier();
  for (uint step = 32u; step > 0u; step >>= 1u) {
    if (lane < step) waterline[lane] = min(waterline[lane], waterline[lane + step]);
    barrier();
  }
  if (lane != 0u) return;
  vec4 result = vec4(0.0);
  int y = lowest_sky_row;
  // Measured only if the horizon shows in this column.
  if (y >= 0 && ViewDir(vec2(x, y)).y <= 0.12) {
    vec3 sum = vec3(0.0);
    float n = 0.0;
    for (int k = 2; k < 14 && y - k >= 0; ++k) {
      if (!IsSky(imageLoad(saved_depth, ivec2(x, y - k)).r)) continue;
      sum += imageLoad(scene, ivec2(x, y - k) + ivec2(screen.xy)).rgb;
      n += 1.0;
    }
    if (n > 0.0 && !any(isnan(sum)) && !any(isinf(sum))) result = vec4(sum / n, 1.0);
  }
  columns[column] = result;
  column_info[column] = vec4(AzimuthBin(ViewDir(vec2(x, size.y / 2))), waterline[0], 0.0, 0.0);
}

// Pass 2 (one workgroup): fold the pass 3 columns into the azimuth bins.
void MeasureHorizon() {
  uint column = gl_LocalInvocationIndex;
#ifdef SCENE_FX_RT
  if (column == 0u) {
    materials[kMaterialCameraOffset] = floatBitsToUint(camera.x);
    materials[kMaterialCameraOffset + 1u] = floatBitsToUint(camera.y);
    materials[kMaterialCameraOffset + 2u] = floatBitsToUint(camera.z);
  }
#endif
  found[column] = columns[column];
  found_bin[column] = column_info[column].x;
  waterline[column] = column_info[column].y;
  barrier();
  // Now one thread per azimuth bin: blend in the columns that fall near it.
  uint bin = column;
  vec4 sum = vec4(0.0);
  // Columns much brighter or darker than the measured sky are left out: a
  // muzzle flash, smoke or an explosion over the horizon (drawn after the
  // depth copy, so they look like sky) brightened the haze, fog and clouds.
  const vec3 lum = vec3(0.2126, 0.7152, 0.0722);
  float mean = average.w != 0.0 ? dot(average.rgb, lum) : 0.0;
  for (int i = 0; i < 64; ++i) {
    float d = abs(found_bin[i] - float(bin) - 0.5);
    d = min(d, 64.0 - d);
    float l = dot(found[i].rgb, lum);
    bool odd = mean > 0.0 && (l > 1.6 * mean || l < 0.5 * mean);
    float w = odd ? 0.0 : found[i].w * max(1.5 - d, 0.0);
    sum += vec4(found[i].rgb, 1.0) * w;
  }
  vec4 old = horizon[bin];
  if (sum.w > 0.0) {
    vec3 c = sum.rgb / sum.w;
    horizon[bin] = old.w != 0.0 ? vec4(mix(old.rgb, c, 0.15), 1.0) : vec4(c, 1.0);
  }
  barrier();
  if (bin == 0u) {
    float level = 1e9;
    for (int i = 0; i < 64; ++i) level = min(level, waterline[i]);
    if (level < camera.y && level > camera.y - 500.0) {
      sea = sea.w != 0.0 ? vec4(mix(sea.x, level, 0.1), 0.0, 0.0, 1.0)
                         : vec4(level, 0.0, 0.0, 1.0);
    }
    vec4 total = vec4(0.0);
    for (int i = 0; i < 64; ++i) total += vec4(horizon[i].rgb * horizon[i].w, horizon[i].w);
    if (total.w > 0.0) average = vec4(total.rgb / total.w, 1.0);
  }
}

// A pixel's position relative to the camera.
vec3 RelativeAt(ivec2 local, float depth) {
  vec2 uv = (vec2(local) + 0.5) * screen.zw;
  vec4 w4 = inv_view_proj * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
  return w4.xyz / w4.w;
}

vec3 WorldAt(ivec2 local, float depth) { return camera.xyz + RelativeAt(local, depth); }

// 1 where something on screen stands between the point and the sun.
float ScreenSpaceShadow(vec3 p, vec3 normal, float dist, ivec2 size, float jitter) {
  vec3 to_sun = normalize(sun_dir.xyz);
  // Off the surface a little (more further away, where depth is coarser).
  p += normal * (0.02 + 0.002 * dist);
  // Long enough for a low sun behind a tall caster, even right at the camera.
  float len = min(ss.y, max(60.0, dist));
  int steps = int(ss.z);
  vec4 c0 = view_proj * vec4(p, 1.0);
  vec4 dc = view_proj * vec4(to_sun, 0.0);
  for (int i = 0; i < steps; ++i) {
    // Denser near the start: contact shadows, then long ones.
    float f = (float(i) + jitter) / float(steps);
    float t = len * f * f + 0.05;
    vec3 q = p + to_sun * t;
    vec4 c = c0 + dc * t;
    if (c.w <= 1e-6) return 0.0;
    vec2 ndc = c.xy / c.w;
    if (any(greaterThan(abs(ndc), vec2(1.0)))) return 0.0;  // off screen: unknown, lit
    ivec2 at = ivec2(vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * vec2(size));
    float d = imageLoad(saved_depth, at).r;
    if (IsSky(d)) continue;
    float scene_dist = length(WorldAt(at, d) - camera.xyz);
    float q_dist = length(q - camera.xyz);
    float behind = q_dist - scene_dist;
    // Plus this step's length, so a thin beam is not stepped over.
    float thickness = ss.w + 0.02 * q_dist + len * (2.0 * f + 1.0 / float(steps)) / float(steps);
    if (behind > 0.01 + 0.002 * q_dist && behind < thickness) return 1.0;
  }
  return 0.0;
}

#ifdef SCENE_FX_RT
// Whether a sun ray from origin along dir hits one of the world's meshes.
float SunRayBlocked(vec3 origin, vec3 dir) {
  rayQueryEXT query;
  // Back faces culled: only the terrain has them (the other meshes' instances
  // opt out), where the ray starts under the heightmap's surface.
  rayQueryInitializeEXT(query, world_meshes,
                        gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsCullBackFacingTrianglesEXT,
                        0xFF, origin, 0.0, dir, rt.y);
  TRACE_ALPHA_TESTED(query)
  return rayQueryGetIntersectionTypeEXT(query, true) !=
                 gl_RayQueryCommittedIntersectionNoneEXT
             ? 1.0
             : 0.0;
}
#endif

// One hard ray to the sun's centre (far away, where a penumbra is under a
// pixel wide).
float RayShadowHard(vec3 p, vec3 normal, float dist) {
#ifdef SCENE_FX_RT
  vec3 to_sun = normalize(sun_dir.xyz);
  return SunRayBlocked(p + normal * (0.03 + 0.002 * dist) + to_sun * (0.02 + 0.001 * dist), to_sun);
#else
  return 0.0;
#endif
}

// How much of the sun's disc the world's meshes hide from the point (0...1).
// Three rays over the disc; where they disagree (a penumbra) seven more. A
// fixed per-pixel rotation (no frame term): steady, the resolve averages it.
float RayShadow(vec3 p, vec3 normal, float dist, vec2 pixel) {
#ifdef SCENE_FX_RT
  vec3 to_sun = normalize(sun_dir.xyz);
  // Off the surface: the position rebuilt from the depth is coarser further
  // away.
  vec3 origin = p + normal * (0.03 + 0.002 * dist) + to_sun * (0.02 + 0.001 * dist);
  if (rt.w <= 0.0) return SunRayBlocked(origin, to_sun);
  vec3 side = normalize(cross(to_sun, abs(to_sun.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
  vec3 up = cross(side, to_sun);
  float turn = InterleavedGradientNoise(pixel, TemporalFrame()) * 6.2831853;
  // Vogel disc: 10 points, the first 3 spread over the whole disc. Under an
  // upscaler 2, and 3 more in a penumbra, turned every frame.
  const int kOrder[10] = int[10](0, 6, 3, 8, 1, 5, 9, 2, 7, 4);
  int first = Temporal() ? 2 : 3, last = Temporal() ? 5 : 10;
  float blocked = 0.0;
  int n = 0;
  for (int i = 0; i < last; ++i) {
    if (i == first && (blocked == 0.0 || blocked == float(first))) break;
    float k = float(kOrder[i]);
    float r = sqrt((k + 0.5) / 10.0) * rt.w;
    float a = k * 2.3999632 + turn;
    vec3 dir = normalize(to_sun + (side * cos(a) + up * sin(a)) * r);
    blocked += SunRayBlocked(origin, dir);
    ++n;
  }
  return blocked / float(n);
#else
  return 0.0;
#endif
}

#ifdef SCENE_FX_RT
bool RayHits(vec3 origin, vec3 dir, float t_max) {
  rayQueryEXT query;
  rayQueryInitializeEXT(query, world_meshes, gl_RayFlagsTerminateOnFirstHitEXT, 0xFF, origin,
                        0.0, dir, t_max);
  TRACE_ALPHA_TESTED(query)
  return rayQueryGetIntersectionTypeEXT(query, true) !=
         gl_RayQueryCommittedIntersectionNoneEXT;
}
#endif

// Whether the game's shadow map's occluder at o (towards the sun from a
// shadowed point) is one of the ray-traced meshes: short rays across and
// along the sun ray find one within r. If not, it is something only the map
// has (foliage, soldiers).
bool OccluderTraced(vec3 o, float r) {
#ifdef SCENE_FX_RT
  vec3 l = normalize(sun_dir.xyz);
  vec3 a = normalize(cross(l, abs(l.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
  vec3 b = cross(l, a);
  vec3 start = o - l * r;
  if (RayHits(start, l, 2.0 * r)) return true;
  for (int i = 0; i < 4; ++i) {
    vec3 d = i == 0 ? a : i == 1 ? -a : i == 2 ? b : -b;
    if (RayHits(o, d, r)) return true;
  }
  return false;
#else
  return false;
#endif
}

// Debug: a ring around the sun's place on screen.
bool OnSunRing(ivec2 local, ivec2 size) {
  vec4 sc = view_proj * vec4(camera.xyz + normalize(sun_dir.xyz) * 1000.0, 1.0);
  vec2 sp = vec2(sc.x / abs(sc.w) * 0.5 + 0.5, 0.5 - sc.y / abs(sc.w) * 0.5) * vec2(size);
  if (sc.w > 1e-6 && abs(length(vec2(local) - sp) - 30.0) < 4.0) return true;
  // Off screen: a line from the centre towards it (dashed if behind).
  vec2 c = vec2(size) * 0.5;
  vec2 d = normalize(sp - c);
  vec2 rel = vec2(local) - c;
  float along = dot(rel, d);
  bool dash = sc.w > 0.0 || fract(along / 40.0) < 0.5;
  return along > 0.0 && along < 300.0 && abs(rel.x * d.y - rel.y * d.x) < 4.0 && dash;
}

vec3 HazeColor(vec3 dir) {
  // A soft lookup over neighbouring azimuth bins.
  float f = AzimuthBin(dir) - 0.5;
  vec4 sum = vec4(0.0);
  for (int k = -2; k <= 3; ++k) {
    int i = (int(floor(f)) + k + 64) & 63;
    float w = max(3.0 - abs(float(int(floor(f)) + k) - f), 0.0) * horizon[i].w;
    sum += vec4(horizon[i].rgb, 1.0) * w;
  }
  if (average.w != 0.0) {
    // Fade to the mean where few nearby directions were measured.
    return sum.w > 0.0 ? mix(average.rgb, sum.rgb / sum.w, min(sum.w * 0.25, 1.0))
                       : average.rgb;
  }
  if (sum.w > 0.0) return sum.rgb / sum.w;
  // Nothing measured yet: a pale version of the game's sky light.
  return mix(vec3(dot(ambient.rgb, vec3(0.2126, 0.7152, 0.0722))), ambient.rgb,
             0.5) * (0.25 * camera.w);
}

// The surface normal from the neighbouring depths (the nearer side of each),
// facing the camera.
vec3 SurfaceNormal(ivec2 local, vec3 world, ivec2 size) {
  ivec2 lx = min(local + ivec2(1, 0), size - 1), sx = max(local - ivec2(1, 0), ivec2(0));
  ivec2 ly = min(local + ivec2(0, 1), size - 1), sy = max(local - ivec2(0, 1), ivec2(0));
  // Offsets between camera-relative points: they keep their precision.
  vec3 at = RelativeAt(local, imageLoad(saved_depth, local).r);
  vec3 px = RelativeAt(lx, imageLoad(saved_depth, lx).r) - at;
  vec3 mx = RelativeAt(sx, imageLoad(saved_depth, sx).r) - at;
  vec3 py = RelativeAt(ly, imageLoad(saved_depth, ly).r) - at;
  vec3 my = RelativeAt(sy, imageLoad(saved_depth, sy).r) - at;
  vec3 dx = length(px) < length(mx) ? px : -mx;
  vec3 dy = length(py) < length(my) ? py : -my;
  vec3 normal = normalize(cross(dy, dx));
  if (dot(normal, -at) < 0.0) normal = -normal;
  return normal;
}

// The sky seen along dir (the measured horizon colours, a little darker up
// high); below the horizon the sea and land the rays cannot hit.
vec3 SkyColor(vec3 dir) {
  vec3 horizon_color = HazeColor(dir);
  if (dir.y < 0.0) return horizon_color * mix(0.6, 0.3, min(-dir.y * 4.0, 1.0));
  return horizon_color * (1.0 - 0.3 * dir.y);
}

#ifdef SCENE_FX_RT
uint Pcg(uint v) {
  uint s = v * 747796405u + 2891336453u;
  uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
  return (w >> 22u) ^ w;
}

float Random(uvec3 seed) {
  return float(Pcg(seed.x + Pcg(seed.y + Pcg(seed.z))) >> 8u) * (1.0 / 16777216.0);
}

// The colour cache: 2^21 cells, 4 places tried per world cell.
const uint kCacheMask = (1u << 21) - 1u;

uint FacingAxis(vec3 n) {
  vec3 a = abs(n);
  if (a.x > a.y && a.x > a.z) return n.x > 0.0 ? 0u : 1u;
  if (a.y > a.z) return n.y > 0.0 ? 2u : 3u;
  return n.z > 0.0 ? 4u : 5u;
}

void CacheCell(vec3 p, vec3 n, out uint place, out uint check) {
  ivec3 c = ivec3(floor(p / gi.z));
  uint axis = FacingAxis(n);
  place = Pcg(uint(c.x) + Pcg(uint(c.y) + Pcg(uint(c.z) + Pcg(axis))));
  check = Pcg(uint(c.x) * 73856093u ^ uint(c.y) * 19349663u ^ uint(c.z) * 83492791u ^
              (axis + 1u) * 2654435761u) | 1u;
}

bool CacheLoad(vec3 p, vec3 n, out vec3 color) {
  uint place, check;
  CacheCell(p, n, place, check);
  for (uint i = 0u; i < 4u; ++i) {
    uvec4 e = cache[(place + i) & kCacheMask];
    if (e.x == check) {
      color = vec3(unpackHalf2x16(e.y), unpackHalf2x16(e.z).x);
      return true;
    }
  }
  color = vec3(0.0);
  return false;
}

// Blends a surface colour seen on screen into its cell (or takes over the
// stalest of the cell's places). Races between pixels only lose a sample.
void CacheStore(vec3 p, vec3 n, vec3 color) {
  if (any(isnan(color)) || any(isinf(color))) return;
  uint place, check;
  CacheCell(p, n, place, check);
  uint frame = uint(gi.w) + 1u;
  uint target = place & kCacheMask;
  uint oldest = 0xFFFFFFFFu;
  float weight = 0.0;
  vec3 old = color;
  for (uint i = 0u; i < 4u; ++i) {
    uint at = (place + i) & kCacheMask;
    uvec4 e = cache[at];
    if (e.x == check) {
      target = at;
      old = vec3(unpackHalf2x16(e.y), unpackHalf2x16(e.z).x);
      weight = unpackHalf2x16(e.z).y;
      break;
    }
    if (e.w < oldest) {
      oldest = e.w;
      target = at;
    }
  }
  // A surface known for a while that is suddenly much brighter is lit for a
  // moment (a muzzle flash and its sprite in front of it, an explosion): not
  // learnt. Clamping each frame instead still compounded over a burst of fire,
  // and the bounce light then lifted the shadows around the player.
  const vec3 lum = vec3(0.2126, 0.7152, 0.0722);
  if (weight >= 4.0 && dot(color, lum) > 1.3 * dot(old, lum) + 1e-3) return;
  weight = min(weight + 1.0, 30.0);
  vec3 c = mix(old, color, 1.0 / weight);
  cache[target] = uvec4(check, packHalf2x16(c.rg), packHalf2x16(vec2(c.b, weight)), frame);
}

vec3 DecodeNormal(uint packed) {
  vec2 e = unpackSnorm2x16(packed);
  vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
  if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
  return normalize(n);
}

// The nearest mesh along a ray: distance (negative if none) and its world
// normal, facing back along the ray.
float TraceMeshes(vec3 origin, vec3 dir, float range, out vec3 normal) {
  rayQueryEXT query;
  rayQueryInitializeEXT(query, world_meshes, gl_RayFlagsNoneEXT, 0xFF, origin, 0.0, dir, range);
  TRACE_ALPHA_TESTED(query)
  normal = -dir;
  if (rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionNoneEXT) {
    return -1.0;
  }
  uint base = rayQueryGetIntersectionInstanceCustomIndexEXT(query, true);
  if (base != 0xFFFFFFu) {
    uint index = base + uint(rayQueryGetIntersectionPrimitiveIndexEXT(query, true));
    vec3 object_normal = DecodeNormal(triangle_normals[index]);
    // Object to world for normals: the transpose of world to object.
    mat4x3 world_to_object = rayQueryGetIntersectionWorldToObjectEXT(query, true);
    vec3 n = object_normal * mat3(world_to_object);
    if (dot(n, n) > 1e-12) {
      n = normalize(n);
      normal = dot(n, dir) > 0.0 ? -n : n;
    }
  }
  return rayQueryGetIntersectionTEXT(query, true);
}

// What a ray sees where it hits a mesh: the colour cache, or for surfaces
// never seen on screen, a grey surface in the sun and sky light.
vec3 HitColor(vec3 p, vec3 n, out bool known) {
  vec3 color;
  known = CacheLoad(p, n, color);
  if (known) return color;
  // Near a cell's edge the surface may have been seen only in the cell
  // beside it, and a slanted one under its other facing (the normal from the
  // depth leans): try those before guessing.
  vec3 f = fract(p / gi.z) - 0.5;
  uint axis = FacingAxis(n) >> 1u;
  for (uint k = 0u; k < 3u; ++k) {
    if (k == axis) continue;
    vec3 step = vec3(0.0);
    step[k] = (f[k] < 0.0 ? -1.0 : 1.0) * gi.z;
    known = CacheLoad(p + step, n, color);
    if (known) return color;
  }
  vec3 n2 = n;
  n2[axis] = 0.0;
  if (dot(n2, n2) > 0.1) {
    known = CacheLoad(p, n2, color);
    if (known) return color;
  }
  int slice;
  float sun = max(dot(n, normalize(sun_dir.xyz)), 0.0) * SunVisibility(p, slice);
  return 0.12 * (ambient.rgb + sun_color.rgb * sun) * camera.w;
}

#endif

// Volumetric clouds: a layer from cloud.y to cloud.y + cloud.z metres over
// the sea. Where clouds are comes from 2D noise (cloud fields kilometres
// across) against the cover, their shape from the height in the layer (flat
// bottoms, rounded tops), worn at the edges by 3D noise; all drift with the
// wind.
float CloudHash(vec3 p) {
  p = fract(p * vec3(0.1031, 0.1030, 0.0973));
  p += dot(p, p.yxz + 33.33);
  return fract((p.x + p.y) * p.z);
}

float CloudNoise(vec3 p) {
  vec3 i = floor(p);
  vec3 f = p - i;
  f = f * f * (3.0 - 2.0 * f);
  float a = mix(mix(CloudHash(i), CloudHash(i + vec3(1, 0, 0)), f.x),
                mix(CloudHash(i + vec3(0, 1, 0)), CloudHash(i + vec3(1, 1, 0)), f.x), f.y);
  float b = mix(mix(CloudHash(i + vec3(0, 0, 1)), CloudHash(i + vec3(1, 0, 1)), f.x),
                mix(CloudHash(i + vec3(0, 1, 1)), CloudHash(i + vec3(1, 1, 1)), f.x), f.y);
  return mix(a, b, f.z);
}

float CloudNoise2(vec2 p) {
  vec2 i = floor(p);
  vec2 f = p - i;
  f = f * f * (3.0 - 2.0 * f);
  return mix(mix(CloudHash(vec3(i, 0.0)), CloudHash(vec3(i + vec2(1, 0), 0.0)), f.x),
             mix(CloudHash(vec3(i + vec2(0, 1), 0.0)), CloudHash(vec3(i + vec2(1, 1), 0.0)), f.x),
             f.y);
}

float CloudSeaLevel() { return sea.w != 0.0 ? sea.x : camera.y - water.x; }

// 0...1 at p; detail adds the 3D wear (left out for the light and shadows,
// which also take only the two broadest octaves of the fields).
float CloudDensity(vec3 p, bool detail) {
  float h = (p.y - CloudSeaLevel() - cloud.y) / cloud.z;
  if (h <= 0.0 || h >= 1.0) return 0.0;
  vec2 wind = vec2(0.9, 0.44) * cloud2.z * cloud.w;
  vec2 q = (p.xz + wind) * (1.0 / 3200.0);
  float fields = 0.5 * CloudNoise2(q) + 0.25 * CloudNoise2(q * 2.03 + 17.1);
  fields += detail ? 0.125 * CloudNoise2(q * 4.11 + 3.7) + 0.0625 * CloudNoise2(q * 8.27 + 9.2)
                   : 0.1875 * 0.5;
  fields /= 0.9375;
  // Flat bottoms, rounded tops: cells need more cover higher up.
  float shape = smoothstep(0.0, 0.12, h) * (1.0 - smoothstep(0.35, 1.0, h));
  float d = fields - (1.0 - cloud.x) - (1.0 - shape) * 0.3;
  if (d <= 0.0) return 0.0;
  d = d / max(cloud.x, 0.05) * shape;
  if (detail) {
    vec3 r = vec3(p.x + wind.x * 1.5, p.y, p.z + wind.y * 1.5) * (1.0 / 420.0);
    float wear = 0.6 * CloudNoise(r) + 0.4 * CloudNoise(r * 2.7 + 5.3);
    d -= wear * 0.35 * (1.0 - d);
  }
  return clamp(d * 2.5, 0.0, 1.0);
}

// The layer's span along a ray (t0 >= t1: none).
void CloudSpan(vec3 o, vec3 dir, out float t0, out float t1) {
  float base = CloudSeaLevel() + cloud.y, top = base + cloud.z;
  t0 = 0.0;
  t1 = -1.0;
  if (abs(dir.y) < 1e-4) {
    if (o.y > base && o.y < top) t1 = 1e9;
    return;
  }
  float a = (base - o.y) / dir.y, b = (top - o.y) / dir.y;
  t0 = max(min(a, b), 0.0);
  t1 = max(a, b);
}

// The sun light reaching p through the clouds (0...1): four steps up the sun
// ray through the layer, without the 3D wear.
float CloudSunlight(vec3 p) {
  vec3 l = normalize(sun_dir.xyz);
  if (l.y < 0.02) return 1.0;
  float t0, t1;
  CloudSpan(p, l, t0, t1);
  if (t1 <= t0) return 1.0;
  float od = 0.0;
  for (int i = 0; i < 4; ++i) od += CloudDensity(p + l * mix(t0, t1, (float(i) + 0.5) * 0.25), false);
  return exp(-od * (t1 - t0) * 0.25 * cloud2.y);
}

// The clouds along a view ray up to max_t: rgb (in the scene's units, from
// the measured sky colour) and a = how much they hide behind them.
vec4 Clouds(vec3 dir, float max_t, float jitter) {
  float t0, t1;
  CloudSpan(camera.xyz, dir, t0, t1);
  t1 = min(min(t1, max_t), t0 + 30000.0);
  if (t1 <= t0 || t0 > 40000.0) return vec4(0.0);
  int steps = int(cloud2.x);
  float dt = (t1 - t0) / float(steps);
  const vec3 lum = vec3(0.2126, 0.7152, 0.0722);
  vec3 l = normalize(sun_dir.xyz);
  // Lit cloud white from the sky's own brightness above the horizon here.
  vec3 haze = HazeColor(dir);
  float white = dot(haze, lum);
  vec3 sun_tint = sun_color.rgb / max(dot(sun_color.rgb, lum), 1e-4);
  vec3 sky_tint = mix(vec3(1.0), ambient.rgb / max(dot(ambient.rgb, lum), 1e-4), 0.6);
  // Henyey-Greenstein, forward and a little back scattering (1 = even).
  float mu = dot(dir, l);
  // Multiple scattering (Hillaire 2016, after Wrenninge): octaves of light
  // that went further in, extinction, contribution and phase anisotropy each
  // halved, so thick clouds stay bright inside instead of turning grey.
  float phases[3];
  for (int o = 0; o < 3; ++o) {
    float sc = pow(0.5, float(o));
    float g1 = 0.6 * sc, g2 = -0.25 * sc;
    phases[o] = 0.5 * (1.0 - g1 * g1) / pow(1.0 + g1 * g1 - 2.0 * g1 * mu, 1.5) +
                0.5 * (1.0 - g2 * g2) / pow(1.0 + g2 * g2 - 2.0 * g2 * mu, 1.5);
  }
  float base = CloudSeaLevel() + cloud.y;
  vec3 light = vec3(0.0);
  float transmittance = 1.0;
  for (int i = 0; i < steps; ++i) {
    float t = t0 + (float(i) + jitter) * dt;
    vec3 p = camera.xyz + dir * t;
    // The 3D wear only where it shows.
    float d = CloudDensity(p, t < 12000.0);
    if (d <= 0.0) continue;
    float sigma = d * cloud2.y;
    // Sun light: three steps towards the sun, longer each time.
    float od = 0.0;
    for (int k = 1; k <= 3; ++k) {
      float s = 60.0 * float(k * k);
      od += CloudDensity(p + l * s, false) * 60.0 * float(2 * k - 1);
    }
    float sun_scatter = 0.0;
    for (int o = 0; o < 3; ++o) {
      float sc = pow(0.5, float(o));
      sun_scatter += sc * exp(-od * cloud2.y * sc) * phases[o];
    }
    // Thin edges facing the sun glow, thick cores scatter more in (powder).
    float powder = 1.0 - exp(-2.0 * od * cloud2.y - 0.3);
    float h = clamp((p.y - base) / cloud.z, 0.0, 1.0);
    vec3 in_light = white * (2.4 * sun_tint * sun_scatter * mix(1.0, powder, 0.5) +
                             0.85 * sky_tint * (0.6 + 0.4 * h));
    // Far away they fade into the haze at the horizon.
    in_light = mix(in_light, haze, smoothstep(4000.0, 30000.0, t) * 0.75);
    float step_t = exp(-sigma * dt);
    light += transmittance * in_light * (1.0 - step_t);
    transmittance *= step_t;
    if (transmittance < 0.01) break;
  }
  float hide = (1.0 - transmittance) * (1.0 - smoothstep(18000.0, 30000.0, t0));
  return vec4(light * (hide / max(1.0 - transmittance, 1e-4)), hide);
}

// Pass 5, one thread per quarter-resolution pixel (soft enough not to show
// it): the clouds along its view ray, however far (pass 1 leaves them out in
// front of nearer surfaces).
void MarchClouds() {
  ivec2 at = ivec2(gl_GlobalInvocationID.xy);
  ivec2 cloud_size = imageSize(cloud_image);
  if (any(greaterThanEqual(at, cloud_size))) return;
  vec2 local = vec2(at) * 4.0 + 1.5;
  vec3 dir = ViewDir(local);
  imageStore(cloud_image, at,
             Clouds(dir, 1e9, InterleavedGradientNoise(vec2(at), 0.0)));
}

// Pass 1: the clouds at a full-resolution pixel (bilinear from pass 5).
vec4 CloudsAt(ivec2 local) {
  ivec2 cloud_size = imageSize(cloud_image);
  vec2 g = (vec2(local) + 0.5) * 0.25 - 0.5;
  ivec2 g0 = ivec2(floor(g));
  vec2 f = g - vec2(g0);
  vec4 a = imageLoad(cloud_image, clamp(g0, ivec2(0), cloud_size - 1));
  vec4 b = imageLoad(cloud_image, clamp(g0 + ivec2(1, 0), ivec2(0), cloud_size - 1));
  vec4 c = imageLoad(cloud_image, clamp(g0 + ivec2(0, 1), ivec2(0), cloud_size - 1));
  vec4 d = imageLoad(cloud_image, clamp(g0 + ivec2(1, 1), ivec2(0), cloud_size - 1));
  return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// Pass 6, one thread per half-resolution pixel: ground truth ambient
// occlusion (Jimenez et al. 2016) from the saved depth. In slices through the
// view direction, the highest horizon on either side within ao.y metres, and
// the arc of sky between them, cosine weighted about the normal.
#ifdef SCENE_FX_RT
// Whether the surface at world (dist along dir from the eye) is ground for
// the surface detail: the traced terrain, or a ground mesh lying on it
// (Guadalcanal's airfield: flat, facing up, low, with the terrain at most a
// metre under it; not wings or decks, nor the first-person weapon).
bool GroundAt(vec3 world, vec3 normal, vec3 dir, float dist, float sea_level) {
  if (normal.y > 0.55) {
    // A short eye ray around the surface against the terrain only.
    rayQueryEXT q;
    float slack = 0.3 + 0.01 * dist;
    rayQueryInitializeEXT(q, world_meshes, gl_RayFlagsOpaqueEXT, 0x02, camera.xyz,
                          max(dist - slack, 0.0), dir, dist + slack);
    while (rayQueryProceedEXT(q)) {
    }
    if (rayQueryGetIntersectionTypeEXT(q, true) != gl_RayQueryCommittedIntersectionNoneEXT) return true;
  }
  if (dist > 1.5 && normal.y > 0.92 && world.y < sea_level + 8.0) {
    rayQueryEXT below;
    rayQueryInitializeEXT(below, world_meshes, gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT,
                          0x02, world + vec3(0.0, 0.2, 0.0), 0.0, vec3(0.0, -1.0, 0.0), 1.2);
    while (rayQueryProceedEXT(below)) {
    }
    return rayQueryGetIntersectionTypeEXT(below, true) != gl_RayQueryCommittedIntersectionNoneEXT;
  }
  return false;
}
#endif

void TraceOcclusion() {
  ivec2 at = ivec2(gl_GlobalInvocationID.xy);
  ivec2 ao_size = imageSize(ao_image);
  if (any(greaterThanEqual(at, ao_size))) return;
  ivec2 size = ivec2(1.0 / screen.zw + 0.5);
  ivec2 local = min(at * 2, size - 1);
  float depth = imageLoad(saved_depth, local).r;
  vec3 p = RelativeAt(local, depth);
  float dist = length(p);
  if (IsSky(depth) || !(dist > 1e-3) || dist > ao.w) {
    imageStore(ao_image, at, vec4(1.0, 0.0, 0.0, -1.0));
    return;
  }
  vec3 v = -p / dist;
  vec3 n = SurfaceNormal(local, camera.xyz + p, size);
  // Off the surface a little: the depth's own steps are no horizon.
  vec3 origin = p + n * (0.01 + 0.0015 * dist);
  // The radius in pixels here; at most a sixteenth of the screen's height.
  float pixel_angle = length(ViewDir(vec2(local) + vec2(1.0, 0.0)) - ViewDir(vec2(local)));
  float radius_px = clamp(ao.y / max(dist * pixel_angle, 1e-6), 2.0, float(size.y) / 16.0);
  float falloff = ao.y * 0.4;
  const int kSteps = 8;
  int kSlices = Temporal() ? 2 : 3;
  float noise = InterleavedGradientNoise(vec2(at), TemporalFrame());
  float jitter = fract(noise * 7.31 + 0.37);
  float visibility = 0.0;
  for (int s = 0; s < kSlices; ++s) {
    float phi = (float(s) + noise) * (3.14159265 / float(kSlices));
    vec2 omega = vec2(cos(phi), sin(phi));
    // The slice's screen direction in the world, across the view.
    vec3 d = ViewDir(vec2(local) + omega * 4.0) - ViewDir(vec2(local));
    vec3 ortho = normalize(d - v * dot(d, v));
    vec3 axis = cross(ortho, v);
    vec3 proj_n = n - axis * dot(n, axis);
    float proj_len = length(proj_n);
    if (proj_len < 1e-4) continue;
    float cos_n = clamp(dot(proj_n / proj_len, v), -1.0, 1.0);
    float n_angle = (dot(ortho, proj_n) < 0.0 ? -1.0 : 1.0) * acos(cos_n);
    // Horizon cosines on the +omega and -omega sides, from the tangent plane.
    vec2 low = vec2(cos(n_angle + 1.5707963), cos(n_angle - 1.5707963));
    vec2 horizon = low;
    for (int j = 0; j < kSteps; ++j) {
      float f = (float(j) + jitter) / float(kSteps);
      vec2 off = omega * (1.0 + f * f * (radius_px - 1.0));
      for (int side = 0; side < 2; ++side) {
        ivec2 q = ivec2(vec2(local) + 0.5 + (side == 0 ? off : -off));
        if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, size))) continue;
        float qd = imageLoad(saved_depth, q).r;
        if (IsSky(qd)) continue;
        vec3 h = RelativeAt(q, qd) - origin;
        float hl = length(h);
        if (!(hl > 1e-4)) continue;
        float w = clamp((ao.y - hl) / falloff, 0.0, 1.0);
        horizon[side] = max(horizon[side], mix(low[side], dot(h / hl, v), w));
      }
    }
    float h1 = n_angle + min(acos(clamp(horizon.x, -1.0, 1.0)) - n_angle, 1.5707963);
    float h0 = n_angle + max(-acos(clamp(horizon.y, -1.0, 1.0)) - n_angle, -1.5707963);
    float sin_n = sin(n_angle);
    visibility += proj_len * 0.25 *
                  (2.0 * cos_n + 2.0 * (h0 + h1) * sin_n - cos(2.0 * h0 - n_angle) -
                   cos(2.0 * h1 - n_angle));
  }
  visibility = clamp(visibility / float(kSlices), 0.0, 1.0);
  // Fades out towards the furthest distance.
  visibility = mix(visibility, 1.0, smoothstep(0.7 * ao.w, ao.w, dist));
  // The soft traced sun shadow here too, at half resolution: pass 1 filters
  // it with the occlusion (SunShadowAt), so the penumbra's few rays a pixel
  // come out smooth instead of dotted.
  float sun_shadow = 0.0;
  if (rt.x > 0.0 && rt.w > 0.0) sun_shadow = RayShadow(camera.xyz + p, n, dist, vec2(at));
  // In alpha: whether it is ground for pass 1's surface detail (-1: not
  // worked out, past the detail's distance).
  float ground = -1.0;
#ifdef SCENE_FX_RT
  if (rt.x > 0.0 && detail.x > 0.0 && dist < detail.y) {
    float sea_level = sea.w != 0.0 ? sea.x : camera.y - water.x;
    ground = GroundAt(camera.xyz + p, n, p / dist, dist, sea_level) ? 1.0 : 0.0;
  }
#endif
  imageStore(ao_image, at, vec4(visibility, dist, sun_shadow, ground));
}

// Water at a pixel: the sea beyond what the game drew (no depth, below the
// horizon), its surface, or what is seen through it (below the sea level).
bool WaterPixel(ivec2 local, float sea_level) {
  vec3 dir = ViewDir(vec2(local));
  if (camera.y <= sea_level || dir.y >= -1e-4) return false;
  float depth = imageLoad(saved_depth, local).r;
  if (IsSky(depth)) return true;
  vec2 uv = (vec2(local) + 0.5) * screen.zw;
  vec4 w4 = inv_view_proj * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
  if (w4.w <= 1e-6) return true;
  float y = camera.y + w4.y / w4.w;
  if (y >= sea_level + 0.5) return false;
  // What is seen through the sea, well under its surface.
  if (y < sea_level - 0.3) return true;
  // At the surface's height only water (blue or turquoise) with nothing
  // traced right under it: ground at the sea's height (Guadalcanal's
  // airfield, in runs whose measured sea level came out near it) was bent by
  // the waves, and its oil stains shimmered like water.
  vec3 col = imageLoad(scene, local + ivec2(screen.xy)).rgb;
  vec3 c = col / max(dot(col, vec3(0.2126, 0.7152, 0.0722)), 1e-4);
  bool cool = c.b > c.r * 1.05 || (c.g > c.r * 1.05 && c.b > c.r * 0.95);
  if (!cool) return false;
#ifdef SCENE_FX_RT
  if (rt.x > 0.0) {
    vec3 world = camera.xyz + w4.xyz / w4.w;
    rayQueryEXT under;
    rayQueryInitializeEXT(under, world_meshes, gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT,
                          0xFF, world + vec3(0.0, 0.15, 0.0), 0.0, vec3(0.0, -1.0, 0.0), 0.6);
    while (rayQueryProceedEXT(under)) {
    }
    if (rayQueryGetIntersectionTypeEXT(under, true) != gl_RayQueryCommittedIntersectionNoneEXT) return false;
  }
#endif
  return true;
}

// The sea's surface normal: a few travelling waves, flatter further away
// (where they would only flicker).
vec3 SeaNormal(vec3 p, float dist) {
  vec2 slope = vec2(0.0);
  const vec3 waves[4] = vec3[4](vec3(0.8, 0.6, 0.35), vec3(-0.5, 0.86, 0.55),
                                vec3(0.2, -0.98, 0.9), vec3(-0.9, -0.3, 1.6));
  for (int i = 0; i < 4; ++i) {
    vec2 d = normalize(waves[i].xy);
    float k = waves[i].z;
    float phase = dot(p.xz, d) * k + refl.z * sqrt(9.81 * k);
    slope += d * cos(phase) * (0.025 / (1.0 + float(i)));
  }
  slope *= refl.w / (1.0 + dist / 150.0);
  return normalize(vec3(-slope.x, 1.0, -slope.y));
}

// Pass 7, one thread per pixel: the scene with what is seen through the sea
// bent by its waves (only from other water pixels), for pass 1. Less far away,
// where the waves would only shimmer.
void RefractWater() {
  ivec2 local = ivec2(gl_GlobalInvocationID.xy);
  ivec2 size = ivec2(1.0 / screen.zw + 0.5);
  if (any(greaterThanEqual(local, size))) return;
  ivec2 pixel = local + ivec2(screen.xy);
  vec4 c = imageLoad(scene, pixel);
  float sea_level = sea.w != 0.0 ? sea.x : camera.y - water.x;
  if (WaterPixel(local, sea_level)) {
    vec3 dir = ViewDir(vec2(local));
    float t = (sea_level - camera.y) / dir.y;
    vec3 n = SeaNormal(camera.xyz + dir * t, t);
    // Pixels per unit of slope, for a 1440-row screen at 10 m.
    float k = wfx.y * 260.0 * float(size.y) / 1440.0 / (1.0 + t / 12.0);
    ivec2 q = clamp(local + ivec2(round(vec2(n.x, -n.z) * k)), ivec2(0), size - 1);
    if (WaterPixel(q, sea_level)) c = imageLoad(scene, q + ivec2(screen.xy));
  }
  imageStore(refract_image, local, c);
}

// Voronoi cells over a plane: distance to the nearest point and to the
// second nearest, the nearest's offset from p and its cell (for its random
// size and tint). Points jittered in their cells.
void Voronoi(vec2 p, out float d1, out float d2, out vec2 offset, out vec2 cell) {
  vec2 i = floor(p), f = p - i;
  d1 = 8.0;
  d2 = 8.0;
  offset = vec2(0.0);
  cell = i;
  for (int y = -1; y <= 1; ++y) {
    for (int x = -1; x <= 1; ++x) {
      vec2 c = i + vec2(x, y);
      vec2 point = vec2(x, y) + 0.15 + 0.7 * vec2(CloudHash(vec3(c, 1.7)), CloudHash(vec3(c, 5.3)));
      vec2 o = f - point;
      float d = length(o);
      if (d < d1) {
        d2 = d1;
        d1 = d;
        offset = o;
        cell = c;
      } else if (d < d2) {
        d2 = d;
      }
    }
  }
}

// The stone layer on bare ground: flattened domes in two sizes (pebbles in
// cells of detail.z metres, stones in cells four times that), each inside
// its own cell (one cell looked up per size: the 3x3 search made the march
// far too slow), in patches (density, from noise worked out once per pixel).
// Their height over the ground at xz (0 where there is none), and the
// surface normal and tint of the one that is highest there.
float StoneHeight(vec2 xz, float density, out vec3 n, out float tint) {
  float best = 0.0;
  n = vec3(0.0, 1.0, 0.0);
  tint = 1.0;
  for (int k = 0; k < 2; ++k) {
    float s = k == 0 ? detail.z : detail.z * 4.0;
    float seed = k == 0 ? 2.9 : 6.7;
    vec2 g = xz / s;
    vec2 cell = floor(g);
    float present = k == 0 ? 0.5 * density : 0.35 * density;
    if (CloudHash(vec3(cell, seed)) >= present) continue;
    float r = 0.22 + 0.16 * CloudHash(vec3(cell, 9.1 + seed));
    // Its centre anywhere it still fits in the cell.
    vec2 centre = r + (1.0 - 2.0 * r) * vec2(CloudHash(vec3(cell, 1.7 + seed)),
                                              CloudHash(vec3(cell, 5.3 + seed)));
    vec2 off = g - cell - centre;
    // An irregular outline: the radius varies around the stone.
    float a = atan(off.y, off.x);
    float ph = 6.2831853 * CloudHash(vec3(cell, 7.7 + seed));
    r *= 0.85 + 0.12 * sin(3.0 * a + ph) + 0.06 * sin(5.0 * a + 2.0 * ph);
    float d2 = dot(off, off);
    if (d2 >= r * r) continue;
    float hmax = r * s * (0.45 + 0.35 * CloudHash(vec3(cell, 3.3 + seed)));
    float q = sqrt(1.0 - d2 / (r * r));
    float h = hmax * q;
    if (h > best) {
      best = h;
      // The slope of hmax sqrt(1 - |o|^2 / r^2), o in cells of s metres.
      vec2 slope = hmax * off / (r * r * max(q, 0.08) * s);
      n = normalize(vec3(slope.x, 1.0, slope.y));
      tint = 0.8 + 0.4 * CloudHash(vec3(cell, 4.4 + seed));
    }
  }
  return best;
}

// Pass 1: the ambient occlusion (x) and the traced sun shadow (y) at a
// full-resolution pixel, from the 3x3 half resolution pixels around it at a
// similar distance; z: how much of them there was (0: none, compute here).
vec3 HalfResAt(ivec2 local, float dist) {
  ivec2 ao_size = imageSize(ao_image);
  vec2 g = (vec2(local) + 0.5) * 0.5 - 0.5;
  ivec2 g0 = ivec2(floor(g + 0.5));
  vec3 sum = vec3(0.0);
  for (int y = -1; y <= 1; ++y) {
    for (int x = -1; x <= 1; ++x) {
      ivec2 q = clamp(g0 + ivec2(x, y), ivec2(0), ao_size - 1);
      vec3 a = imageLoad(ao_image, q).rgb;
      if (a.g <= 0.0) continue;
      vec2 off = vec2(q) - g;
      float w = exp(-dot(off, off) * 0.5) * exp(-abs(a.g - dist) / (0.01 * dist + 0.05));
      sum += vec3(a.r, a.b, 1.0) * w;
    }
  }
  return sum.z > 1e-4 ? vec3(sum.xy / sum.z, sum.z) : vec3(1.0, 0.0, 0.0);
}
float OcclusionAt(ivec2 local, float dist) { return HalfResAt(local, dist).x; }
// Pass 1 needs it up to three times a pixel (the sun shadow, the occlusion,
// the grime): worked out once.
vec3 g_half_res;
bool g_half_res_done = false;
vec3 HalfResCached(ivec2 local, float dist) {
  if (!g_half_res_done) g_half_res = HalfResAt(local, dist), g_half_res_done = true;
  return g_half_res;
}

#ifdef SCENE_FX_RT
// Pass 4, one thread per half-resolution pixel: rays over the surface's
// hemisphere (cosine weighted) see sky, or the meshes in the way and the
// light they give back. The ratio of that light to the open sky's scales the
// game's ambient share in pass 1; it is averaged over frames, following the
// surface through last frame's view.
void TraceLight() {
  ivec2 at = ivec2(gl_GlobalInvocationID.xy);
  ivec2 light_size = imageSize(light_out);
  if (any(greaterThanEqual(at, light_size))) return;
  ivec2 size = ivec2(1.0 / screen.zw + 0.5);
  ivec2 local = min(at * 2, size - 1);
  float depth = imageLoad(saved_depth, local).r;
  vec2 uv = (vec2(local) + 0.5) * screen.zw;
  vec4 w4 = inv_view_proj * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
  if (IsSky(depth) || w4.w <= 1e-6) {
    imageStore(light_out, at, vec4(1.0, 1.0, 1.0, 0.0));
    imageStore(light_out_dist, at, vec4(0.0));
    return;
  }
  vec3 world = camera.xyz + w4.xyz / w4.w;
  float dist = length(w4.xyz / w4.w);
  vec3 normal = SurfaceNormal(local, world, size);
  vec3 tangent = normalize(abs(normal.y) < 0.9 ? cross(normal, vec3(0.0, 1.0, 0.0))
                                               : cross(normal, vec3(1.0, 0.0, 0.0)));
  vec3 bitangent = cross(normal, tangent);
  vec3 origin = world + normal * (0.05 + 0.002 * dist);
  // Last frame's light at the same surface.
  float frames = 0.0;
  vec3 history = vec3(1.0);
  if (prev_camera.w > 0.0) {
    vec4 pc = prev_view_proj * vec4(world, 1.0);
    if (pc.w > 1e-6) {
      vec2 ndc = pc.xy / pc.w;
      ivec2 prev = ivec2(vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * vec2(light_size));
      if (all(greaterThanEqual(prev, ivec2(0))) && all(lessThan(prev, light_size))) {
        float prev_dist = imageLoad(light_history_dist, prev).r;
        float expected = length(world - prev_camera.xyz);
        if (prev_dist > 0.0 && abs(prev_dist - expected) < 0.03 * expected + 0.15) {
          vec4 h = imageLoad(light_history, prev);
          if (!any(isnan(h))) {
            history = h.rgb;
            frames = min(h.a, gi2.y - 1.0);
          }
        }
      }
    }
  }
  // Half the pixels trace each frame (a checkerboard turning over every
  // frame); the others keep what they had, once they have some history.
  if (frames >= 2.0 && ((at.x + at.y + int(gi.w)) & 1) == 1) {
    imageStore(light_out, at, vec4(history, frames));
    imageStore(light_out_dist, at, vec4(dist));
    return;
  }
  vec3 seen = vec3(0.0), open = vec3(0.0);
  int rays = int(gi.y);
  for (int i = 0; i < rays; ++i) {
    uvec3 seed = uvec3(at.x | (at.y << 16), uint(gi.w), uint(i));
    float u1 = Random(seed), u2 = Random(seed + uvec3(0u, 0u, 977u));
    float r = sqrt(u1), phi = 6.2831853 * u2;
    vec3 dir = normalize(tangent * (r * cos(phi)) + bitangent * (r * sin(phi)) +
                         normal * sqrt(max(1.0 - u1, 0.0)));
    vec3 sky = SkyColor(dir);
    open += sky;
    vec3 hit_normal;
    float t = TraceMeshes(origin, dir, gi2.x, hit_normal);
    bool known;
    if (t < 0.0) {
      seen += sky;
    } else {
      // A cell's colour is that of its open, lit part; close by, the ray
      // meets a corner or an underside that gets less light than that.
      float contact = gi2.w > 0.0 ? mix(0.25, 1.0, smoothstep(0.0, gi2.w, t)) : 1.0;
      seen += HitColor(origin + dir * t, hit_normal, known) * contact;
    }
  }
  vec3 ratio = clamp(seen / max(open, vec3(1e-5)), vec3(0.0), vec3(2.5));
  if (frames <= 0.0) history = ratio;
  imageStore(light_out, at, vec4(mix(history, ratio, 1.0 / (frames + 1.0)), frames + 1.0));
  imageStore(light_out_dist, at, vec4(dist));
}

// Pass 1: the ray-traced light at a full-resolution pixel, from the 3x3 half
// resolution pixels around it at a similar distance and on the pixel's own
// plane (normal): where a post meets the ground both are at one distance, and
// the ground's light on the post (and the post's on the ground) was a blotch.
vec3 LightAt(ivec2 local, float dist, vec3 normal) {
  vec3 here = ViewDir(vec2(local)) * dist;
  float plane_tolerance = 0.02 * dist + 0.04;
  ivec2 light_size = imageSize(light_out);
  vec2 g = (vec2(local) + 0.5) * 0.5 - 0.5;
  ivec2 g0 = ivec2(floor(g + 0.5));
  vec4 sum = vec4(0.0);
  // Without the distance weight too: where no neighbour is at the pixel's
  // distance (bumpy ground) it had no light at all, and such pixels stayed
  // bright specks in the darker light around them.
  vec4 near = vec4(0.0);
  for (int y = -1; y <= 1; ++y) {
    for (int x = -1; x <= 1; ++x) {
      ivec2 q = clamp(g0 + ivec2(x, y), ivec2(0), light_size - 1);
      float qd = imageLoad(light_out_dist, q).r;
      if (qd <= 0.0) continue;
      vec2 off = vec2(q) - g;
      vec4 l = imageLoad(light_out, q);
      // Pixels with few frames yet count less; ones off this pixel's plane
      // hardly at all.
      vec3 there = ViewDir(vec2(q) * 2.0 + 0.5) * qd;
      float plane = abs(dot(normal, there - here)) / plane_tolerance;
      float w = exp(-dot(off, off) * 0.35) * min(l.a, 8.0) * exp(-plane * plane);
      near += vec4(l.rgb, 1.0) * w;
      w *= exp(-abs(qd - dist) / (0.01 * dist + 0.05));
      sum += vec4(l.rgb, 1.0) * w;
    }
  }
  if (sum.w > 1e-3) return sum.rgb / sum.w;
  return near.w > 1e-4 ? near.rgb / near.w : vec3(1.0);
}


// The material of the surface drawn at a pixel dist from the camera; false
// if it has none (not a --pbr shader, or something else in front).
bool MaterialAt(ivec2 p, float dist, out float rough, out float metal) {
  rough = 1.0;
  metal = 0.0;
  if (uint(p.x) >= kMaterialPitch || uint(p.y) >= kMaterialRows) return false;
  uint m = materials[uint(p.y) * kMaterialPitch + uint(p.x)];
  if (m == 0xFFFFFFFFu) return false;
  float d = uintBitsToFloat(m & 0xFFFFFF00u);
  if (abs(d - dist) > 0.03 * dist + 0.2) return false;
  rough = float((m >> 4) & 15u) / 15.0;
  metal = float(m & 15u) / 15.0;
  return true;
}

// What a mirror at p with normal n shows of the world's meshes: how much of
// the reflected view one covers (0 for open sky, less far away, through the
// fog), its colour (the colour cache) and the sky it hides.
float ReflectionHit(vec3 p, vec3 n, vec3 dir, float dist, bool sea_surface,
                    out vec3 hit_color, out vec3 sky_color) {
  vec3 r = reflect(dir, n);
  // Waves never reflect into the water: at most half as low as the flat sea.
  if (sea_surface) {
    vec3 flat_r = reflect(dir, vec3(0.0, 1.0, 0.0));
    r = normalize(vec3(r.x, max(r.y, 0.5 * flat_r.y), r.z));
  }
  sky_color = SkyColor(r);
  hit_color = sky_color;
  vec3 hit_normal;
  vec3 origin = p + n * (0.05 + 0.002 * dist);
  float t = TraceMeshes(origin, r, rt.y, hit_normal);
  if (t < 0.0) return 0.0;
  bool known;
  hit_color = HitColor(origin + r * t, hit_normal, known);
  return exp(-sun_dir.w * t);
}
#endif

float GoreHash(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }

float GoreNoise(vec2 p) {
  vec2 i = floor(p);
  vec2 f = p - i;
  vec2 u = f * f * (3.0 - 2.0 * f);
  float a = GoreHash(i), b = GoreHash(i + vec2(1.0, 0.0));
  float c = GoreHash(i + vec2(0.0, 1.0)), d = GoreHash(i + vec2(1.0, 1.0));
  return a + (b - a) * u.x + (c - a) * u.y + (a - b - c + d) * u.x * u.y;
}

// Blood (gpu/vulkan/bf_gore.cc): splats and chunks lie on the surfaces under
// them (the surface's colour tinted, so its light and shadow stay); puffs,
// drops and flying chunks are sprites in the air, lit by the sun and the sky,
// and hidden behind nearer surfaces.
vec3 ApplyGore(vec3 color, vec3 world, bool surface, vec3 dir, float dist) {
  int count = int(gore_info.x);
  if (count == 0) return color;
  vec3 right = normalize(cross(dir, vec3(0.0, 1.0, 0.0)));
  vec3 up = cross(right, dir);
  for (int i = 0; i < count; ++i) {
    vec4 a = gore[i * 2];
    vec4 b = gore[i * 2 + 1];
    float kind = b.x;
    vec2 sp = vec2(b.y * 7.31, b.y * 3.17);
    if (kind == 6.0) {
      // Explosion smoke: a puff ray-marched as a volume. Noise inside a soft
      // sphere that grows and thins, lit by the sun (through itself, darker
      // underneath, and the shadow map) and the sky; cut off by the surface
      // behind, so it sits in the scene without hard edges.
      float t = b.z;
      float R = a.w * (1.0 + 1.6 * sqrt(t));
      vec3 oc = camera.xyz - a.xyz;
      float bq = dot(oc, dir), h = bq * bq - (dot(oc, oc) - R * R);
      if (h <= 0.0) continue;
      h = sqrt(h);
      float t0 = max(-bq - h, 0.05), t1 = -bq + h;
      if (surface) t1 = min(t1, dist);
      if (t1 <= t0) continue;
      float alive = smoothstep(0.0, 0.03, t) * (1.0 - smoothstep(0.55, 1.0, t));
      float dens0 = 1.6 / R * alive * (1.0 - 0.55 * t);
      vec3 l = normalize(sun_dir.xyz);
      int slice;
      float sun_vis = SunVisibility(a.xyz, slice);
      float mu = dot(dir, l);
      float phase = 0.55 + 0.45 * (1.0 - 0.16) / pow(1.0 + 0.16 - 0.8 * mu, 1.5);
      vec3 albedo = mix(vec3(0.42, 0.40, 0.38), vec3(0.62, 0.58, 0.52), t);  // darker when fresh
      vec3 seed3 = vec3(b.y * 1.37, b.y * 2.11, b.y * 0.73);
      const int kSteps = 8;
      float step_t = (t1 - t0) / float(kSteps);
      float jitter = CloudHash(dir * 1731.0 + seed3);
      float T = 1.0;
      vec3 L = vec3(0.0);
      for (int k = 0; k < kSteps; ++k) {
        vec3 p = camera.xyz + dir * (t0 + (float(k) + jitter) * step_t);
        vec3 q = (p - a.xyz) / R;
        float r = length(q);
        vec3 np = p * (0.9 / max(a.w, 0.5)) + seed3 + vec3(0.0, -t * 3.0, 0.0);
        float n = CloudNoise(np) * 0.65 + CloudNoise(np * 2.3) * 0.35;
        float d = dens0 * (1.0 - smoothstep(0.35, 1.0, r)) * clamp(n * 1.6 - 0.25, 0.0, 1.5);
        if (d <= 0.0) continue;
        // Through itself towards the sun: thicker from the far side.
        float sun_t = exp(-dens0 * R * 1.1 * clamp(1.0 - dot(q, l), 0.0, 2.0));
        vec3 lit = albedo * (ambient.rgb * (0.75 + 0.35 * q.y) + sun_color.rgb * sun_t * phase * sun_vis) *
                   camera.w * gore_info.y;
        float a_step = 1.0 - exp(-d * step_t);
        L += T * lit * a_step;
        T *= 1.0 - a_step;
        if (T < 0.02) break;
      }
      color = color * T + L;
      continue;
    }
    float t = b.z;
    float cover = 0.0;
    vec3 blood;
    if (kind == 2.0 || kind == 4.0) {
      if (!surface) continue;
      vec3 d = world - a.xyz;
      if (abs(d.y) > 0.35) continue;
      vec2 q = d.xz / a.w;
      float r = length(q);
      if (r > 1.6) continue;
      if (kind == 2.0) {
        // A pool that spreads (b.w) with drops around it.
        float edge = (0.35 + 0.65 * b.w) * (0.65 + 0.5 * GoreNoise(q * 2.0 + sp));
        float blobs = step(0.72, GoreNoise(q * 4.0 + sp + 3.0)) * step(r, edge * 1.5);
        cover = max(1.0 - smoothstep(edge - 0.08, edge, r), blobs) * 0.92;
        blood = color * vec3(1.7, 0.12, 0.1);
      } else {
        // A lump of meat: dark, wet and lighter in the middle.
        float edge = 0.75 + 0.35 * GoreNoise(q * 2.5 + sp);
        cover = 1.0 - smoothstep(edge - 0.1, edge, r);
        float inner = 1.0 - smoothstep(0.0, 0.7, r);
        blood = color * (vec3(0.6, 0.04, 0.04) + vec3(1.6, 0.3, 0.25) * inner);
      }
    } else {
      vec3 to = a.xyz - camera.xyz;
      float along = dot(to, dir);
      // Behind the camera, or behind a nearer surface.
      if (along <= 0.05 || along > dist + a.w * 0.5) continue;
      vec3 off = camera.xyz + dir * along - a.xyz;
      vec2 q = vec2(dot(off, right), dot(off, up)) / a.w;
      float r = length(q);
      if (r > 2.5) continue;
      vec2 qdir = q / max(r, 1e-4);
      if (kind == 0.0) {
        // A spiky burst that spreads and thins, droplets around it.
        float grow = 0.45 + 0.75 * sqrt(t);
        float edge = grow * (0.5 + 0.75 * GoreNoise(qdir * 3.0 + sp));
        float mist = 1.0 - smoothstep(edge * 0.55, edge, r);
        float drops = step(0.75, GoreNoise(q * 5.0 + sp)) * step(grow * 0.4, r) * step(r, grow * 1.7);
        cover = max(mist * 0.85, drops) * (1.0 - t) * (1.0 - t);
        blood = vec3(0.5, 0.02, 0.03) * (0.75 + 0.25 * mist);
      } else if (kind == 5.0 || kind == 6.0) {
        // Gun smoke (grey) and an explosion's dust (sandy): a soft, noisy
        // puff that grows and thins out.
        float grow = kind == 5.0 ? 0.6 + 1.6 * sqrt(t) : 0.7 + 1.1 * sqrt(t);
        float wisp = GoreNoise(q * 1.7 / grow + sp + vec2(t * 0.6));
        float edge = grow * (0.6 + 0.5 * wisp);
        float dens = 1.0 - smoothstep(edge * 0.25, edge, r);
        cover = dens * (kind == 5.0 ? 0.5 : 0.8) * (1.0 - t) * smoothstep(0.0, 0.04, t);
        blood = kind == 5.0 ? vec3(1.5, 1.5, 1.55) : vec3(1.75, 1.55, 1.25);
      } else if (kind == 7.0) {
        // Debris: a small dark lump of earth or stone.
        float edge = 0.75 + 0.3 * GoreNoise(q * 2.5 + sp);
        cover = (1.0 - smoothstep(edge - 0.15, edge, r)) * (1.0 - smoothstep(0.9, 1.0, t));
        blood = vec3(0.6, 0.55, 0.5);
      } else if (kind == 1.0) {
        float edge = 0.8 + 0.35 * GoreNoise(q * 2.5 + sp);
        cover = (1.0 - smoothstep(edge - 0.15, edge, r)) * (1.0 - smoothstep(0.9, 1.0, t));
        float inner = 1.0 - smoothstep(0.0, 0.7, r);
        blood = vec3(0.16, 0.0, 0.01) + vec3(0.36, 0.05, 0.05) * inner;
      } else {
        cover = (1.0 - smoothstep(0.6, 1.0, r)) * (1.0 - smoothstep(0.8, 1.0, t));
        blood = vec3(0.45, 0.02, 0.03);
      }
      // Lit like a rough ball: the sun on its upper side.
      float n_dot_l = clamp(dot(normalize(vec3(qdir.x * right + qdir.y * up) - dir), normalize(sun_dir.xyz)), 0.0, 1.0);
      blood *= (ambient.rgb + sun_color.rgb * n_dot_l) * camera.w * gore_info.y;
    }
    color = mix(color, blood, clamp(cover, 0.0, 1.0));
  }
  return color;
}

// Single scattering in the height fog along the view ray, len metres (the
// surface's distance, at most march.x): in-scattered light (times the haze
// colour) and optical depth. The haze converges to the measured horizon sky
// colour; where the shadow map says the air is in shadow it loses the
// sunlit share (the light shafts).
void HazeMarch(vec3 dir, float len, bool sky, float jitter, out float scattered, out float optical) {
  int steps = int(march.y);
  float dt = len / float(steps);
  // The densest fog is at the (measured) sea level.
  float fog_base = (sea.w != 0.0 ? sea.x : camera.y - water.x) + ambient.w;
  // The game's sky already has its haze: thinner fog in front of it.
  float density_scale = sky ? water.z : 1.0;
  scattered = 0.0;
  optical = 0.0;
  vec3 base[3], delta[3];
  for (int i = 0; i < 3; ++i) {
    base[i] = vec3(dot(slices[i * 3], vec4(camera.xyz, 1.0)), dot(slices[i * 3 + 1], vec4(camera.xyz, 1.0)),
                   dot(slices[i * 3 + 2], vec4(camera.xyz, 1.0)));
    delta[i] = vec3(dot(slices[i * 3].xyz, dir), dot(slices[i * 3 + 1].xyz, dir),
                    dot(slices[i * 3 + 2].xyz, dir));
  }
  for (int i = 0; i < steps; ++i) {
    float t = (float(i) + jitter) * dt;
    float y = camera.y + dir.y * t;
    float density = density_scale * sun_dir.w * exp(-sun_color.w * max(y - fog_base, 0.0));
    float vis = SunVisibilityAlong(t, base, delta);
    float step_optical = density * dt;
    scattered += exp(-(optical + 0.5 * step_optical)) * step_optical *
                 (1.0 - strength.x * (1.0 - vis));
    optical += step_optical;
  }
  if (!sky) {
    // Surfaces never get more haze than the sky in the same direction, so
    // they fade into it with distance and the horizon has no seam. (Thinning
    // the whole ray by the surface's distance instead left the sea from
    // 300 m out less hazy than nearer: a darker band in the distance.)
    const int kFull = 8;
    float full_dt = march.x / float(kFull);
    float full = 0.0;
    for (int i = 0; i < kFull; ++i) {
      float y = camera.y + dir.y * (float(i) + 0.5) * full_dt;
      full += sun_dir.w * exp(-sun_color.w * max(y - fog_base, 0.0)) * full_dt;
    }
    float cap = water.z * full;
    if (optical > cap) {
      scattered *= (1.0 - exp(-cap)) / max(1.0 - exp(-optical), 1e-6);
      optical = cap;
    }
  }
}

// Pass 8, one thread per half-resolution pixel: the haze along its view ray
// (the march is the costliest part of pass 1 at full resolution, and the
// haze and its light shafts change slowly across the screen).
void MarchHazeHalf() {
  ivec2 at = ivec2(gl_GlobalInvocationID.xy);
  ivec2 half_size = imageSize(haze_image);
  if (any(greaterThanEqual(at, half_size))) return;
  ivec2 size = ivec2(1.0 / screen.zw + 0.5);
  ivec2 local = min(at * 2, size - 1);
  float depth = imageLoad(saved_depth, local).r;
  vec2 uv = (vec2(local) + 0.5) * screen.zw;
  vec4 world4 = inv_view_proj * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
  vec3 dir = ViewDir(vec2(local));
  bool sky = IsSky(depth) || world4.w <= 1e-6;
  float dist = sky ? 1e9 : length(world4.xyz / world4.w);
  if (sky && dir.y < -1e-4) {
    float sea_level = sea.w != 0.0 ? sea.x : camera.y - water.x;
    dist = (sea_level - camera.y) / dir.y;
    sky = false;
  }
  float scattered, optical;
  HazeMarch(dir, min(dist, march.x), sky, InterleavedGradientNoise(vec2(at), march.w), scattered, optical);
  imageStore(haze_image, at, vec4(scattered, optical, sky ? -1.0 : dist, 0.0));
}

// Pass 1: the haze at a full-resolution pixel from the 3x3 half-resolution
// pixels around it at a similar distance (or also sky); false if none is.
bool HazeAt(ivec2 local, float dist, bool sky, out float scattered, out float optical) {
  scattered = 0.0;
  optical = 0.0;
  if (march.z <= 0.0) return false;
  ivec2 half_size = imageSize(haze_image);
  vec2 g = (vec2(local) + 0.5) * 0.5 - 0.5;
  ivec2 g0 = ivec2(floor(g + 0.5));
  vec3 sum = vec3(0.0);
  for (int y = -1; y <= 1; ++y) {
    for (int x = -1; x <= 1; ++x) {
      ivec2 q = clamp(g0 + ivec2(x, y), ivec2(0), half_size - 1);
      vec4 h = imageLoad(haze_image, q);
      bool q_sky = h.z < 0.0;
      if (q_sky != sky) continue;
      vec2 off = vec2(q) - g;
      float w = exp(-dot(off, off) * 0.6);
      if (!sky) w *= exp(-abs(h.z - dist) / (0.02 * dist + 0.1));
      sum += vec3(h.x, h.y, 1.0) * w;
    }
  }
  if (sum.z < 0.05) return false;
  scattered = sum.x / sum.z;
  optical = sum.y / sum.z;
  return true;
}

// Pass 9, one thread per pixel: FSR 3's motion vectors from the saved depth
// and last frame's view: the world position (or, for the sky, the direction)
// projected both ways.
void MotionVectors() {
  ivec2 local = ivec2(gl_GlobalInvocationID.xy);
  ivec2 size = imageSize(motion_image);
  if (any(greaterThanEqual(local, size))) return;
  float depth = imageLoad(saved_depth, local).r;
  vec2 uv = (vec2(local) + 0.5) / vec2(size);
  vec4 ndc = vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
  vec4 w4 = inv_view_proj * ndc;
  vec4 prev;
  if (IsSky(depth) || w4.w <= 1e-6) {
    // Far: only the rotation counts (a direction, w 0).
    vec3 dir = ViewDir(vec2(local));
    prev = prev_view_proj * vec4(dir, 0.0);
  } else {
    prev = prev_view_proj * vec4(camera.xyz + w4.xyz / w4.w, 1.0);
  }
  vec2 mv = vec2(0.0);
  if (prev.w > 1e-6) {
    vec2 prev_ndc = prev.xy / prev.w;
    vec2 prev_uv = vec2(prev_ndc.x * 0.5 + 0.5, 0.5 - prev_ndc.y * 0.5);
    mv = (prev_uv - uv) * vec2(size);
  }
  imageStore(motion_image, local, vec4(mv, 0.0, 0.0));
}

void main() {
  if (pass_mode == 9u) {
    MotionVectors();
    return;
  }
  if (pass_mode == 8u) {
    MarchHazeHalf();
    return;
  }
  if (pass_mode == 2u) {
    MeasureHorizon();
    return;
  }
  if (pass_mode == 3u) {
    ScanColumn();
    return;
  }
  if (pass_mode == 5u) {
    MarchClouds();
    return;
  }
  if (pass_mode == 6u) {
    TraceOcclusion();
    return;
  }
  if (pass_mode == 7u) {
    RefractWater();
    return;
  }
#ifdef SCENE_FX_RT
  if (pass_mode == 4u) {
    TraceLight();
    return;
  }
#endif
  uvec2 pixel = gl_GlobalInvocationID.xy + uvec2(screen.xy);
  if (any(greaterThanEqual(gl_GlobalInvocationID.xy, uvec2(1.0 / screen.zw + 0.5)))) {
    return;
  }
  ivec2 local = ivec2(gl_GlobalInvocationID.xy);
  if (pass_mode == 0u) {
    imageStore(saved_depth, local, vec4(texelFetch(scene_depth, ivec2(pixel), 0).r));
    imageStore(world_color, local, imageLoad(scene, ivec2(pixel)));
#ifdef SCENE_FX_RT
    if (pixel.x < kMaterialPitch && pixel.y < kMaterialRows) {
      float d = texelFetch(scene_depth, ivec2(pixel), 0).r;
      vec2 duv = (vec2(local) + 0.5) * screen.zw;
      vec4 w4 = inv_view_proj * vec4(duv.x * 2.0 - 1.0, 1.0 - duv.y * 2.0, d, 1.0);
      // Clip w is 1 / w4.w; sky (or nothing): far.
      bool far_away = IsSky(d) || w4.w <= 1e-6;
      float w = far_away ? 1e6 : 1.0 / w4.w;
      // In the low 8 bits: how much sun reaches the surface (last frame's
      // half-resolution traced shadow), for the particles near it.
      float lit = 1.0;
      if (!far_away && rt.x > 0.0 && rt.w > 0.0) {
        // The nearest half-resolution value (the particles only need it roughly).
        vec4 hr = imageLoad(ao_image, clamp(local / 2, ivec2(0), imageSize(ao_image) - 1));
        if (hr.g > 0.0) lit = 1.0 - hr.b;
      }
      materials[kMaterialDepthOffset + pixel.y * kMaterialPitch + pixel.x] =
          (floatBitsToUint(w) & 0xFFFFFF00u) | uint(clamp(lit, 0.0, 1.0) * 255.0 + 0.5);
    }
#endif
    return;
  }
  // Pixels with depth now were drawn after the world's depth was copied out:
  // the first-person weapon.
  if (texelFetch(scene_depth, ivec2(pixel), 0).r > 0.0 && shadow_info.w == 0u) {
    return;
  }
  vec4 color = wfx.y > 0.0 ? imageLoad(refract_image, local) : imageLoad(scene, ivec2(pixel));
  float depth = imageLoad(saved_depth, local).r;

  vec2 uv = (vec2(local) + 0.5) * screen.zw;
  vec4 world4 = inv_view_proj * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
  vec3 dir = ViewDir(vec2(local));
  // Distant geometry (the far sea) has a depth of almost 0, at w ~ 0: treat
  // it as infinitely far rather than dividing by it.
  bool sky = IsSky(depth) || world4.w <= 1e-6;
  float dist = sky ? 1e9 : length(world4.xyz / world4.w);
  if (sky && dir.y < -1e-4) {
    // The sea beyond what the game drew: the plane at sea level.
    float sea_level = sea.w != 0.0 ? sea.x : camera.y - water.x;
    dist = (sea_level - camera.y) / dir.y;
    sky = false;
  }
  vec3 world = camera.xyz + dir * min(dist, 1e5);
  float len = min(dist, march.x);
  // What the game drew over the world after the depth copy (its particles:
  // impact dust, sparks, smoke) is taken off here and put back unchanged at
  // the end, so shadows, occlusion and surface detail light the ground under
  // it, not it (it came out as black squares and big bright bursts). Not
  // the sea and what is seen through it (drawn after the copy too).
  vec3 overlay = vec3(0.0);
  if (!sky && depth > 0.0 &&
      world.y > (sea.w != 0.0 ? sea.x : camera.y - water.x) + 0.5) {
    vec3 before = imageLoad(world_color, local).rgb;
    overlay = color.rgb - before;
    color.rgb = before;
  }

  uint debug = shadow_info.w;
  // Debug 17: everything as usual, but the left half without the traced light.
  bool gi_split = debug == 17u;
  if (gi_split) debug = 0u;
  ivec2 size = ivec2(1.0 / screen.zw + 0.5);
  // A surface with depth (not the sea beyond what the game drew: a guessed
  // plane, no depth).
  bool has_depth = !sky && !IsSky(depth);
  // The surface normal from the neighbouring depths; the sea plane faces up.
  vec3 normal = has_depth ? SurfaceNormal(local, world, size) : vec3(0.0, 1.0, 0.0);
  float n_dot_l = dot(normal, normalize(sun_dir.xyz));
  int slice;
  float game_lit = has_depth ? SunVisibility(world, slice) : 1.0;
  // All sun shadows drawn here (the game's own test always passes): the
  // ray-traced meshes and terrain, and the game's map for what only it has.
  bool own = gi2.z > 0.0;
  float sea_level = sea.w != 0.0 ? sea.x : camera.y - water.x;
  // Far away the depth is coarse: a looser height and no normal test.
  // Within 0.3 m of the measured sea level close by (the sea is drawn flat at
  // it), looser further out where the depth is coarse: at 0.75 m Guadalcanal's
  // airfield, just over the sea, passed for it in runs where its shaded
  // ground looked blue enough (waves and foam on dry land).
  bool on_sea = has_depth && abs(world.y - sea_level) < min(0.3 + 0.005 * dist, 0.75 + 0.005 * dist) &&
                (normal.y > 0.9 || dist > 300.0);
  bool ground_at_sea = false;
#ifdef SCENE_FX_RT
  // Never a traced mesh (the sea isn't one): a deck at a wrong sea level.
  if (on_sea && rt.x > 0.0) {
    vec3 n;
    float t = TraceMeshes(camera.xyz, dir, dist * 1.02 + 2.0, n);
    if (t > 0.0 && abs(t - dist) < 1.0 + 0.01 * dist) on_sea = false, ground_at_sea = true;
  }
  // Nor bare ground at the sea's height (Guadalcanal's airfield took the
  // sea's reflections; ground_at_sea then: the surface detail takes it as
  // bare ground, though it is not the traced terrain).
  // sea's reflections): the terrain right at the pixel, not blue or
  // turquoise (water drawn over the terrain in the shallows stays sea).
  if (on_sea && rt.x > 0.0 && dist < 400.0) {
    rayQueryEXT q;
    float slack = 0.15 + 0.004 * dist;
    rayQueryInitializeEXT(q, world_meshes, gl_RayFlagsOpaqueEXT, 0x02, camera.xyz,
                          max(dist - slack, 0.0), dir, dist + slack);
    while (rayQueryProceedEXT(q)) {
    }
    vec3 c = color.rgb / max(dot(color.rgb, vec3(0.2126, 0.7152, 0.0722)), 1e-4);
    bool cool = c.b > c.r * 1.05 || (c.g > c.r * 1.05 && c.b > c.r * 0.95);
    // Water drawn with depth is blue or turquoise; anything else at the sea's
    // height is ground or a deck (some ground, as that airfield, isn't in the
    // traced terrain; grey shadowed dirt took the sea's reflections).
    if (!cool) on_sea = false, ground_at_sea = true;
  }
  // Nor ground meshes at the sea's height (the airfield): the eye ray's
  // checks missed them at grazing angles, and dark
  // oil stains on them passed for water (waves and foam in specks, in the
  // runs whose measured sea level came out near the airfield's height).
  if (on_sea && rt.x > 0.0 && dist < 400.0 && normal.y > 0.9) {
    rayQueryEXT under;
    // Any traced mesh (the airfield's own, or the terrain) right under it.
    rayQueryInitializeEXT(under, world_meshes, gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT,
                          0xFF, world + vec3(0.0, 0.15, 0.0), 0.0, vec3(0.0, -1.0, 0.0), 0.6);
    while (rayQueryProceedEXT(under)) {
    }
    if (rayQueryGetIntersectionTypeEXT(under, true) != gl_RayQueryCommittedIntersectionNoneEXT) {
      on_sea = false, ground_at_sea = true;
    }
  }
#endif
  // The game's shading already darkens where its shadow map says shadow
  // (casters off screen included): add only where it says lit. strength.y
  // darkens the map's shadows again (debugging).
  float occluded = on_sea || own ? 0.0 : (1.0 - game_lit) * strength.y;
  float map_only = 0.0;
  if ((ss.x > 0.0 || own) && has_depth && (debug == 0u || debug == 8u || debug == 9u)) {
    // Not on the drawn sea: water shows reflections, not shadows, and far out
    // the ray lands behind hulls and leaves dark streaks.
    float traced = 0.0;
    // Surfaces nearly edge-on to the sun fade in: the normal from the depth
    // flips around zero there (blocks of shadow on a ship's side), and the
    // sun's light, as n.l, is little there anyway.
    float graze = smoothstep(0.02, 0.12, n_dot_l);
    if (n_dot_l > 0.02 && (own || game_lit > 0.5) && !on_sea) {
      // A fixed per-pixel pattern (no frame term): steady, no shimmer. Only
      // without ray tracing: unfiltered, its pattern showed as dotted, grainy
      // edges, and the traced shadows plus the game's own cover it.
      if (ss.x > 0.0 && rt.x <= 0.0) {
        occluded = max(occluded, ss.x * graze * ScreenSpaceShadow(world, normal, dist, size,
                                            InterleavedGradientNoise(vec2(pixel), TemporalFrame())));
      }
      if (rt.x > 0.0) {
        // Soft, filtered at half resolution with the occlusion (within its
        // distance); further out, or without it, one ray per pixel here.
        vec3 half_res = ao.x > 0.0 && rt.w > 0.0 && dist < 0.95 * ao.w ? HalfResCached(local, dist)
                                                                       : vec3(0.0);
        // Far away (past the occlusion's distance) one hard ray: a penumbra
        // there is under a pixel wide, and 3-10 rays a pixel over the distant
        // ground was a good part of the frame.
        // From 85 % to 95 % of that distance the filtered shadow fades into
        // the hard ray, so there is no line where one takes over.
        if (half_res.z > 0.05) {
          traced = half_res.y;
          float far = smoothstep(0.85 * ao.w, 0.95 * ao.w, dist);
          if (far > 0.0) traced = mix(traced, RayShadowHard(world, normal, dist), far);
        } else {
          traced = dist > 0.9 * ao.w ? RayShadowHard(world, normal, dist) : RayShadow(world, normal, dist, vec2(pixel));
        }
        occluded = max(occluded, traced * graze);
      }
      if (own && traced == 0.0) {
        float t_occ, texel_m;
        if (SunVisibilityAt(world, t_occ, texel_m) < 0.5 &&
            (rt.x <= 0.0 ||
             !OccluderTraced(world + normalize(sun_dir.xyz) * t_occ, 3.0 * texel_m + 0.15))) {
          map_only = 1.0;
          // Filtered: the map's texels showed as steps along the edges.
          occluded = max(occluded, graze * (1.0 - SunVisibilitySoft(world)));
        }
      }
    }
    if (debug == 9u) {
      // Red: the traced ray hits a mesh; grey: in the game's map shadow;
      // blue: the sea; dark: facing away from the sun.
      vec3 v = n_dot_l <= 0.02 ? vec3(0.25) : game_lit <= 0.5 ? vec3(0.5) : vec3(0.95);
      if (traced > 0.0) v = vec3(1.0, 0.15, 0.1);
      if (map_only > 0.0) v = vec3(0.1, 0.8, 0.2);
      if (on_sea) v = vec3(0.1, 0.2, 0.6);
      // Top left: one bar per thousand instances.
      if (local.y < 12 && local.x < int(rt.z / 1000.0 * 20.0)) v = vec3(0.0, 1.0, 0.0);
      imageStore(scene, ivec2(pixel), vec4(v, color.a));
      return;
    }
    if (debug == 8u) {
      vec3 v = vec3(0.15) + vec3(0.85) * (1.0 - occluded) * vec3(1.0, game_lit, n_dot_l > 0.02 ? 1.0 : 0.0);
      if (on_sea) v = vec3(0.1, 0.2, 0.6);
      if (OnSunRing(local, size)) v = vec3(1.0, 0.0, 1.0);
      imageStore(scene, ivec2(pixel), vec4(v, color.a));
      return;
    }
    // Lit = albedo * (sun * N.L + ambient), shadowed = albedo * ambient.
    const vec3 lum = vec3(0.2126, 0.7152, 0.0722);
    float sun_l = dot(sun_color.rgb, lum) * n_dot_l;
    float amb_l = dot(ambient.rgb, lum);
    float shadowed = clamp(amb_l / max(sun_l + amb_l, 1e-4), 0.2, 1.0);
    // shadows=2 (testing): the game's off and none drawn here.
    // Own shadows: faded out where the sun only grazes (the normal from the
    // depth is too coarse there to tell how much sun the game gave).
    float own_amount = gi2.z > 1.5 ? 0.0 : occluded * smoothstep(0.05, 0.25, n_dot_l);
    color.rgb *= mix(1.0, shadowed, own ? own_amount : occluded * ss.x);
  }
  // Cloud shadows: the sun's share lost under the clouds (where the sun still
  // reached the surface).
  float cloud_sun = 1.0;
  if (cloud.x > 0.0 && cloud2.w > 0.0 && has_depth && !on_sea && debug == 0u) {
    cloud_sun = CloudSunlight(world);
    const vec3 lum = vec3(0.2126, 0.7152, 0.0722);
    float sun_l = dot(sun_color.rgb, lum) * max(n_dot_l, 0.0);
    float amb_l = dot(ambient.rgb, lum);
    float shadowed = clamp(amb_l / max(sun_l + amb_l, 1e-4), 0.2, 1.0);
    float reach = (own ? 1.0 : game_lit) * (1.0 - min(occluded, 1.0)) *
                  smoothstep(0.02, 0.12, n_dot_l);
    color.rgb *= mix(1.0, shadowed, (1.0 - cloud_sun) * cloud2.w * reach);
  }
  // Water: shallow water clearer and turquoise over the seabed, foam where
  // the sea meets hulls and shores. The surface point is where the eye ray
  // crosses the sea level. Debug 22: foam red, shallow green, at any
  // distance.
  bool water_debug = debug == 22u;
  if ((wfx.x > 0.0 || wfx.z > 0.0) && !sky && (debug == 0u || water_debug) &&
      camera.y > sea_level && dir.y < -1e-4) {
    float t_surf = (sea_level - camera.y) / dir.y;
    // Seen through the water: depth well below the surface point.
    // Seabed seen through the water is tinted blue or turquoise; dry ground
    // a little under the measured sea level (Guadalcanal's airfield) is not,
    // and took the shallows' tint and foam (white-green, grainy).
    vec3 cn = color.rgb / max(dot(color.rgb, vec3(0.2126, 0.7152, 0.0722)), 1e-4);
    // Nor dark: the oil stains on the airfield (dark and a little blue) took
    // the shallows' tint and drifting foam in runs whose measured sea level
    // came out over the airfield (white specks moving like water).
    float lum_here = dot(color.rgb, vec3(0.2126, 0.7152, 0.0722));
    float lum_sky = dot(HazeColor(normalize(vec3(dir.x, 0.05, dir.z))), vec3(0.2126, 0.7152, 0.0722));
    bool through_water = (cn.b > cn.r * 1.15 || (cn.g > cn.r * 1.15 && cn.b > cn.r)) &&
                         lum_here > 0.12 * lum_sky;
    bool below = has_depth && !on_sea && world.y < sea_level - 0.05 && dist > t_surf && through_water;
    bool surface = !has_depth || on_sea;
    if ((below || surface) && (t_surf < 400.0 || water_debug)) {
      vec3 p = camera.xyz + dir * t_surf;
      float depth_below = below ? sea_level - world.y : 1e9;
      float near_edge = 1e9;  // metres from a hull or the shore
      bool enclosed = false;
#ifdef SCENE_FX_RT
      if (rt.x > 0.0) {
        // Four level rays just over the water: what they hit is a hull or the
        // shore; hit all round within 8 m, the water is inside a boat.
        int hits = 0;
        for (int i = 0; i < 4; ++i) {
          float a = 1.5707963 * float(i) + 0.3;
          vec3 d = vec3(cos(a), 0.0, sin(a));
          vec3 hn;
          float t = TraceMeshes(p + vec3(0.0, 0.25, 0.0), d, 8.0, hn);
          if (t > 0.0) {
            ++hits;
            near_edge = min(near_edge, t);
          }
        }
        enclosed = hits == 4;
      }
#endif
      if (!enclosed && (rt.x > 0.0 || surface)) {
        // Shallow: the seabed shows through clearer and turquoise.
        if (below && wfx.z > 0.0) {
          float shallow = wfx.z * (1.0 - smoothstep(0.2, 3.5, depth_below));
          if (water_debug) color.g = 1.0;
          color.rgb = mix(color.rgb, color.rgb * vec3(0.9, 1.18, 1.1) * 1.3, shallow);
        }
        // Foam: strongest at the edge, broken up by drifting noise.
        float edge = min(near_edge, depth_below * 4.0);
        if (wfx.x > 0.0 && edge < wfx.w) {
          float close = 1.0 - edge / wfx.w;
          vec2 drift = vec2(0.31, 0.17) * refl.z;
          float noise = 0.6 * CloudNoise2(p.xz * 0.9 + drift) +
                        0.4 * CloudNoise2(p.xz * 3.1 - drift * 1.7);
          float foam = smoothstep(0.55, 0.8, noise + close * 0.45) * close * wfx.x;
          // Fades out with distance (a few pixels wide, it only flickers).
          if (water_debug) color.r = foam;
          foam *= water_debug ? 0.0 : 1.0 - smoothstep(150.0, 400.0, t_surf);
          const vec3 lum = vec3(0.2126, 0.7152, 0.0722);
          float white = dot(HazeColor(normalize(vec3(dir.x, 0.05, dir.z))), lum);
          int slice;
          float lit = mix(0.55, 1.0, SunVisibility(p, slice));
          color.rgb = mix(color.rgb, vec3(white * 1.15 * lit), clamp(foam, 0.0, 0.9));
        }
      }
    }
  }
  if (water_debug) {
    imageStore(scene, ivec2(pixel), color);
    return;
  }
  // Ambient occlusion: the ambient share of the pixel's light (and ao.z of the
  // sun's) darkened by how much of the sky the surroundings hide. Debug 19
  // shows it; debug 20: the left half without it.
  if (ao.x > 0.0 && has_depth && !on_sea && (debug == 0u || debug == 19u || debug == 20u)) {
    float visibility = HalfResCached(local, dist).x;
    if (debug == 19u) {
      imageStore(scene, ivec2(pixel), vec4(vec3(visibility * 0.6), color.a));
      return;
    }
    const vec3 lum = vec3(0.2126, 0.7152, 0.0722);
    float sun_l = dot(sun_color.rgb, lum) * max(n_dot_l, 0.0) * (own ? 1.0 : game_lit) *
                  (1.0 - min(occluded, 1.0)) * cloud_sun;
    float amb_l = dot(ambient.rgb, lum);
    float share = amb_l / max(sun_l + amb_l, 1e-4);
    if (!(debug == 20u && local.x < size.x / 2)) {
      color.rgb *= max(1.0 - ao.x * mix(ao.z, 1.0, share) * (1.0 - visibility), 0.0);
    }
  }
  if (debug == 20u) debug = 0u;
  // Procedural surface detail, by world position: grime everywhere (more in
  // corners), and on bare ground (traced terrain that is not grass) small
  // pebbles lit by the sun and cracks in dry patches. Each fades out where it
  // would be smaller than a few pixels. Debug 23: terrain red, pebbles green,
  // cracks blue.
  // (Not under the sea: the seabed seen through the water is terrain too.)
  if (detail.x > 0.0 && has_depth && !on_sea && dist < detail.y &&
      (world.y > sea_level + 0.3 || ground_at_sea) && (debug == 0u || debug == 23u)) {
    const vec3 lum = vec3(0.2126, 0.7152, 0.0722);
    float pixel_m = dist * length(ViewDir(vec2(local) + vec2(1.0, 0.0)) - ViewDir(vec2(local)));
    float fade = 1.0 - smoothstep(0.6 * detail.y, detail.y, dist);
    // Grime: broad blotches, and darker where the surroundings hide the sky.
    float blotch = CloudNoise(world * 0.6) * 0.65 + CloudNoise(world * 2.3 + 7.1) * 0.35;
    float hidden = ao.x > 0.0 ? 1.0 - HalfResCached(local, dist).x : 0.0;
    float grime = (0.1 * smoothstep(0.45, 0.8, blotch) + 0.3 * hidden) * detail.x * fade;
    vec3 v23 = color.rgb * 0.25;
    bool terrain = ground_at_sea;
#ifdef SCENE_FX_RT
    if (rt.x > 0.0 && !terrain) {
      // From the occlusion pass (half resolution) where its pixel is this
      // surface; else traced here.
      bool known = false;
      if (ao.x > 0.0) {
        vec4 hq = imageLoad(ao_image, clamp(local / 2, ivec2(0), imageSize(ao_image) - 1));
        if (hq.a >= 0.0 && hq.g > 0.0 && abs(hq.g - dist) < 0.03 * dist + 0.2) {
          terrain = hq.a > 0.5;
          known = true;
        }
      }
      if (!known) terrain = GroundAt(world, normal, dir, dist, sea_level);
    }
#endif
    // Bare ground (sand, dirt, rock: warm or grey): not green (grass), and
    // not cool (blue or turquoise water drawn over the terrain, as in the
    // lagoon, above the measured sea level).
    vec3 c = color.rgb / max(dot(color.rgb, lum), 1e-4);
    // Grass (olive to green: green at least as strong as red, well above
    // blue) against bare ground (sand, dirt, rock: warm or grey).
    bool green = c.g >= c.r * 0.97 && c.g > c.b * 1.2;
    bool bare = terrain && !green && c.r >= c.b * 0.95;
    if (terrain) v23.r = 0.35;
    if (bare) {
      v23.r = 0.8;
      vec3 l = normalize(sun_dir.xyz);
      // Stones and pebbles as a thin height layer over the ground: the view
      // ray is marched through it back from the ground point, so they stand
      // up with their own outlines and parallax; ground they do not cover
      // marches towards the sun for their contact shadows.
      float stone_fade = smoothstep(2.0, 5.0, detail.z / max(pixel_m, 1e-4)) * fade *
                         (1.0 - smoothstep(9.0, 12.0, dist));
      if (stone_fade > 0.0) {
        float layer = detail.z * 4.0 * 0.42 * 0.8;  // the tallest stone
        vec3 up = normal;
        float lit = 1.0 - occluded;
        float ground_ndl = max(dot(up, l), 0.0);
        vec3 back = -dir;
        float rise = dot(back, up);
        bool hit = false;
        vec3 sn;
        float tint, stone_h = 0.0;
        float patch_noise = CloudNoise(vec3(world.xz * 0.35, 3.0));
        float density = smoothstep(0.35, 0.75, patch_noise);
        if (rise > 0.04 && density > 0.0) {
          const int kSteps = 8;
          float t_in = layer / rise;
          float prev_t = t_in;
          for (int i = 1; i <= kSteps; ++i) {
            float t = t_in * (1.0 - float(i) / float(kSteps));
            vec3 p = world + back * t;
            float h = StoneHeight(p.xz, density, sn, tint);
            if (t * rise <= h) {
              // Between the last step above and this one.
              float lo = t, hi = prev_t;
              for (int r = 0; r < 1; ++r) {
                float mid = 0.5 * (lo + hi);
                if (mid * rise <= StoneHeight((world + back * mid).xz, density, sn, tint)) lo = mid;
                else hi = mid;
              }
              stone_h = StoneHeight((world + back * lo).xz, density, sn, tint);
              hit = stone_h > 0.0;
              break;
            }
            prev_t = t;
          }
        }
        float k = detail.x * stone_fade;
        if (hit) {
          // Lit like the ground around it, by its own facing; darker at its
          // foot (the ground's bounce and sky light blocked).
          float stone_ndl = max(dot(sn, l), 0.0);
          float shade = (0.45 + lit * stone_ndl) / (0.45 + lit * ground_ndl);
          // Steep sides (towards its foot) darker: less sky reaches them.
          float foot = mix(0.7, 1.0, smoothstep(0.3, 0.9, sn.y));
          // Stone a little cooler or warmer than the ground, by its tint.
          vec3 hue = mix(vec3(0.95, 0.97, 1.02), vec3(1.04, 1.0, 0.94), fract(tint * 7.31));
          color.rgb *= mix(vec3(1.0), shade * tint * foot * hue, k);
          v23.g = 1.0;
        } else if (lit > 0.0 && l.y > 0.05 && density > 0.0) {
          // Shadowed by a stone between here and the sun.
          float sun_rise = dot(l, up);
          for (int i = 1; i <= 3; ++i) {
            float t = layer / max(sun_rise, 0.05) * float(i) / 3.0;
            vec3 dummy_n;
            float dummy_t;
            if (t * sun_rise < StoneHeight((world + l * t).xz, density, dummy_n, dummy_t)) {
              color.rgb *= 1.0 - 0.5 * lit * ground_ndl / (0.45 + ground_ndl) * k;
              v23.g = 0.5;
              break;
            }
          }
        }
      }
      // Cracks: thin dark lines along cell borders in dry patches.
      float crack_fade = smoothstep(4.0, 10.0, detail.w / max(pixel_m, 1e-4)) * fade;
      if (crack_fade > 0.0) {
        float d1, d2;
        vec2 off, cell;
        vec2 q = world.xz / detail.w;
        // Wobbly lines rather than straight cell edges.
        q += 0.25 * vec2(CloudNoise(vec3(q * 2.1, 0.5)), CloudNoise(vec3(q * 2.1, 8.5))) - 0.125;
        Voronoi(q, d1, d2, off, cell);
        float width = 0.012 + pixel_m / detail.w * 0.6;
        float line = 1.0 - smoothstep(width * 0.5, width, d2 - d1);
        float dry = smoothstep(0.55, 0.75, CloudNoise(vec3(world.xz * 0.15, 11.0)));
        float crack = line * dry * crack_fade * detail.x;
        // Faint: on the airfield's wide flat ground strong ones read as seams.
        color.rgb *= 1.0 - 0.22 * crack;
        v23.b = crack;
      }
    }
    // Grass: blades in a layer kGrassHeight deep over green ground, the view
    // ray marched down through it from the top, so the nearest blade wins;
    // each blade a thin leaning strand from a random point in its cell,
    // darker at its foot, lighter and yellower at its tip. Faded out where
    // a blade would be thinner than a pixel.
    bool grassy = terrain && green && normal.y > 0.6;
    float grass_fade = smoothstep(0.5, 1.5, 0.012 / max(pixel_m, 1e-4)) * fade *
                       (1.0 - smoothstep(25.0, 30.0, dist));
    if (grassy && grass_fade > 0.0) {
      const float kGrassHeight = 0.22, kCell = 0.045;
      vec3 back = -dir;
      float rise = max(dot(back, normal), 0.03);
      float t_in = kGrassHeight / rise;
      float meadow = smoothstep(0.25, 0.6, CloudNoise(vec3(world.xz * 0.5, 13.0)));
      const int kSteps = 10;
      for (int i = 0; i < kSteps; ++i) {
        float t = t_in * (1.0 - (float(i) + 0.5) / float(kSteps));
        vec3 p = world + back * t;
        float y = t * rise;  // over the ground
        vec2 g = p.xz / kCell;
        vec2 cell = floor(g);
        if (CloudHash(vec3(cell, 21.0)) > 0.35 + 0.6 * meadow) continue;
        float hb = kGrassHeight * (0.35 + 0.65 * CloudHash(vec3(cell, 22.0)));
        if (y > hb) continue;
        float u = y / hb;
        vec2 base = vec2(0.2) + 0.6 * vec2(CloudHash(vec3(cell, 23.0)), CloudHash(vec3(cell, 24.0)));
        float lean_a = 6.2831853 * CloudHash(vec3(cell, 25.0));
        vec2 axis = base + vec2(cos(lean_a), sin(lean_a)) * (0.35 * u * u);
        float width = 0.16 * (1.0 - u);  // in cells
        vec2 o = g - cell - axis;
        if (dot(o, o) < width * width) {
          float shade = mix(0.45, 1.15, u);
          vec3 tip = mix(vec3(1.0), vec3(1.15, 1.08, 0.8), u * u);
          color.rgb *= mix(vec3(1.0), shade * tip * (0.85 + 0.3 * CloudHash(vec3(cell, 26.0))),
                           detail.x * grass_fade);
          v23.g = 0.7;
          break;
        }
      }
    }
    color.rgb *= 1.0 - grime;
    if (debug == 23u) {
      imageStore(scene, ivec2(pixel), vec4(v23, color.a));
      return;
    }
  }
#ifdef SCENE_FX_RT
  // Ray-traced sky and bounce light: the game's ambient share of the pixel's
  // light (the rest is the sun's where it reaches) scaled by what the rays saw.
  // The colour before the bounce light: what the cache learns (storing the
  // lit result fed the bounce back into itself each frame, and bare ground
  // washed out to white, blotchy and grainy).
  vec3 unbounced = color.rgb;
  if (gi.x > 0.0 && has_depth && !on_sea) {
    const vec3 lum = vec3(0.2126, 0.7152, 0.0722);
    float sun_l = dot(sun_color.rgb, lum) * max(n_dot_l, 0.0) * (own ? 1.0 : game_lit) *
                  (1.0 - occluded);
    float amb_l = dot(ambient.rgb, lum);
    float share = amb_l / max(sun_l + amb_l, 1e-4);
    // Between 0.6 and 1.1 times the game's ambient: lower turned the ground
    // under a plane into a dark stain; higher, bounce rays from shadow onto
    // cache cells learnt in the sun brightened shadows in moving patches
    // (the ground looked like water).
    vec3 light = clamp(LightAt(local, dist, normal), vec3(0.6), vec3(1.1));
    if (debug == 10u) {
      imageStore(scene, ivec2(pixel), vec4(light * 0.5, color.a));
      return;
    }
    if (debug == 0u && !(gi_split && local.x < size.x / 2)) {
      color.rgb *= max(1.0 + share * gi.x * (light - 1.0), vec3(0.0));
    }
  }
  // What surfaces look like, for what reflection and bounce rays hit: a
  // quarter of the pixels each frame, in turn.
  if (gi.w >= 0.0 && rt.x > 0.0 && has_depth && !on_sea && (debug == 0u || debug == 12u) &&
      ((local.x & 1) | ((local.y & 1) << 1)) == (int(gi.w) & 3)) {
    CacheStore(world, normal, unbounced);
  }
  if (debug == 12u && has_depth) {
    vec3 cached;
    bool known = CacheLoad(world, normal, cached);
    imageStore(scene, ivec2(pixel), vec4(known ? cached : vec3(1.0, 0.0, 1.0), color.a));
    return;
  }
  // Reflections: the sea (water's Fresnel, waves), other surfaces by their
  // --pbr material (smooth ones only: a mirror image on a rough one looks
  // like glass).
  if (rt.x > 0.0 && !sky && (debug == 0u || debug == 11u || debug == 13u)) {
    // Mirror images on the open sea only: ground at the sea's height (in
    // shadow it looks as blue as water) took them for puddles.
    bool water_surface = !has_depth;
    vec2 refl_amount = on_sea ? vec2(0.0) : refl.xy;
    float rough = 1.0, metal = 0.0;
    bool has_material = !water_surface && refl_amount.y > 0.0 &&
                        MaterialAt(ivec2(pixel), dist, rough, metal);
    if (debug == 13u) {
      // Material: red smooth, green metal; grey none, blue water.
      vec3 v = has_material ? vec3(1.0 - rough, metal, 0.15)
                            : water_surface ? vec3(0.1, 0.2, 0.6) : vec3(0.25);
      imageStore(scene, ivec2(pixel), vec4(v, color.a));
      return;
    }
    vec3 n = water_surface ? SeaNormal(world, dist) : normal;
    float facing = clamp(dot(n, -dir), 0.0, 1.0);
    float f0 = water_surface ? 0.02 : mix(0.04, 0.7, metal);
    float fresnel = f0 + (1.0 - f0) * pow(1.0 - facing, 5.0);
    float amount = water_surface ? fresnel * refl_amount.x
                 : fresnel * refl_amount.y * (1.0 - smoothstep(0.1, 0.4, rough));
    // Nothing facing up but water mirrors here: dirt whose material read
    // smooth reflected planes in red and white patches, and the oil decals
    // under the planes (read as metal) shimmered like water as the colour
    // cache of the plane above them changed. The game's own reflection
    // still shows them the sky.
    if (!water_surface && normal.y > 0.7) amount = 0.0;
    // Metals tint what they reflect with their colour.
    const vec3 lum3 = vec3(0.2126, 0.7152, 0.0722);
    vec3 tint = mix(vec3(1.0), clamp(color.rgb / max(dot(color.rgb, lum3), 1e-3), 0.0, 2.0), metal);
    if (amount > 0.01) {
      // The game's own reflection already shows the sky (and on the sea its
      // waves and glints): only where a mesh is in the way does it change.
      vec3 hit_color, sky_color;
      float cover = ReflectionHit(world, n, dir, dist, water_surface, hit_color, sky_color);
      vec3 change = clamp(amount, 0.0, 1.0) * cover * (hit_color - sky_color) * tint;
      if (debug == 11u) {
        imageStore(scene, ivec2(pixel), vec4(cover * amount, 0.5 + dot(change, vec3(0.3, 0.6, 0.1)), water_surface ? 0.5 : 0.0, color.a));
        return;
      }
      color.rgb = max(color.rgb + change, color.rgb * 0.15);
    } else if (debug == 11u) {
      imageStore(scene, ivec2(pixel), vec4(0.5, 0.5, 0.5, color.a));
      return;
    }
  }
#endif
  // The muzzle flash's light (the game's is off on the world): its colour on
  // the surface's own colour, which is the pixel over the light the game gave
  // it; fading with the square of the distance to nothing at flash2.w, and
  // shadowed by the traced meshes.
  if (flash.w > 0.0 && has_depth && debug == 0u) {
    vec3 to_light = flash.xyz - world;
    float d = length(to_light);
    if (d < flash2.w && d > 1e-3) {
      vec3 l = to_light / d;
      float n_l = max(dot(normal, l), 0.0);
      float x = d / flash2.w;
      float window = (1.0 - x * x) * (1.0 - x * x);
      // Six times the game's colour over d^2 (the game's own is the colour
      // over d, unshadowed: as strong as this at 6 m and lighting everything
      // around); brighter close by, nothing past the radius.
      float falloff = 6.0 * window / max(d * d, 0.25);
      float vis = 1.0;
#ifdef SCENE_FX_RT
      if (rt.x > 0.0 && n_l > 0.0) {
        vec3 hn;
        float t = TraceMeshes(world + normal * (0.03 + 0.002 * dist), l, d - 0.4, hn);
        if (t > 0.0) vis = 0.0;
      }
#endif
      if (n_l * vis > 0.0) {
        const vec3 lum = vec3(0.2126, 0.7152, 0.0722);
        vec3 had = ambient.rgb + sun_color.rgb * max(n_dot_l, 0.0) * (own ? 1.0 : game_lit) *
                                     (1.0 - min(occluded, 1.0)) * cloud_sun;
        vec3 albedo = color.rgb / max(had, vec3(1e-3));
        color.rgb += albedo * flash2.rgb * (flash.w * n_l * falloff * vis);
      }
    }
  }
  if (debug == 0u) color.rgb = ApplyGore(color.rgb, world, !sky && !IsSky(depth), dir, dist);
#ifdef SCENE_FX_RT
  // Debug: the terrain meshes (14, front faces only), all meshes (15) or the
  // alpha-tested ones (16) seen from the eye: green where they meet the drawn
  // surface, red in front of it, blue behind, black where the ray misses them.
  if ((debug >= 14u && debug <= 16u) && has_depth) {
    vec3 dir = normalize(world - camera.xyz);
    rayQueryEXT query;
    rayQueryInitializeEXT(query, world_meshes,
                          debug == 14u ? gl_RayFlagsCullBackFacingTrianglesEXT : gl_RayFlagsNoneEXT,
                          debug == 14u ? 0x02 : debug == 16u ? 0x04 : 0xFF, camera.xyz, 0.0, dir, 5000.0);
    TRACE_ALPHA_TESTED(query)
    vec3 v = vec3(0.0);
    if (rayQueryGetIntersectionTypeEXT(query, true) != gl_RayQueryCommittedIntersectionNoneEXT) {
      float t = rayQueryGetIntersectionTEXT(query, true);
      float off = (t - dist) / max(1.0 + 0.01 * dist, 1e-3);
      v = abs(off) < 1.0 ? vec3(0.1, 0.8, 0.2) : off < 0.0 ? vec3(0.9, 0.1, 0.1) : vec3(0.1, 0.3, 1.0);
    }
    imageStore(scene, ivec2(pixel), vec4(mix(v, color.rgb, 0.35), color.a));
    return;
  }
#endif
  if (debug == 8u && (sky || IsSky(depth))) {
    vec3 v = OnSunRing(local, size) ? vec3(1.0, 0.0, 1.0) : color.rgb * 0.3;
    // Top left: red if the sun is in front of the camera, blue if behind.
    if (all(lessThan(local, ivec2(60))))
      v = dot(ViewDir(vec2(size) * 0.5), sun_dir.xyz) > 0.0 ? vec3(1, 0, 0) : vec3(0, 0, 1);
    imageStore(scene, ivec2(pixel), vec4(v, color.a));
    return;
  }
  if (debug == 7u) {
    // The three shadow slices side by side along the top of the screen.
    int w = int(min((1.0 / screen.z) / 3.0, 1.0 / screen.w));
    if (local.y < w) {
      uint s = uint(local.x / w);
      if (s < 3u) {
        uvec2 texel = uvec2(vec2(local.x - int(s) * w, local.y) * float(shadow_info.x) / float(w));
        float d = ShadowDepth(s, texel);
        color.rgb = d >= 0.99999 ? vec3(0.0, 0.0, 0.3) : vec3(d, fract(d * 20.0), fract(d * 400.0));
      }
    }
    imageStore(scene, ivec2(pixel), color);
    return;
  }
  if (debug != 0u) {
    int slice;
    float vis = SunVisibility(world, slice);
    vec3 v = vec3(0.0);
    if (debug == 1u) v = vec3(fract(dist / 50.0), fract(world.y / 10.0), sky ? 1.0 : 0.0);
    if (debug == 2u) v = vec3(vis) * (slice == 0 ? vec3(0.4, 1, 0.4) : slice == 1 ? vec3(1, 1, 0.4) : slice == 2 ? vec3(1, 0.4, 0.4) : vec3(0.4, 0.4, 1));
    if (debug == 3u) v = vec3(depth * 20.0, fract(depth * 100.0), fract(depth * 10000.0));
    if (debug == 5u) v = HazeColor(dir);
    if (debug == 6u) v = vec3(dist < 0.0 ? 1.0 : 0.0, isnan(dist) || isinf(dist) ? 1.0 : 0.0,
                              sky ? 0.5 : 0.0) + (world4.w <= 1e-6 ? vec3(0.5, 0.5, 0.0) : vec3(0.0));
    if (debug == 4u) v = vec3(world.y > sea.x, world.y > sea.x + 5.0, world.y > sea.x + 10.0);
    imageStore(scene, ivec2(pixel), vec4(v, color.a));
    return;
  }

  // Single scattering in the height fog. The haze converges to the measured
  // horizon sky colour; where the shadow map says the air is in shadow it
  // loses the sunlit share (the light shafts).
  int steps = int(march.y);
  if (steps == 0) {  // no fog (blood only)
    color.rgb = max(color.rgb + overlay, vec3(0.0));
    imageStore(scene, ivec2(pixel), color);
    return;
  }
  float jitter = InterleavedGradientNoise(vec2(pixel), march.w);
  if (cloud.x > 0.0) {
    // In front of a surface only if the surface is beyond the layer's start.
    float t0, t1;
    CloudSpan(camera.xyz, dir, t0, t1);
    if (t1 > t0 && (sky || dist > t0)) {
      vec4 c = CloudsAt(local);
      color.rgb = color.rgb * (1.0 - c.a) + c.rgb;
    }
  }
  vec3 haze = HazeColor(dir);
  float scattered, optical;
  if (!HazeAt(local, dist, sky, scattered, optical)) HazeMarch(dir, len, sky, jitter, scattered, optical);
  float transmittance = exp(-optical);
  color.rgb = color.rgb * transmittance + haze * scattered;
  color.rgb = max(color.rgb + overlay * transmittance, vec3(0.0));
  imageStore(scene, ivec2(pixel), color);
}
