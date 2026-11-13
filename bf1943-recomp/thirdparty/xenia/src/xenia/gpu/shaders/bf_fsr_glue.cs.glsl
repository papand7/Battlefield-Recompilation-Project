#version 460
// bf1943 FSR 3 (gpu/vulkan/bf_fsr3.cc), around the upscale:
// mode 0: the frame the game's tone mapping pass wrote to the buffer (RGBA8
//   per pixel, before the HUD) into an image FSR reads;
// mode 1: the HUD back over FSR's output: what the final frame has over the
//   pre-HUD one, sampled bilinearly up to the output size.
#extension GL_EXT_samplerless_texture_functions : require

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform Params {
  uint mode;
  uint render_width, render_height;
  uint display_width, display_height;
  uint color_offset;  // in uints
  uint pitch;         // uints a row
};

layout(set = 0, binding = 0, std430) readonly buffer Captured { uint captured[]; };
layout(set = 0, binding = 1, rgba8) uniform image2D pre_hud;   // render size
layout(set = 0, binding = 2) uniform texture2D final_frame;    // render size
layout(set = 0, binding = 3, rgba8) uniform image2D upscaled;  // display size

vec3 HudAt(ivec2 p) {
  p = clamp(p, ivec2(0), ivec2(render_width, render_height) - 1);
  return texelFetch(final_frame, p, 0).rgb - imageLoad(pre_hud, p).rgb;
}

void main() {
  ivec2 p = ivec2(gl_GlobalInvocationID.xy);
  if (mode == 0u) {
    if (p.x >= int(render_width) || p.y >= int(render_height)) return;
    // The tone mapping pass leaves the frame's last row unwritten: the one
    // above it stands in (else the HUD step adds it back as a bright line).
    ivec2 s = ivec2(p.x, min(p.y, int(render_height) - 2));
    uint v = captured[color_offset + uint(s.y) * pitch + uint(s.x)];
    imageStore(pre_hud, p, vec4(unpackUnorm4x8(v).rgb, 1.0));
    return;
  }
  if (p.x >= int(display_width) || p.y >= int(display_height)) return;
  vec2 g = (vec2(p) + 0.5) * vec2(render_width, render_height) / vec2(display_width, display_height) - 0.5;
  ivec2 g0 = ivec2(floor(g));
  vec2 f = g - vec2(g0);
  vec3 hud = mix(mix(HudAt(g0), HudAt(g0 + ivec2(1, 0)), f.x),
                 mix(HudAt(g0 + ivec2(0, 1)), HudAt(g0 + ivec2(1, 1)), f.x), f.y);
  vec4 o = imageLoad(upscaled, p);
  imageStore(upscaled, p, vec4(clamp(o.rgb + hud, 0.0, 1.0), 1.0));
}
