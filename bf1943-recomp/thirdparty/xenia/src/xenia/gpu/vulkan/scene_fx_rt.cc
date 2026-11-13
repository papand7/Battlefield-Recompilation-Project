#include "xenia/gpu/vulkan/scene_fx_rt.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/texture_address.h"
#include "xenia/gpu/texture_info.h"
#include "xenia/gpu/texture_util.h"
#include "xenia/gpu/vulkan/vulkan_command_processor.h"

namespace xe {
namespace gpu {
namespace scenefx {
namespace rt {

namespace {

constexpr uint32_t kMaxInstances = 32768;
// Instance buffers and mesh upload segments in flight.
constexpr uint32_t kSlots = 4;
constexpr VkDeviceSize kUploadSegment = VkDeviceSize(24) << 20;
constexpr VkDeviceSize kScratchSize = VkDeviceSize(64) << 20;
constexpr VkDeviceSize kPoolChunk = VkDeviceSize(128) << 20;
constexpr uint32_t kMaxPoolChunks = 12;
constexpr uint32_t kMaxMeshVertices = 1 << 20;
// Instance masks (scene_fx.cs.glsl): drawn meshes, the terrain, alpha-tested meshes.
constexpr uint8_t kMaskObjects = 0x01, kMaskTerrain = 0x02, kMaskFoliage = 0x04;
// Indices decoded on the CPU per frame (new meshes wait for the next frame).
constexpr uint32_t kDecodeBudget = 3 << 20;
// How long static objects stay while they are out of view (frames).
constexpr uint64_t kStaticKeepFrames = 60 * 180;
// Triangle normals kept on the GPU (one packed normal each; the instance's
// custom index is its mesh's first, 24 bits).
constexpr uint32_t kMaxNormals = (1u << 24) - 1;
constexpr uint32_t kNoNormals = 0xFFFFFF;
// After the normals, in the same buffer (scene_fx.cs.glsl AlphaSolid): per
// alpha-tested triangle its corners' texture coordinates (half2 each) and its
// alpha mask (word offset | log2 width << 24 | log2 height << 28), an
// instance's shader binding table record offset being its mesh's first; then
// the masks' bits.
constexpr uint32_t kAlphaBase = 1u << 24;
constexpr uint32_t kMaxAlphaTriangles = 1u << 20;
constexpr uint32_t kMaskBase = kAlphaBase + kMaxAlphaTriangles * 4;
constexpr uint32_t kMaxMaskWords = 1u << 21;
constexpr uint32_t kTriangleDataWords = kMaskBase + kMaxMaskWords;
constexpr uint32_t kNoAlpha = UINT32_MAX;
// BuildMask: the texture's alpha passes the test everywhere.
constexpr uint32_t kOpaqueMask = UINT32_MAX - 1;

VkDeviceSize Align(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) / a * a; }

uint64_t Fnv(const void* data, size_t size, uint64_t h = 14695981039346656037ull) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

uint32_t Load32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

// The guest's endian swap of a 32-bit word (as the shaders' vertex fetch).
uint32_t Swap(uint32_t v, uint32_t endian) {
  switch (endian) {
    case 1:  // 8 in 16
      return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    case 2:  // 8 in 32
      return (v << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24);
    case 3:  // 16 in 32
      return (v << 16) | (v >> 16);
    default:
      return v;
  }
}

float Half(uint32_t h) {
  uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 31, mantissa = h & 1023;
  float v;
  if (exponent == 0) {
    v = std::ldexp(float(mantissa), -24);
  } else if (exponent == 31) {
    v = mantissa ? NAN : INFINITY;
  } else {
    v = std::ldexp(float(mantissa | 1024), int(exponent) - 25);
  }
  return sign ? -v : v;
}

// Where a vertex shader's position comes from.
struct PositionFetch {
  bool ok = false;
  uint32_t fetch_constant = 0, stride = 0;
  int32_t offset = 0;
  xenos::VertexFormat format = xenos::VertexFormat::kUndefined;
  uint8_t components[3] = {};
  // Its w is made 1 by `cndeq rN, c255.xxxy, rM.zxyy, c255.yyyy`, which
  // needs c255.xy = (0, 1).
  bool needs_c255 = false;
  // No world matrix (c16-c18) in the shader: the world matrix is worked out
  // from c12-c15 and the frame's view and projection.
  bool world_from_view = false;
  // Register components holding world x, y, z.
  uint8_t reg_components[3] = {0, 1, 2};
  // Terrain: world y gets this constant's x added (UINT32_MAX: none).
  uint32_t height_constant = UINT32_MAX;
  // The object to clip matrix's first constant and the world matrix's (12 and
  // 16, or 16 and 20 in the foliage shaders).
  uint32_t clip_constant = 12, world_constant = 16;
  // Rigid multi-part models (vehicles): each vertex's matrix, rows
  // c[part_base + a0] to c[part_base + 2 + a0], with a0 a byte of the vertex
  // (part_offset dwords in, byte part_component), times c255.z where
  // part_c255. UINT32_MAX: one matrix for the draw.
  uint32_t part_base = UINT32_MAX;
  int32_t part_offset = 0;
  uint8_t part_component = 0;
  bool part_c255 = false;
};

// Where an alpha-tested draw's coverage comes from: the pixel shader's
// texture fetch of all four components from an interpolator's xy, which the
// vertex shader copies from a vertex attribute.
struct AlphaSource {
  bool ok = false;
  uint32_t texture = 0;  // fetch constant
  uint32_t fetch_constant = 0;
  int32_t offset = 0;
  xenos::VertexFormat format = xenos::VertexFormat::kUndefined;
  uint8_t components[2] = {};
  uint8_t texel_component = 3;  // of the fetched texel, read as alpha
  bool doubled = false;         // the pixel shader samples at twice the coordinates
};

uint32_t FormatWords(xenos::VertexFormat format) {
  switch (format) {
    case xenos::VertexFormat::k_16_16_FLOAT:
      return 1;
    case xenos::VertexFormat::k_16_16_16_16_FLOAT:
    case xenos::VertexFormat::k_32_32_FLOAT:
      return 2;
    case xenos::VertexFormat::k_32_32_32_FLOAT:
      return 3;
    case xenos::VertexFormat::k_32_32_32_32_FLOAT:
      return 4;
    default:
      return 0;
  }
}

// What identifies a mesh in guest memory.
struct MeshKey {
  uint32_t vertex_base, vertex_size, vertex_endian;  // dwords
  uint32_t stride, offset, format, components;
  uint32_t index_base, index_count;
  // Bit 0 32-bit indices, 1-2 endian, 4-9 primitive type, 12 auto index,
  // 13 strip restart.
  uint32_t index_info;
  uint32_t index_offset, reset_index;
  // Alpha-tested: the texture coordinates' attribute (format 0 for none) and
  // the alpha mask.
  uint32_t uv_offset, uv_format, uv_components, mask;
  // A multi-part model's part: bit 0 set, bits 1-2 the byte, 8-15 its value;
  // the triangles whose first vertex has it.
  uint32_t part_offset, part;
  bool operator==(const MeshKey& o) const {
    return !std::memcmp(this, &o, sizeof(*this));
  }
};
struct MeshKeyHash {
  size_t operator()(const MeshKey& k) const { return size_t(Fnv(&k, sizeof(k))); }
};

struct Mesh {
  uint64_t sample = 0;  // hash of some of its guest data, to see it replaced
  VkAccelerationStructureKHR blas = VK_NULL_HANDLE;
  VkDeviceAddress address = 0;  // set when its build is recorded
  float bmin[3] = {}, bmax[3] = {};
  uint32_t triangles = 0;
  uint32_t normal_base = kNoNormals;  // its first triangle normal
  uint32_t alpha_base = kNoAlpha;     // its first alpha-tested triangle
  bool dead = false;
};

// A unit vector as two 16-bit signed normalized octahedral coordinates.
uint32_t PackNormal(float x, float y, float z) {
  float l = std::abs(x) + std::abs(y) + std::abs(z);
  if (!(l > 0.0f)) return 0;
  x /= l;
  y /= l;
  if (z < 0.0f) {
    float ox = (1.0f - std::abs(y)) * (x >= 0.0f ? 1.0f : -1.0f);
    float oy = (1.0f - std::abs(x)) * (y >= 0.0f ? 1.0f : -1.0f);
    x = ox;
    y = oy;
  }
  auto snorm = [](float v) {
    return uint32_t(int32_t(std::round(std::clamp(v, -1.0f, 1.0f) * 32767.0f)) & 0xFFFF);
  };
  return snorm(x) | snorm(y) << 16;
}

struct PendingMesh {
  uint32_t mesh;
  std::vector<float> vertices;
  std::vector<uint32_t> indices;
  // Alpha-tested: two per vertex, and the mask.
  std::vector<float> uvs;
  uint32_t mask = kNoAlpha;
};

// A float as a half (round to nearest, no denormals).
uint32_t ToHalf(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  uint32_t sign = (u >> 16) & 0x8000;
  int32_t exponent = int32_t((u >> 23) & 0xFF) - 127 + 15;
  if (!std::isfinite(f) || exponent >= 31) return sign | 0x7BFF;
  if (exponent <= 0) return sign;
  return sign | ((uint32_t(exponent) << 10) + ((u >> 13) & 0x3FF) + ((u >> 12) & 1));
}

struct Draw {
  uint32_t mesh;
  float w[12];
  // Without a world matrix in the shader: c12-c15, for w in BuildFrame.
  bool world_from_view;
  float wvp[16];
  float height;  // terrain: added to world y
  const void* depth_target;
};

struct Instance {
  uint32_t mesh;
  float w[12];
  float wmin[3], wmax[3];
  uint64_t first_frame, last_frame;
};

std::unordered_map<uint64_t, PositionFetch> position_fetches;
std::unordered_map<uint64_t, AlphaSource> alpha_sources;
// Alpha masks by texture and test: descriptor, kOpaqueMask, or kNoAlpha.
std::unordered_map<uint64_t, uint32_t> masks;
std::vector<std::pair<uint32_t, std::vector<uint32_t>>> pending_masks;  // word offset, bits
uint32_t mask_words_used = 0;
std::unordered_map<uint64_t, bool> killing_pixel_shaders;
std::unordered_map<MeshKey, uint32_t, MeshKeyHash> mesh_ids;
std::vector<Mesh> meshes;
std::vector<PendingMesh> pending;
std::vector<Draw> draws;
std::unordered_map<uint64_t, Instance> instances;
std::vector<uint32_t> retired;
uint64_t frame = 0;
uint32_t decoded_this_frame = 0;
bool recording = true;  // off from the depth copy to the scene resolve

// The terrain: every heightmap patch drawn (the level's and the coarser ones
// around it), each as tiles of kTerrainTile cells each way. One slot for all
// of them rebuilt the big one whenever a small one was drawn, so the traced
// world mostly had a patch of distant terrain in it and no island.
constexpr uint32_t kTerrainTile = 128;
// Patches not drawn for this many frames are dropped (another level).
constexpr uint64_t kTerrainKeepFrames = 600;
struct Terrain {
  uint32_t base = 0, width = 0, height = 0, pitch = 0, endian = 0;
  // world x = sx * texel x + ox, z likewise, y = sy * value + oy.
  float sx = 0, ox = 0, sz = 0, oz = 0, sy = 0, oy = 0;
  uint64_t sample = 0;
  uint64_t checked_frame = 0, seen_frame = 0;
  std::vector<uint32_t> tiles;  // mesh ids
};
std::vector<Terrain> terrains;
uint64_t total_triangles = 0;
// Draws left out, by reason (logged with the frame stats).
uint32_t skipped_alpha = 0, skipped_kill = 0, skipped_position = 0,
         skipped_primitive = 0, skipped_decode = 0, skipped_c255 = 0;

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceAddress address = 0;
  uint8_t* mapping = nullptr;
};

struct Vk {
  bool tried = false, ok = false;
  const ui::vulkan::VulkanDevice* device = nullptr;
  PFN_vkCreateAccelerationStructureKHR create = nullptr;
  PFN_vkDestroyAccelerationStructureKHR destroy = nullptr;
  PFN_vkGetAccelerationStructureBuildSizesKHR sizes = nullptr;
  PFN_vkCmdBuildAccelerationStructuresKHR build = nullptr;
  PFN_vkGetAccelerationStructureDeviceAddressKHR as_address = nullptr;
  PFN_vkGetBufferDeviceAddress buffer_address = nullptr;
  Buffer upload, instance_data, scratch, tlas_storage, normals;
  uint32_t normals_used = 0, alpha_used = 0;
  bool normals_full = false, alpha_full = false;
  VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
  VkDeviceSize tlas_scratch = 0;
  std::vector<Buffer> pool;
  VkDeviceSize pool_used = 0;  // in the last chunk
  bool pool_full = false;
  uint64_t slot_submission[kSlots] = {};
  uint32_t next_slot = 0;
  uint32_t last_count = 0;
  // Replaced meshes' structures, freed once the GPU is past the submission.
  std::vector<std::pair<VkAccelerationStructureKHR, uint64_t>> doomed;
} vk;

bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host,
                  Buffer& out) {
  const auto& dfn = vk.device->functions();
  VkDevice d = vk.device->device();
  VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = size;
  info.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (dfn.vkCreateBuffer(d, &info, nullptr, &out.buffer) != VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements requirements;
  dfn.vkGetBufferMemoryRequirements(d, out.buffer, &requirements);
  const auto& types = vk.device->memory_types();
  uint32_t type;
  if (!xe::bit_scan_forward(
          requirements.memoryTypeBits &
              (host ? types.host_visible & types.host_coherent
                    : types.device_local),
          &type)) {
    return false;
  }
  VkMemoryAllocateFlagsInfo flags = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
  flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
  VkMemoryAllocateInfo allocate = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate.pNext = &flags;
  allocate.allocationSize = requirements.size;
  allocate.memoryTypeIndex = type;
  if (dfn.vkAllocateMemory(d, &allocate, nullptr, &out.memory) != VK_SUCCESS ||
      dfn.vkBindBufferMemory(d, out.buffer, out.memory, 0) != VK_SUCCESS) {
    return false;
  }
  if (host) {
    void* mapping;
    if (dfn.vkMapMemory(d, out.memory, 0, VK_WHOLE_SIZE, 0, &mapping) !=
        VK_SUCCESS) {
      return false;
    }
    out.mapping = static_cast<uint8_t*>(mapping);
  }
  VkBufferDeviceAddressInfo address = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  address.buffer = out.buffer;
  out.address = vk.buffer_address(d, &address);
  return true;
}

// Room for a bottom-level structure in the pool.
bool AllocatePool(VkDeviceSize size, VkBuffer& buffer, VkDeviceSize& offset) {
  size = Align(size, 256);
  if (size > kPoolChunk) return false;
  if (vk.pool.empty() || vk.pool_used + size > kPoolChunk) {
    Buffer chunk;
    if (vk.pool.size() >= kMaxPoolChunks ||
        !CreateBuffer(kPoolChunk,
                      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
                      false, chunk)) {
      if (!vk.pool_full) {
        vk.pool_full = true;
        XELOGW("scenefx rt: acceleration structure pool full ({} MB)",
               (vk.pool.size() * kPoolChunk) >> 20);
      }
      return false;
    }
    vk.pool.push_back(chunk);
    vk.pool_used = 0;
  }
  buffer = vk.pool.back().buffer;
  offset = vk.pool_used;
  vk.pool_used += size;
  return true;
}

// The instruction on a disassembly line (after the "/* n */" comment, a "+"
// for a co-issued one and a predicate), or npos.
size_t InstructionAt(const std::string& d, size_t line, size_t end) {
  size_t p = d.find("*/", line);
  p = p == std::string::npos || p > end ? line : p + 2;
  p = d.find_first_not_of(" +", p);
  if (p < end && d[p] == '(') {
    p = d.find(')', p);
    if (p == std::string::npos || p > end) return std::string::npos;
    p = d.find_first_not_of(' ', p + 1);
  }
  return p < end ? p : std::string::npos;
}

// The last ALU instruction before `limit` that writes any of the components
// (bits) of register r (predicated ones too unless skip_predicated: those
// collapse hidden parts' vertices), or npos.
size_t LastAluWrite(const std::string& d, uint32_t r, size_t limit, bool skip_predicated = false,
                    uint32_t components = 0xF) {
  std::string reg = "r" + std::to_string(r);
  size_t last = std::string::npos;
  for (size_t line = 0; line < limit;) {
    size_t end = std::min(d.find(char(10), line), d.size());
    size_t op = InstructionAt(d, line, end);
    if (op != std::string::npos && d.compare(op, 6, "vfetch") && d.compare(op, 6, "tfetch") &&
        !(skip_predicated && d.find('(', line) < op)) {
      size_t dest = d.find(' ', op);
      size_t after = dest + 1 + reg.size();
      if (dest < end && !d.compare(dest + 1, reg.size(), reg) &&
          (d[after] == '.' || d[after] == ',')) {
        // Its write mask (`rN.x_z_`), or all of it (`rN,`).
        uint32_t written = 0xF;
        if (d[after] == '.') {
          written = 0;
          for (int i = 0; i < 4 && after + 1 + i < end; ++i) {
            written |= uint32_t(d[after + 1 + i] != '_') << i;
          }
        }
        if (written & components) last = op;
      }
    }
    line = end + 1;
  }
  return last;
}

bool AluWrites(const std::string& d, uint32_t r, size_t limit, bool skip_predicated = false,
               uint32_t components = 0xF) {
  return LastAluWrite(d, r, limit, skip_predicated, components) != std::string::npos;
}

int SwizzleIndex(char c) {
  switch (c) {
    case 'x': return 0;
    case 'y': return 1;
    case 'z': return 2;
    case 'w': return 3;
    default: return -1;
  }
}

// The position fetch of a world-space vertex shader: clip = c12-c15 * p and
// world = c16-c18 * p where the shader has it (or, in the foliage shaders,
// clip = c16-c19 * p and world = c20-c22 * p), for p = (x, y, z, 1) from a
// vertex fetch, either dotted directly (`dp4 oPos.x___, c12.zxyw, rN.zxyw`,
// rN fetched as xyz1), after `cndeq rN, c255.xxxy, rM.abcc, c255.yyyy` (rN =
// (rM.a, rM.b, rM.c, 1), which `dp4 oPos.x___, c12.zxyw, rN` takes as (b, c,
// a)), or plus a wind offset (`mad rN.xyz_, ..., ..., rM.xyzz`, left out); the
// clip position goes to oPos straight or through a temporary (`dp4
// rT.x___, c12.zxyw, ...` and `max oPos, rT, rT`). Rigid multi-part models
// (vehicles: one matrix c[17+a0] per vertex, a0 from a byte of it) are taken
// part by part; skinned ones (blended bones) are left out. a0 that only reads
// a part's visibility flag (`setp_gt r0._, c[21+a0].x`, destructible parts
// and plants) is fine.
const PositionFetch& GetPositionFetch(const Shader& shader) {
  auto [it, added] = position_fetches.try_emplace(shader.ucode_data_hash());
  PositionFetch& pf = it->second;
  if (!added) return pf;
  const std::string& d = shader.ucode_disassembly();
  auto reject = [&](const char* why) -> const PositionFetch& {
    XELOGI("scenefx rt: vertex shader {:016X} left out: {} ({} chars)",
           shader.ucode_data_hash(), why, d.size());
    return pf;
  };
  // The clip position's first row, from c12 or c16.
  std::string tag;
  size_t at = std::string::npos;
  for (uint32_t clip : {12u, 16u}) {
    std::string c = ".x___, c" + std::to_string(clip) + ".zxyw, r";
    tag = "dp4 oPos" + c;
    at = d.find(tag);
    if (at != std::string::npos) {
      pf.clip_constant = clip;
      break;
    }
    // Through a temporary.
    static const char kTemporary[] = "dp4 r";
    for (size_t from = 0; (from = d.find(kTemporary, from)) != std::string::npos; ++from) {
      char* temp_end;
      uint32_t temp = uint32_t(std::strtoul(d.c_str() + from + sizeof(kTemporary) - 1, &temp_end, 10));
      std::string moved = "max oPos, r" + std::to_string(temp) + ", r" + std::to_string(temp);
      if (temp_end != d.c_str() + from + sizeof(kTemporary) - 1 &&
          !std::strncmp(temp_end, c.c_str(), c.size()) && d.find(moved) != std::string::npos) {
        at = from;
        tag = "dp4 r" + std::to_string(temp) + c;
        break;
      }
    }
    if (at != std::string::npos) {
      pf.clip_constant = clip;
      break;
    }
  }
  if (at == std::string::npos) return reject("no c12 or c16 position");
  pf.world_constant = pf.clip_constant + 4;
  bool parts = false;
  for (size_t a0 = 0; (a0 = d.find("+a0]", a0)) != std::string::npos; ++a0) {
    size_t line = d.rfind(char(10), a0);
    line = line == std::string::npos ? 0 : line + 1;
    size_t op = InstructionAt(d, line, a0);
    if (op == std::string::npos || d.compare(op, 5, "setp_")) parts = true;
  }
  char* end;
  uint32_t reg = uint32_t(std::strtoul(d.c_str() + at + tag.size(), &end, 10));
  uint32_t part_reg = UINT32_MAX;
  int part_reg_component = -1;
  if (parts) {
    // One matrix per vertex (`maxas r0._, rX.cc` once, then `dp4
    // rW.x___, c[17+a0].zxyw, rP` for the position c12-c15 takes); several
    // (blended bones, soldiers) are left out.
    size_t maxas = d.find("maxas r0._, r");
    bool one = maxas != std::string::npos;
    for (size_t m = maxas; one && (m = d.find("maxas", m + 5)) != std::string::npos;) {
      one = !d.compare(m, 17, d, maxas, 17);
    }
    if (!one || pf.clip_constant != 12 || std::strncmp(end, ".zxyw", 5)) {
      return reject("skinned");
    }
    char* maxas_end;
    part_reg = uint32_t(std::strtoul(d.c_str() + maxas + 13, &maxas_end, 10));
    part_reg_component = maxas_end[0] == '.' ? SwizzleIndex(maxas_end[1]) : -1;
    tag = "dp4 r" + std::to_string(reg) + ".x___, c[17+a0].zxyw, r";
    at = d.find(tag);
    if (at == std::string::npos || part_reg_component < 0) return reject("skinned");
    reg = uint32_t(std::strtoul(d.c_str() + at + tag.size(), &end, 10));
    pf.part_base = 17;
    pf.part_c255 = d.find(", c255.z, r" + std::to_string(part_reg)) != std::string::npos;
  }
  auto world_row = [&](uint32_t row) {
    return d.find("c" + std::to_string(pf.world_constant + row) + ".zxyw, r") != std::string::npos;
  };
  pf.world_from_view = !parts && (!world_row(0) || !world_row(1) || !world_row(2));
  if (pf.world_from_view && pf.clip_constant != 12) return reject("no c20-c22 world matrix");
  // BF_RT_NO_VIEW_WORLD leaves those out.
  if (pf.world_from_view && std::getenv("BF_RT_NO_VIEW_WORLD")) {
    return reject("no c16-c18 world matrix");
  }
  bool direct = !std::strncmp(end, ".zxyw", 5);
  bool needs_w = direct;
  // Plants: the fetched position plus a wind offset.
  size_t wind = d.rfind("mad r" + std::to_string(reg) + ".xyz_, ", at);
  if (direct && wind != std::string::npos) {
    size_t line_end = std::min(d.find(char(10), wind), d.size());
    size_t source = d.rfind(", r", line_end);
    if (source == std::string::npos || source < wind) return reject("other wind offset");
    reg = uint32_t(std::strtoul(d.c_str() + source + 3, &end, 10));
    if (std::strncmp(end, ".xyzz", 5)) return reject("other wind offset");
    needs_w = false;
  } else if (!direct) {
    if (*end != '\r' && *end != '\n') return reject("swizzled position");
    std::string select = "cndeq r" + std::to_string(reg) + ", c255.xxxy, r";
    size_t s = d.find(select);
    if (s == std::string::npos) return reject("no cndeq for the position");
    reg = uint32_t(std::strtoul(d.c_str() + s + select.size(), &end, 10));
    if (!std::strncmp(end, ".wyxx, c255.yyyy", 16)) {
      // Terrain: (x, y + cN.x, z) from r.y, r.x = cN.x + r.z, r.w.
      std::string height = "addsc r" + std::to_string(reg) + ".x___, c";
      size_t h = d.find(height);
      char* h_end;
      uint32_t c = h == std::string::npos ? UINT32_MAX
                   : uint32_t(std::strtoul(d.c_str() + h + height.size(), &h_end, 10));
      if (c == UINT32_MAX ||
          std::strncmp(h_end, (".x, r" + std::to_string(reg) + ".z").c_str(),
                       5 + std::to_string(reg).size() + 2)) {
        return reject("other cndeq");
      }
      pf.height_constant = c;
      pf.reg_components[0] = 1;
      pf.reg_components[1] = 2;
      pf.reg_components[2] = 3;
    } else {
      // (a, b, c) taken as (b, c, a), from the fetch as it is.
      int a = SwizzleIndex(end[1]), b = SwizzleIndex(end[2]), c = SwizzleIndex(end[3]);
      // Nothing may change the source before the select (which itself may
      // write it: `cndeq r1, c255.xxxy, r1.zxyy, c255.yyyy`, the destructible
      // buildings).
      size_t select_line = d.rfind(char(10), s);
      select_line = select_line == std::string::npos ? 0 : select_line;
      if (end[0] != '.' || a < 0 || b < 0 || c < 0 || std::strncmp(end + 5, ", c255.yyyy", 11) ||
          AluWrites(d, reg, select_line)) {
        return reject("other cndeq");
      }
      pf.reg_components[0] = uint8_t(b);
      pf.reg_components[1] = uint8_t(c);
      pf.reg_components[2] = uint8_t(a);
    }
    pf.needs_c255 = true;
  }
  for (const auto& binding : shader.vertex_bindings()) {
    for (const auto& attribute : binding.attributes) {
      const auto& result = attribute.fetch_instr.result;
      const auto& a = attribute.fetch_instr.attributes;
      uint32_t mask = (1u << pf.reg_components[0]) | (1u << pf.reg_components[1]) |
                      (1u << pf.reg_components[2]) | (needs_w ? 8u : 0u);
      if (result.storage_target != InstructionStorageTarget::kRegister ||
          result.storage_index != reg ||
          (result.original_write_mask & mask) != mask ||
          (needs_w && result.components[3] != SwizzleSource::k1) ||
          !FormatWords(a.data_format)) {
        continue;
      }
      // Each of x, y, z from a component the format has.
      bool components_ok = true;
      uint32_t format_components = xenos::GetVertexFormatComponentCount(a.data_format);
      for (int i = 0; i < 3; ++i) {
        uint32_t c = uint32_t(result.components[pf.reg_components[i]]);
        components_ok &= c < format_components;
        pf.components[i] = uint8_t(c);
      }
      if (!components_ok) continue;
      pf.ok = true;
      pf.fetch_constant = binding.fetch_constant;
      pf.stride = binding.stride_words;
      pf.offset = a.offset;
      pf.format = a.data_format;
      break;
    }
    if (pf.ok) break;
  }
  if (pf.ok && parts) {
    // The byte: an 8_8_8_8 attribute of the same vertex written into one
    // component of the register maxas reads (directly, or truncated and
    // times c255.z into another of its components), or of the one truncated
    // into it (`truncs rA.x___, rB.x`).
    uint32_t fetched_reg = part_reg;
    std::string truncs = "truncs r" + std::to_string(part_reg) + ".";
    size_t t = d.find(truncs);
    if (t != std::string::npos) {
      size_t source = d.find(", r", t);
      if (source != std::string::npos && source < d.find(char(10), t)) {
        fetched_reg = uint32_t(std::strtoul(d.c_str() + source + 3, nullptr, 10));
      }
    }
    bool found = false;
    for (const auto& binding : shader.vertex_bindings()) {
      if (binding.fetch_constant != pf.fetch_constant) continue;
      for (const auto& attribute : binding.attributes) {
        const auto& result = attribute.fetch_instr.result;
        const auto& a = attribute.fetch_instr.attributes;
        uint32_t written = result.original_write_mask;
        if (a.data_format != xenos::VertexFormat::k_8_8_8_8 ||
            result.storage_target != InstructionStorageTarget::kRegister ||
            (result.storage_index != part_reg && result.storage_index != fetched_reg) ||
            !written || (written & (written - 1))) {
          continue;
        }
        int component = 0;
        while (!(written & (1u << component))) ++component;
        uint32_t source = uint32_t(result.components[component]);
        if (source > 3) continue;
        pf.part_offset = a.offset;
        pf.part_component = uint8_t(source);
        found = true;
        break;
      }
      if (found) break;
    }
    if (!found) {
      pf.ok = false;
      return reject("no part index");
    }
  }
  XELOGI("scenefx rt: vertex shader {:016X}: position in r{}{}, clip c{}, {}",
         shader.ucode_data_hash(), reg,
         pf.world_from_view ? " (world matrix from the view)" : "", pf.clip_constant,
         pf.ok ? fmt::format("format {} components {}{}{}", uint32_t(pf.format),
                             pf.components[0], pf.components[1], pf.components[2])
               : std::string("not supported"));
  return pf;
}

// An alpha-tested draw's texture coordinates and alpha texture (see
// AlphaSource).
const AlphaSource& GetAlphaSource(const Shader& vertex_shader, const Shader& pixel_shader) {
  auto [it, added] = alpha_sources.try_emplace(vertex_shader.ucode_data_hash() * 31 ^
                                               pixel_shader.ucode_data_hash());
  AlphaSource& as = it->second;
  if (!added) return as;
  auto reject = [&](const char* why) -> const AlphaSource& {
    XELOGI("scenefx rt: alpha-tested shaders {:016X} {:016X} left out: {}",
           vertex_shader.ucode_data_hash(), pixel_shader.ucode_data_hash(), why);
    return as;
  };
  // The pixel shader's first fetch writing w (all four components, or one
  // into w) from two components of an untouched interpolator, past the
  // shadow map's point-sampled ones. (Alpha written as the constant 1 is
  // left out too: those are the ground decals, which traced shadowed the
  // ground they lie on.)
  const std::string& p = pixel_shader.ucode_disassembly();
  static const char kFetch[] = "tfetch2D r";
  uint32_t interpolator = UINT32_MAX;
  int coords[2] = {};
  for (size_t f = 0; (f = p.find(kFetch, f)) != std::string::npos; ++f) {
    size_t line_end = std::min(p.find(char(10), f), p.size());
    if (p.find("Offset", f) < line_end || p.find("=point", f) < line_end) continue;
    char* e;
    std::strtoul(p.c_str() + f + sizeof(kFetch) - 1, &e, 10);
    int texel = 3;  // the texel component written to w
    if (!std::strncmp(e, ".___", 4)) {
      texel = SwizzleIndex(e[4]);
      e += 5;
    } else if (std::getenv("BF_RT_GRASS") && e[0] == '.' && SwizzleIndex(e[1]) >= 0 &&
               SwizzleIndex(e[2]) >= 0 && SwizzleIndex(e[3]) >= 0 && SwizzleIndex(e[4]) >= 0) {
      // All four written in another order (`tfetch2D r1.yxzw`): w gets the
      // texel's component named last. Only with BF_RT_GRASS: that is the
      // grass, thousands of alpha-tested blades that took the frame rate
      // below 5.
      texel = SwizzleIndex(e[4]);
      e += 5;
    }
    if (texel < 0 || std::strncmp(e, ", r", 3)) continue;
    uint32_t r = uint32_t(std::strtoul(e + 3, &e, 10));
    int c0 = SwizzleIndex(e[1]), c1 = SwizzleIndex(e[2]);
    if (e[0] != '.' || c0 < 0 || c1 < 0 || std::strncmp(e + 3, ", tf", 4)) continue;
    uint32_t tf = uint32_t(std::strtoul(e + 7, &e, 10));
    if (r >= 16) continue;
    if (AluWrites(p, r, f, false, 1u << c0 | 1u << c1)) {
      // Each coordinate a copy (`maxs rR.x___, rS.yy`) or a double (`adds`)
      // of one component of the same untouched interpolator.
      uint32_t from = UINT32_MAX;
      int components[2] = {-1, -1};
      bool doubled[2] = {};
      for (int k = 0; k < 2; ++k) {
        size_t op = LastAluWrite(p, r, f, false, 1u << (k ? c1 : c0));
        if (op == std::string::npos || (p.compare(op, 5, "adds ") && p.compare(op, 5, "maxs ")) ||
            p.find('(', p.rfind(char(10), op)) < op) {
          break;
        }
        doubled[k] = !p.compare(op, 5, "adds ");
        size_t s = p.find(", r", op);
        char* se;
        uint32_t source = uint32_t(std::strtoul(p.c_str() + s + 3, &se, 10));
        int m = SwizzleIndex(se[1]);
        if (se[0] != '.' || m < 0 || se[2] != se[1] || (from != UINT32_MAX && source != from) ||
            source >= 16 || AluWrites(p, source, op, false, 1u << m)) {
          break;
        }
        from = source;
        components[k] = m;
      }
      if (components[0] < 0 || components[1] < 0 || doubled[0] != doubled[1]) continue;
      r = from;
      c0 = components[0];
      c1 = components[1];
      as.doubled = doubled[0];
    }
    interpolator = r;
    coords[0] = c0;
    coords[1] = c1;
    as.texture = tf;
    as.texel_component = uint8_t(texel);
    break;
  }
  if (interpolator == UINT32_MAX) return reject("no texture fetch from an interpolator");
  // The vertex shader's copy of a register to it (`max oN.mask, rR.swizzle,
  // rR.swizzle`, or whole).
  const std::string& v = vertex_shader.ucode_disassembly();
  std::string copy = "max o" + std::to_string(interpolator);
  size_t c = v.find(copy + ".");
  if (c == std::string::npos) c = v.find(copy + ", ");
  if (c == std::string::npos) return reject("no interpolator copy");
  bool whole = v[c + copy.size()] == ',';
  const char* mask_letters = v.c_str() + c + copy.size() + 1;
  size_t source = v.find(", r", c);
  char* e;
  uint32_t reg = uint32_t(std::strtoul(v.c_str() + source + 3, &e, 10));
  int s[2];
  for (int k = 0; k < 2; ++k) {
    s[k] = whole ? coords[k]
                 : SwizzleIndex(mask_letters[coords[k]]) < 0 ? -1 : SwizzleIndex(e[1 + coords[k]]);
  }
  size_t swizzle_size = whole ? 0 : 5;
  std::string again = std::string(", r") + std::to_string(reg) + std::string(e, swizzle_size);
  if ((!whole && e[0] != '.') || s[0] < 0 || s[1] < 0 ||
      std::strncmp(e + swizzle_size, again.c_str(), again.size()) ||
      AluWrites(v, reg, c, true, 1u << s[0] | 1u << s[1])) {
    return reject("texture coordinates not copied from a fetch");
  }
  int s0 = s[0], s1 = s[1];
  for (const auto& binding : vertex_shader.vertex_bindings()) {
    for (const auto& attribute : binding.attributes) {
      const auto& result = attribute.fetch_instr.result;
      const auto& a = attribute.fetch_instr.attributes;
      uint32_t mask = (1u << s0) | (1u << s1);
      if (result.storage_target != InstructionStorageTarget::kRegister ||
          result.storage_index != reg || (result.original_write_mask & mask) != mask ||
          !FormatWords(a.data_format)) {
        continue;
      }
      uint32_t u = uint32_t(result.components[s0]), w = uint32_t(result.components[s1]);
      uint32_t count = xenos::GetVertexFormatComponentCount(a.data_format);
      if (u >= count || w >= count) continue;
      as.ok = true;
      as.fetch_constant = binding.fetch_constant;
      as.offset = a.offset;
      as.format = a.data_format;
      as.components[0] = uint8_t(u);
      as.components[1] = uint8_t(w);
    }
  }
  if (!as.ok) return reject("texture coordinates in another format");
  XELOGI("scenefx rt: alpha-tested shaders {:016X} {:016X}: tf{}, coordinates from offset {} "
         "format {} components {}{}{}",
         vertex_shader.ucode_data_hash(), pixel_shader.ucode_data_hash(), as.texture, as.offset,
         uint32_t(as.format), as.components[0], as.components[1], as.doubled ? ", doubled" : "");
  return as;
}

// The alpha of texel (x, y) of a block, 0-255.
uint32_t BlockAlpha(xenos::TextureFormat format, const uint8_t* b, uint32_t x, uint32_t y,
                    uint32_t component) {
  uint32_t i = y * 4 + x;
  switch (format) {
    case xenos::TextureFormat::k_DXT1: {
      uint32_t c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
      uint32_t index = (Load32(b + 4) >> (2 * i)) & 3;
      return c0 <= c1 && index == 3 ? 0 : 255;
    }
    case xenos::TextureFormat::k_DXT2_3:
    case xenos::TextureFormat::k_DXT3A: {
      uint32_t nibble = (b[i >> 1] >> ((i & 1) * 4)) & 15;
      return nibble * 17;
    }
    case xenos::TextureFormat::k_DXT4_5:
    case xenos::TextureFormat::k_DXT5A: {
      uint32_t a0 = b[0], a1 = b[1];
      uint64_t bits = 0;
      for (int k = 0; k < 6; ++k) bits |= uint64_t(b[2 + k]) << (8 * k);
      uint32_t index = uint32_t(bits >> (3 * i)) & 7;
      if (index == 0) return a0;
      if (index == 1) return a1;
      if (a0 > a1) return ((8 - index) * a0 + (index - 1) * a1) / 7;
      if (index == 6) return 0;
      if (index == 7) return 255;
      return ((6 - index) * a0 + (index - 1) * a1) / 5;
    }
    default:  // k_8_8_8_8, k_8
      return b[component];
  }
}

// A mask of where the alpha of the 2D texture in fetch constant `texture`
// passes the draw's alpha test, at most 256x256: its descriptor (see
// kAlphaBase), kOpaqueMask, or kNoAlpha if it can't be read.
uint32_t GetMask(const RegisterFile& regs, uint32_t texture, uint32_t texel_component,
                 Memory& memory) {
  xenos::xe_gpu_texture_fetch_t tf;
  std::memcpy(&tf, &regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + texture * 6], sizeof(tf));
  auto colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  float ref = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  uint32_t func = uint32_t(colorcontrol.alpha_func);
  if (!colorcontrol.alpha_test_enable) {
    // Alpha to coverage only: half covered.
    ref = 0.5f;
    func = uint32_t(xenos::CompareFunction::kGreaterEqual);
  }
  uint32_t w1, h1, d1, base_page, mip_page, mip_min, mip_max;
  texture_util::GetSubresourcesFromFetchConstant(tf, &w1, &h1, &d1, &base_page, &mip_page,
                                                 &mip_min, &mip_max);
  if (tf.type != xenos::FetchConstantType::kTexture ||
      tf.dimension != xenos::DataDimension::k2DOrStacked || !base_page) {
    XELOGI("scenefx rt: alpha texture tf{} not a 2D texture", texture);
    return kNoAlpha;
  }
  const uint8_t* data = memory.TranslatePhysical(base_page << 12);
  uint64_t key = Fnv(&tf, sizeof(tf), Fnv(&ref, 4, Fnv(&func, 4, Fnv(&texel_component, 4))));
  key = Fnv(data, 256, key);
  auto [it, added] = masks.try_emplace(key, kNoAlpha);
  if (!added) return it->second;
  // The texture component read as alpha.
  uint32_t alpha = (tf.swizzle >> (3 * texel_component)) & 7;
  bool dxt = tf.format == xenos::TextureFormat::k_DXT1 ||
             tf.format == xenos::TextureFormat::k_DXT2_3 ||
             tf.format == xenos::TextureFormat::k_DXT4_5;
  // Single-channel ones (alpha from x).
  bool single = tf.format == xenos::TextureFormat::k_DXT3A ||
                tf.format == xenos::TextureFormat::k_DXT5A || tf.format == xenos::TextureFormat::k_8;
  if (alpha == 5) {  // one
    XELOGI("scenefx rt: alpha texture format {} is opaque", uint32_t(tf.format));
    return it->second = kOpaqueMask;
  }
  if (!(dxt && alpha == 3) && !(single && alpha == 0) &&
      !(tf.format == xenos::TextureFormat::k_8_8_8_8 && alpha < 4)) {
    XELOGI("scenefx rt: alpha texture format {} (alpha from {}) not read", uint32_t(tf.format),
           alpha);
    return kNoAlpha;
  }
  const FormatInfo* info = FormatInfo::Get(tf.format);
  uint32_t width = w1 + 1, height = h1 + 1;
  uint32_t block_bytes = info->bytes_per_block();
  texture_util::TextureGuestLayout layout = texture_util::GetGuestTextureLayout(
      tf.dimension, tf.pitch, width, height, 1, tf.tiled, tf.format, tf.packed_mips, true, 0);
  uint32_t pitch_blocks = layout.base.row_pitch_bytes / block_bytes;
  uint32_t lw = std::min(8u, xe::log2_floor(width)), lh = std::min(8u, xe::log2_floor(height));
  uint32_t mw = 1u << lw, mh = 1u << lh;
  uint32_t words = std::max(1u, mw * mh / 32);
  if (mask_words_used + words > kMaxMaskWords) return kNoAlpha;
  std::vector<uint32_t> bits(words);
  uint32_t passed = 0;
  for (uint32_t my = 0; my < mh; ++my) {
    for (uint32_t mx = 0; mx < mw; ++mx) {
      uint32_t x = (mx * 2 + 1) * width / (mw * 2), y = (my * 2 + 1) * height / (mh * 2);
      uint32_t bx = x / info->block_width, by = y / info->block_height;
      uint32_t offset = tf.tiled ? uint32_t(texture_address::Tiled2D(
                                       int32_t(bx), int32_t(by), pitch_blocks,
                                       xe::log2_floor(block_bytes)))
                                 : by * layout.base.row_pitch_bytes + bx * block_bytes;
      uint8_t block[16];
      for (uint32_t k = 0; k < block_bytes; k += 4) {
        uint32_t word = Swap(Load32(data + offset + k), uint32_t(tf.endianness));
        std::memcpy(block + k, &word, 4);
      }
      float a = BlockAlpha(tf.format, block, x % info->block_width, y % info->block_height,
                           alpha) / 255.0f;
      bool pass = (a < ref && (func & 1)) || (a == ref && (func & 2)) || (a > ref && (func & 4));
      if (pass) {
        uint32_t bit = my * mw + mx;
        bits[bit >> 5] |= 1u << (bit & 31);
        ++passed;
      }
    }
  }
  if (passed == mw * mh) return it->second = kOpaqueMask;
  if (!passed) {
    XELOGI("scenefx rt: alpha texture format {} fails the test (alpha {} {}) everywhere",
           uint32_t(tf.format), func, ref);
    return kNoAlpha;
  }
  uint32_t descriptor = mask_words_used | lw << 24 | lh << 28;
  pending_masks.emplace_back(mask_words_used, std::move(bits));
  mask_words_used += words;
  XELOGI("scenefx rt: alpha mask {}x{} from a {}x{} texture (format {}{}), {}% covered",
         mw, mh, width, height, uint32_t(tf.format), tf.tiled ? ", tiled" : "",
         passed * 100 / (mw * mh));
  return it->second = descriptor;
}

bool KillsPixels(const Shader& shader) {
  auto [it, added] = killing_pixel_shaders.try_emplace(shader.ucode_data_hash(), false);
  if (added) {
    it->second = shader.ucode_disassembly().find("kill") != std::string::npos;
  }
  return it->second;
}

uint64_t SampleMesh(const MeshKey& key, Memory& memory) {
  uint64_t h = Fnv(&key, sizeof(key));
  if (!(key.index_info & (1u << 12))) {
    const uint8_t* indices = memory.TranslatePhysical(key.index_base & 0x1FFFFFFF);
    uint32_t bytes = key.index_count * ((key.index_info & 1) ? 4 : 2);
    h = Fnv(indices, std::min(bytes, 64u), h);
    if (bytes > 64) h = Fnv(indices + bytes - 64, 64, h);
  }
  const uint8_t* vertices = memory.TranslatePhysical(key.vertex_base * 4);
  uint32_t bytes = key.vertex_size * 4;
  h = Fnv(vertices, std::min(bytes, 64u), h);
  if (bytes > 128) h = Fnv(vertices + (bytes / 8) * 4, 64, h);
  return h;
}

// A multi-part model's vertex's part byte.
uint32_t PartByte(const MeshKey& key, const uint8_t* vertex_data, uint32_t vertex) {
  int64_t word = int64_t(vertex) * key.stride + int32_t(key.part_offset);
  if (word < 0 || word >= int64_t(key.vertex_size)) return UINT32_MAX;
  uint32_t v = Swap(Load32(vertex_data + word * 4), key.vertex_endian);
  return (v >> (8 * ((key.part >> 1) & 3))) & 0xFF;
}

// The part bytes a multi-part draw's vertices have (its key without a part).
const std::vector<uint8_t>& PartBytes(const MeshKey& key, uint64_t sample, Memory& memory) {
  static std::unordered_map<MeshKey, std::pair<uint64_t, std::vector<uint8_t>>, MeshKeyHash> known;
  auto [it, added] = known.try_emplace(key);
  if (!added && it->second.first == sample) return it->second.second;
  if (known.size() > 20000) {
    known.clear();
    it = known.try_emplace(key).first;
  }
  it->second.first = sample;
  std::vector<uint8_t>& out = it->second.second;
  out.clear();
  bool auto_index = key.index_info & (1u << 12);
  bool is32 = key.index_info & 1;
  uint32_t index_endian = (key.index_info >> 1) & 3;
  const uint8_t* index_data =
      auto_index ? nullptr : memory.TranslatePhysical(key.index_base & 0x1FFFFFFF);
  const uint8_t* vertex_data = memory.TranslatePhysical(key.vertex_base * 4);
  bool seen[256] = {};
  for (uint32_t i = 0; i < key.index_count; ++i) {
    uint32_t v;
    if (auto_index) {
      v = i;
    } else if (is32) {
      v = Swap(Load32(index_data + i * 4), index_endian) & 0xFFFFFF;
    } else {
      uint32_t dword = Swap(Load32(index_data + (i >> 1) * 4), index_endian);
      v = (i & 1) ? dword >> 16 : dword & 0xFFFF;
    }
    uint32_t b = PartByte(key, vertex_data, (v + key.index_offset) & 0xFFFFFF);
    if (b < 256 && !seen[b]) {
      seen[b] = true;
      out.push_back(uint8_t(b));
    }
  }
  return out;
}

bool DecodeMesh(const MeshKey& key, const PositionFetch& pf, Memory& memory,
                PendingMesh& out, Mesh& mesh) {
  bool auto_index = key.index_info & (1u << 12);
  bool is32 = key.index_info & 1;
  uint32_t index_endian = (key.index_info >> 1) & 3;
  const uint8_t* index_data =
      auto_index ? nullptr : memory.TranslatePhysical(key.index_base & 0x1FFFFFFF);
  bool strip = ((key.index_info >> 4) & 63) == uint32_t(xenos::PrimitiveType::kTriangleStrip);
  bool restart = key.index_info & (1u << 13);
  uint32_t reset = is32 ? key.reset_index & 0xFFFFFF : key.reset_index & 0xFFFF;
  // Raw index i, or UINT32_MAX for a strip restart.
  auto index_at = [&](uint32_t i) -> uint32_t {
    uint32_t v;
    if (auto_index) {
      v = i;
    } else if (is32) {
      v = Swap(Load32(index_data + i * 4), index_endian) & 0xFFFFFF;
    } else {
      uint32_t dword = Swap(Load32(index_data + (i >> 1) * 4), index_endian);
      v = (i & 1) ? dword >> 16 : dword & 0xFFFF;
    }
    if (restart && v == reset) return UINT32_MAX;
    return (v + key.index_offset) & 0xFFFFFF;
  };
  std::vector<uint32_t>& tris = out.indices;
  auto add = [&](uint32_t a, uint32_t b, uint32_t c) {
    if (a == b || b == c || a == c) return;
    tris.push_back(a);
    tris.push_back(b);
    tris.push_back(c);
  };
  if (strip) {
    uint32_t window[2];
    uint32_t have = 0, parity = 0;
    for (uint32_t i = 0; i < key.index_count; ++i) {
      uint32_t v = index_at(i);
      if (v == UINT32_MAX) {
        have = parity = 0;
        continue;
      }
      if (have < 2) {
        window[have++] = v;
        continue;
      }
      if (parity) {
        add(window[1], window[0], v);
      } else {
        add(window[0], window[1], v);
      }
      parity ^= 1;
      window[0] = window[1];
      window[1] = v;
    }
  } else {
    for (uint32_t i = 0; i + 2 < key.index_count; i += 3) {
      uint32_t a = index_at(i), b = index_at(i + 1), c = index_at(i + 2);
      if (a == UINT32_MAX || b == UINT32_MAX || c == UINT32_MAX) continue;
      add(a, b, c);
    }
  }
  const uint8_t* vertex_data = memory.TranslatePhysical(key.vertex_base * 4);
  if (key.part) {
    std::vector<uint32_t> kept;
    for (size_t t = 0; t + 2 < tris.size(); t += 3) {
      if (PartByte(key, vertex_data, tris[t]) == ((key.part >> 8) & 0xFF)) {
        kept.insert(kept.end(), tris.begin() + t, tris.begin() + t + 3);
      }
    }
    tris = std::move(kept);
  }
  if (tris.empty()) return false;
  uint32_t lowest = *std::min_element(tris.begin(), tris.end());
  uint32_t highest = *std::max_element(tris.begin(), tris.end());
  uint32_t count = highest - lowest + 1;
  if (count > kMaxMeshVertices) return false;
  for (uint32_t& i : tris) i -= lowest;

  // A vertex's attribute at offset (dwords) in format.
  auto fetch = [&](uint32_t vertex, uint32_t offset, xenos::VertexFormat format, float c[4]) {
    uint32_t words[4] = {};
    for (uint32_t k = 0; k < FormatWords(format); ++k) {
      int64_t word = int64_t(vertex) * key.stride + int32_t(offset) + k;
      if (word >= 0 && word < int64_t(key.vertex_size)) {
        words[k] = Swap(Load32(vertex_data + word * 4), key.vertex_endian);
      }
    }
    if (format == xenos::VertexFormat::k_16_16_FLOAT ||
        format == xenos::VertexFormat::k_16_16_16_16_FLOAT) {
      c[0] = Half(words[0] & 0xFFFF);
      c[1] = Half(words[0] >> 16);
      c[2] = Half(words[1] & 0xFFFF);
      c[3] = Half(words[1] >> 16);
    } else {
      std::memcpy(c, words, 4 * sizeof(float));
    }
  };
  out.vertices.resize(size_t(count) * 3);
  if (key.uv_format) {
    out.uvs.resize(size_t(count) * 2);
    out.mask = key.mask;
  }
  for (int i = 0; i < 3; ++i) {
    mesh.bmin[i] = INFINITY;
    mesh.bmax[i] = -INFINITY;
  }
  for (uint32_t v = 0; v < count; ++v) {
    float c[4];
    if (key.uv_format) {
      fetch(lowest + v, key.uv_offset, xenos::VertexFormat(key.uv_format), c);
      float scale = (key.uv_components >> 16) & 1 ? 2.0f : 1.0f;
      out.uvs[size_t(v) * 2] = c[key.uv_components & 3] * scale;
      out.uvs[size_t(v) * 2 + 1] = c[(key.uv_components >> 8) & 3] * scale;
    }
    fetch(lowest + v, key.offset, pf.format, c);
    float* p = &out.vertices[size_t(v) * 3];
    bool finite = true;
    for (int i = 0; i < 3; ++i) {
      p[i] = c[pf.components[i]];
      finite &= std::isfinite(p[i]) && std::abs(p[i]) < 1e6f;
    }
    if (!finite) {
      // An inactive vertex: its triangles are left out of the structure.
      p[0] = NAN;
      continue;
    }
    for (int i = 0; i < 3; ++i) {
      mesh.bmin[i] = std::min(mesh.bmin[i], p[i]);
      mesh.bmax[i] = std::max(mesh.bmax[i], p[i]);
    }
  }
  if (!(mesh.bmin[0] <= mesh.bmax[0])) return false;
  mesh.triangles = uint32_t(tris.size() / 3);
  return true;
}

void Retire(uint32_t id);
void AddDraw(const MeshKey& key, const PositionFetch& pf, const RegisterFile& regs,
             const float* world, const void* depth_target, Memory& memory);

}  // namespace

bool IsTerrainVertexShader(const Shader& vertex_shader) {
  static std::unordered_map<uint64_t, bool> known;
  auto [it, added] = known.try_emplace(vertex_shader.ucode_data_hash(), false);
  if (added) {
    const std::string& d = vertex_shader.ucode_disassembly();
    it->second = d.find("DataFormat=FMT_16_16,") != std::string::npos &&
                 d.find("tfetch2D") != std::string::npos &&
                 d.find("c22.xyyy, c22.zwww") != std::string::npos &&
                 d.find(", c20.xyzz, c21.xyzz") != std::string::npos;
  }
  return it->second;
}

namespace {
uint64_t SampleTerrain(const Terrain& terrain, Memory& memory) {
  const uint8_t* data = memory.TranslatePhysical(terrain.base);
  uint64_t h = 14695981039346656037ull;
  for (uint32_t y = 0; y < terrain.height; y += 64) {
    h = Fnv(data + size_t(y) * terrain.pitch * 2, terrain.width * 2, h);
  }
  return h;
}

// Builds the terrain's tiles: every step-th texel, triangles facing up.
void BuildTerrain(Terrain& terrain, Memory& memory) {
  for (uint32_t id : terrain.tiles) Retire(id);
  terrain.tiles.clear();
  static const uint32_t step = [] {
    const char* e = std::getenv("BF_RT_TERRAIN_STEP");
    return e ? std::clamp(uint32_t(std::atoi(e)), 1u, 16u) : 2u;
  }();
  static const bool flip = std::getenv("BF_RT_TERRAIN_FLIP") != nullptr;
  const uint8_t* data = memory.TranslatePhysical(terrain.base);
  auto value = [&](uint32_t x, uint32_t y) {
    x = std::min(x, terrain.width - 1);
    y = std::min(y, terrain.height - 1);
    const uint8_t* p = data + (size_t(y) * terrain.pitch + x) * 2;
    uint32_t v = terrain.endian == 1 || terrain.endian == 2 ? uint32_t(p[0]) << 8 | p[1]
                                                            : uint32_t(p[1]) << 8 | p[0];
    return float(v) * (1.0f / 65535.0f);
  };
  uint32_t cells_x = (terrain.width - 1) / step, cells_y = (terrain.height - 1) / step;
  uint64_t triangles = 0;
  for (uint32_t ty = 0; ty < cells_y; ty += kTerrainTile) {
    for (uint32_t tx = 0; tx < cells_x; tx += kTerrainTile) {
      uint32_t nx = std::min(kTerrainTile, cells_x - tx), ny = std::min(kTerrainTile, cells_y - ty);
      uint32_t id = uint32_t(meshes.size());
      Mesh& mesh = meshes.emplace_back();
      PendingMesh p;
      p.mesh = id;
      p.vertices.reserve(size_t(nx + 1) * (ny + 1) * 3);
      for (int i = 0; i < 3; ++i) {
        mesh.bmin[i] = INFINITY;
        mesh.bmax[i] = -INFINITY;
      }
      for (uint32_t y = 0; y <= ny; ++y) {
        for (uint32_t x = 0; x <= nx; ++x) {
          uint32_t u = (tx + x) * step, v = (ty + y) * step;
          float pos[3] = {terrain.sx * u + terrain.ox, terrain.sy * value(u, v) + terrain.oy,
                          terrain.sz * v + terrain.oz};
          for (int i = 0; i < 3; ++i) {
            p.vertices.push_back(pos[i]);
            mesh.bmin[i] = std::min(mesh.bmin[i], pos[i]);
            mesh.bmax[i] = std::max(mesh.bmax[i], pos[i]);
          }
        }
      }
      // On the (x, z) grid, a, c, b has its normal along +y.
      p.indices.reserve(size_t(nx) * ny * 6);
      for (uint32_t y = 0; y < ny; ++y) {
        for (uint32_t x = 0; x < nx; ++x) {
          uint32_t a = y * (nx + 1) + x, b = a + 1, c = a + nx + 1, d = c + 1;
          uint32_t t[6] = {a, c, b, b, c, d};
          if (flip) {
            std::swap(t[1], t[2]);
            std::swap(t[4], t[5]);
          }
          p.indices.insert(p.indices.end(), t, t + 6);
        }
      }
      mesh.triangles = nx * ny * 2;
      triangles += mesh.triangles;
      pending.push_back(std::move(p));
      terrain.tiles.push_back(id);
    }
  }
  XELOGI("scenefx rt: terrain {}x{} at {:08X}, {} tiles, {} triangles (every {} texels), "
         "x {:.1f}..{:.1f} z {:.1f}..{:.1f} y {:.1f} + {:.1f} * h",
         terrain.width, terrain.height, terrain.base, terrain.tiles.size(), triangles, step,
         terrain.ox, terrain.sx * (terrain.width - 1) + terrain.ox, terrain.oz,
         terrain.sz * (terrain.height - 1) + terrain.oz, terrain.oy, terrain.sy);
}
}  // namespace

void RecordTerrain(const RegisterFile& regs, Memory& memory) {
  if (vk.tried && !vk.ok) return;
  xenos::xe_gpu_texture_fetch_t tf;
  std::memcpy(&tf, &regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 22 * 6], sizeof(tf));
  if (tf.format != xenos::TextureFormat::k_16 || tf.tiled) return;
  const float* f = reinterpret_cast<const float*>(&regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X]);
  const float* scale = f + 20 * 4;
  const float* offset = f + 21 * 4;
  const float* uv = f + 22 * 4;
  if (uv[0] == 0.0f || uv[1] == 0.0f) return;
  Terrain t;
  t.base = tf.base_address << 12;
  t.width = tf.size_2d.width + 1;
  t.height = tf.size_2d.height + 1;
  t.pitch = tf.pitch << 5;
  t.endian = uint32_t(tf.endianness);
  // uv = grid * c22.xy + c22.zw at texel centres; world = grid * c20 + c21.
  t.sx = scale[0] / (uv[0] * t.width);
  t.ox = offset[0] + scale[0] / uv[0] * (0.5f / t.width - uv[2]);
  t.sz = scale[2] / (uv[1] * t.height);
  t.oz = offset[2] + scale[2] / uv[1] * (0.5f / t.height - uv[3]);
  t.sy = scale[1];
  t.oy = offset[1];
  auto close = [](float a, float b) { return std::abs(a - b) <= 0.01f + 1e-4f * std::abs(a); };
  auto same = [&](const Terrain& o) {
    return t.base == o.base && t.width == o.width && t.height == o.height &&
           t.pitch == o.pitch && close(t.sx, o.sx) && close(t.ox, o.ox) && close(t.sz, o.sz) &&
           close(t.oz, o.oz) && close(t.sy, o.sy) && close(t.oy, o.oy);
  };
  // Patches no longer drawn.
  for (auto it = terrains.begin(); it != terrains.end();) {
    if (frame - it->seen_frame > kTerrainKeepFrames) {
      for (uint32_t id : it->tiles) Retire(id);
      it = terrains.erase(it);
    } else {
      ++it;
    }
  }
  auto found = std::find_if(terrains.begin(), terrains.end(), same);
  if (found != terrains.end()) {
    Terrain& terrain = *found;
    terrain.seen_frame = frame;
    // Changed heights (another level in the same memory): checked now and then.
    if (frame - terrain.checked_frame < 120) return;
    terrain.checked_frame = frame;
    uint64_t sample = SampleTerrain(terrain, memory);
    if (sample == terrain.sample) return;
    terrain.sample = sample;
    BuildTerrain(terrain, memory);
    return;
  }
  if (t.pitch < t.width || t.width < 2 || t.height < 2) return;
  Terrain& terrain = terrains.emplace_back(std::move(t));
  terrain.checked_frame = terrain.seen_frame = frame;
  terrain.sample = SampleTerrain(terrain, memory);
  BuildTerrain(terrain, memory);
}

namespace {

void Retire(uint32_t id) {
  meshes[id].dead = true;
  retired.push_back(id);
}

void WorldBounds(const Mesh& mesh, const float w[12], float wmin[3], float wmax[3]) {
  for (int r = 0; r < 3; ++r) {
    // Each row's extremes over the box, per axis.
    float lo = w[r * 4 + 3], hi = w[r * 4 + 3];
    for (int c = 0; c < 3; ++c) {
      float a = w[r * 4 + c] * mesh.bmin[c], b = w[r * 4 + c] * mesh.bmax[c];
      lo += std::min(a, b);
      hi += std::max(a, b);
    }
    wmin[r] = lo;
    wmax[r] = hi;
  }
}

// Whether a world box may be in the camera's view.
bool InView(const float wmin[3], const float wmax[3], const double vp[4][4]) {
  bool all_left = true, all_right = true, all_below = true, all_above = true,
       all_behind = true;
  for (int corner = 0; corner < 8; ++corner) {
    double p[3] = {(corner & 1) ? wmax[0] : wmin[0], (corner & 2) ? wmax[1] : wmin[1],
                   (corner & 4) ? wmax[2] : wmin[2]};
    double c[4];
    for (int r = 0; r < 4; ++r) {
      c[r] = vp[r][0] * p[0] + vp[r][1] * p[1] + vp[r][2] * p[2] + vp[r][3];
    }
    all_left &= c[0] < -c[3];
    all_right &= c[0] > c[3];
    all_below &= c[1] < -c[3];
    all_above &= c[1] > c[3];
    all_behind &= c[3] <= 0.0;
  }
  return !(all_left || all_right || all_below || all_above || all_behind);
}

// The builds recorded for one frame, kept alive until the command buffer is
// executed.
struct Batch {
  std::vector<VkAccelerationStructureGeometryKHR> geometries;
  std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos;
  std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
  VkAccelerationStructureGeometryKHR tlas_geometry;
  VkAccelerationStructureBuildGeometryInfoKHR tlas_info;
  VkAccelerationStructureBuildRangeInfoKHR tlas_range;
  // New meshes' triangle normals, from the upload segment.
  std::vector<VkBufferCopy> normal_copies;
};

}  // namespace

void RecordDraw(const Shader& vertex_shader, const Shader& pixel_shader,
                const RegisterFile& regs, const void* depth_target,
                Memory& memory) {
  if ((vk.tried && !vk.ok) || !recording) return;
  // Nothing built while the frames have no world pass (menus).
  if (draws.size() >= 100000) draws.clear();
  if (KillsPixels(pixel_shader)) {
    ++skipped_kill;
    return;
  }
  const PositionFetch& pf = GetPositionFetch(vertex_shader);
  if (!pf.ok) {
    ++skipped_position;
    return;
  }
  // Alpha-tested draws (foliage): their triangles are tested against a mask
  // of their texture's alpha (left out where that can't be read: solid cards
  // would cast).
  auto colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  const AlphaSource* alpha = nullptr;
  uint32_t mask = kNoAlpha;
  if ((colorcontrol.alpha_test_enable &&
       colorcontrol.alpha_func != xenos::CompareFunction::kAlways) ||
      colorcontrol.alpha_to_mask_enable) {
    // Decals (oil stains, scorch marks) lie on the ground they mark: traced,
    // the ground under them shadowed itself in a dark, speckled stain. They
    // write no depth, or are pushed towards the camera by a polygon offset.
    // Only alpha-tested draws: solid ground meshes (Guadalcanal's airfield)
    // use a polygon offset too, and left out of the traced world, ground at
    // the sea's height was taken for the sea (waves and reflections in shade).
    if (!regs.Get<reg::RB_DEPTHCONTROL>().z_write_enable ||
        regs.Get<reg::PA_SU_SC_MODE_CNTL>().poly_offset_front_enable) {
      ++skipped_alpha;
      return;
    }
    alpha = &GetAlphaSource(vertex_shader, pixel_shader);
    if (alpha->ok && alpha->fetch_constant == pf.fetch_constant) {
      mask = GetMask(regs, alpha->texture, alpha->texel_component, memory);
    } else if (alpha->ok) {
      static uint32_t logs = 0;
      if (logs++ < 8) {
        XELOGI("scenefx rt: alpha-tested shader {:016X}: coordinates from vf{}, position vf{}",
               vertex_shader.ucode_data_hash(), alpha->fetch_constant, pf.fetch_constant);
      }
    }
    if (mask == kOpaqueMask) {
      alpha = nullptr;
    } else if (mask == kNoAlpha) {
      ++skipped_alpha;
      return;
    }
  }
  if (pf.needs_c255) {
    const float* c255 = reinterpret_cast<const float*>(
        &regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X + 255 * 4]);
    if (c255[0] != 0.0f || c255[1] != 1.0f) {
      ++skipped_c255;
      return;
    }
  }
  auto initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  if ((initiator.prim_type != xenos::PrimitiveType::kTriangleList &&
       initiator.prim_type != xenos::PrimitiveType::kTriangleStrip) ||
      (initiator.source_select != xenos::SourceSelect::kDMA &&
       initiator.source_select != xenos::SourceSelect::kAutoIndex) ||
      initiator.num_indices < 3) {
    ++skipped_primitive;
    return;
  }
  MeshKey key = {};
  const uint32_t* fetch =
      &regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + pf.fetch_constant * 2];
  key.vertex_base = (fetch[0] >> 2) & 0x07FFFFFF;
  key.vertex_size = (fetch[1] >> 2) & 0xFFFFFF;
  key.vertex_endian = fetch[1] & 3;
  key.stride = pf.stride;
  key.offset = uint32_t(pf.offset);
  key.format = uint32_t(pf.format);
  key.components = pf.components[0] | pf.components[1] << 8 | pf.components[2] << 16;
  key.index_count = initiator.num_indices;
  if (initiator.source_select == xenos::SourceSelect::kDMA) {
    key.index_base = regs.values[XE_GPU_REG_VGT_DMA_BASE];
    key.index_info = uint32_t(initiator.index_size) |
                     uint32_t(regs.Get<reg::VGT_DMA_SIZE>().swap_mode) << 1;
  } else {
    key.index_info = 1u << 12;
  }
  key.index_info |= uint32_t(initiator.prim_type) << 4;
  if (initiator.prim_type == xenos::PrimitiveType::kTriangleStrip &&
      regs.Get<reg::PA_SU_SC_MODE_CNTL>().multi_prim_ib_ena) {
    key.index_info |= 1u << 13;
    key.reset_index = regs.values[XE_GPU_REG_VGT_MULTI_PRIM_IB_RESET_INDX];
  }
  key.index_offset = regs.values[XE_GPU_REG_VGT_INDX_OFFSET];
  if (alpha) {
    key.uv_offset = uint32_t(alpha->offset);
    key.uv_format = uint32_t(alpha->format);
    key.uv_components = alpha->components[0] | alpha->components[1] << 8 | uint32_t(alpha->doubled) << 16;
    key.mask = mask;
  }
  const float* constants =
      reinterpret_cast<const float*>(&regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X]);
  if (pf.part_base == UINT32_MAX) {
    AddDraw(key, pf, regs, nullptr, depth_target, memory);
    return;
  }
  // A multi-part model: each part a mesh, placed by its matrix (part to
  // world; c12-c15 is the view and projection there). Not worked out from
  // the view: the frame's camera may differ from the draw's (turning), which
  // misplaced the parts for that frame (planes' shadows gone, ghosts in the
  // reflections).
  key.part_offset = uint32_t(pf.part_offset);
  const std::vector<uint8_t>& bytes = PartBytes(key, SampleMesh(key, memory), memory);
  float scale = pf.part_c255 ? constants[255 * 4 + 2] : 1.0f;
  for (uint8_t b : std::vector<uint8_t>(bytes)) {
    int a0 = int(std::floor(float(b) * scale + 0.5f));
    if (a0 < 0 || pf.part_base + a0 + 2 > 255) continue;
    const float* part = constants + (pf.part_base + a0) * 4;
    MeshKey part_key = key;
    part_key.part = 1u | uint32_t(pf.part_component) << 1 | uint32_t(b) << 8;
    AddDraw(part_key, pf, regs, part, depth_target, memory);
  }
}

namespace {
// A draw of the mesh key: its mesh decoded if new, placed by the draw's world
// matrix, or by world (3 rows) where given.
void AddDraw(const MeshKey& key, const PositionFetch& pf, const RegisterFile& regs,
             const float* world, const void* depth_target, Memory& memory) {
  uint64_t sample = SampleMesh(key, memory);
  auto it = mesh_ids.find(key);
  if (it != mesh_ids.end() && meshes[it->second].sample != sample) {
    // The memory holds another mesh now.
    Retire(it->second);
    mesh_ids.erase(it);
    it = mesh_ids.end();
  }
  uint32_t id;
  if (it == mesh_ids.end()) {
    if (decoded_this_frame >= kDecodeBudget) return;
    decoded_this_frame += key.index_count;
    id = uint32_t(meshes.size());
    meshes.emplace_back();
    meshes[id].sample = sample;
    PendingMesh p;
    p.mesh = id;
    if (DecodeMesh(key, pf, memory, p, meshes[id])) {
      pending.push_back(std::move(p));
    } else {
      meshes[id].dead = true;
    }
    mesh_ids.emplace(key, id);
  } else {
    id = it->second;
  }
  if (meshes[id].dead) {
    ++skipped_decode;
    return;
  }
  Draw& draw = draws.emplace_back();
  draw.mesh = id;
  if (world) {
    std::memcpy(draw.w, world, sizeof(draw.w));
  } else {
    std::memcpy(draw.w, &regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X + pf.world_constant * 4],
                sizeof(draw.w));
  }
  draw.world_from_view = !world && pf.world_from_view;
  draw.height = pf.height_constant == UINT32_MAX
                    ? 0.0f
                    : regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X +
                                      pf.height_constant * 4);
  if (draw.world_from_view) {
    std::memcpy(draw.wvp, &regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X + pf.clip_constant * 4],
                sizeof(draw.wvp));
  }
  draw.depth_target = depth_target;
}
}  // namespace

void ResumeRecording() { recording = true; }

bool Init(const ui::vulkan::VulkanDevice* device) {
  if (vk.tried) return vk.ok;
  vk.tried = true;
  if (!device->extensions().bf_ray_query) {
    XELOGI("scenefx rt: the device has no ray queries");
    return false;
  }
  vk.device = device;
  VkDevice d = device->device();
  auto get = device->vulkan_instance()->functions().vkGetDeviceProcAddr;
  vk.create = PFN_vkCreateAccelerationStructureKHR(get(d, "vkCreateAccelerationStructureKHR"));
  vk.destroy = PFN_vkDestroyAccelerationStructureKHR(get(d, "vkDestroyAccelerationStructureKHR"));
  vk.sizes = PFN_vkGetAccelerationStructureBuildSizesKHR(get(d, "vkGetAccelerationStructureBuildSizesKHR"));
  vk.build = PFN_vkCmdBuildAccelerationStructuresKHR(get(d, "vkCmdBuildAccelerationStructuresKHR"));
  vk.as_address = PFN_vkGetAccelerationStructureDeviceAddressKHR(get(d, "vkGetAccelerationStructureDeviceAddressKHR"));
  vk.buffer_address = PFN_vkGetBufferDeviceAddress(get(d, "vkGetBufferDeviceAddress"));
  if (!vk.create || !vk.destroy || !vk.sizes || !vk.build || !vk.as_address ||
      !vk.buffer_address) {
    XELOGW("scenefx rt: missing acceleration structure functions");
    return false;
  }
  const VkBufferUsageFlags input =
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
  if (!CreateBuffer(kUploadSegment * kSlots, input | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    true, vk.upload) ||
      !CreateBuffer(sizeof(VkAccelerationStructureInstanceKHR) * kMaxInstances * kSlots,
                    input, true, vk.instance_data) ||
      !CreateBuffer(kScratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false,
                    vk.scratch) ||
      !CreateBuffer(VkDeviceSize(kTriangleDataWords) * 4,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    false, vk.normals)) {
    XELOGW("scenefx rt: buffers not created");
    return false;
  }
  VkAccelerationStructureGeometryKHR geometry = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
  geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
  geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
  VkAccelerationStructureBuildGeometryInfoKHR info = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
  info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  info.geometryCount = 1;
  info.pGeometries = &geometry;
  VkAccelerationStructureBuildSizesInfoKHR sizes = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
  uint32_t max_instances = kMaxInstances;
  vk.sizes(d, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info,
           &max_instances, &sizes);
  vk.tlas_scratch = Align(sizes.buildScratchSize, 256);
  if (vk.tlas_scratch > kScratchSize / 4 ||
      !CreateBuffer(sizes.accelerationStructureSize,
                    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
                    false, vk.tlas_storage)) {
    XELOGW("scenefx rt: top-level structure storage not created");
    return false;
  }
  VkAccelerationStructureCreateInfoKHR create = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
  create.buffer = vk.tlas_storage.buffer;
  create.size = sizes.accelerationStructureSize;
  create.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  if (vk.create(d, &create, nullptr, &vk.tlas) != VK_SUCCESS) {
    XELOGW("scenefx rt: top-level structure not created");
    return false;
  }
  vk.ok = true;
  XELOGI("scenefx rt: ready (top level {} KB, scratch {} KB)",
         sizes.accelerationStructureSize >> 10, vk.tlas_scratch >> 10);
  return true;
}

VkAccelerationStructureKHR Tlas() { return vk.tlas; }

VkBuffer Normals(VkDeviceSize* size) {
  *size = VkDeviceSize(kTriangleDataWords) * 4;
  return vk.normals.buffer;
}

uint32_t BuildFrame(vulkan::VulkanCommandProcessor& command_processor,
                    const void* depth_target, const double view_proj[4][4],
                    const double world_from_clip[4][4]) {
  recording = false;
  if (!vk.ok) {
    draws.clear();
    pending.clear();
    return 0;
  }
  VkDevice d = vk.device->device();
  uint64_t completed = command_processor.GetCompletedSubmission();
  uint64_t current = command_processor.GetCurrentSubmission();
  for (uint32_t id : retired) {
    if (meshes[id].blas != VK_NULL_HANDLE) {
      vk.doomed.emplace_back(meshes[id].blas, current);
      meshes[id].blas = VK_NULL_HANDLE;
    }
    PendingMesh* p = nullptr;
    for (auto& pm : pending) {
      if (pm.mesh == id) p = &pm;
    }
    if (p) p->vertices.clear();
  }
  retired.clear();
  vk.doomed.erase(std::remove_if(vk.doomed.begin(), vk.doomed.end(),
                                 [&](const auto& doomed) {
                                   if (doomed.second > completed) return false;
                                   vk.destroy(d, doomed.first, nullptr);
                                   return true;
                                 }),
                  vk.doomed.end());

  uint32_t slot = vk.next_slot;
  if (vk.slot_submission[slot] > completed) {
    // The GPU still reads this slot: keep the last top-level structure.
    draws.clear();
    return vk.last_count;
  }
  ++frame;
  decoded_this_frame = 0;
  vk.next_slot = (slot + 1) % kSlots;
  vk.slot_submission[slot] = current;

  // New meshes, as many as fit in this frame's upload and scratch space.
  auto batch = std::make_shared<Batch>();
  batch->geometries.reserve(pending.size());
  batch->infos.reserve(pending.size());
  batch->ranges.reserve(pending.size());
  VkDeviceSize upload_at = 0, scratch_at = vk.tlas_scratch;
  // New alpha masks first, before the meshes that use them.
  size_t masks_done = 0;
  for (; masks_done < pending_masks.size(); ++masks_done) {
    const auto& [offset, bits] = pending_masks[masks_done];
    VkDeviceSize bytes = Align(bits.size() * 4, 16);
    if (upload_at + bytes > kUploadSegment / 4) break;
    std::memcpy(vk.upload.mapping + slot * kUploadSegment + upload_at, bits.data(),
                bits.size() * 4);
    batch->normal_copies.push_back({slot * kUploadSegment + upload_at,
                                    (VkDeviceSize(kMaskBase) + offset) * 4,
                                    VkDeviceSize(bits.size()) * 4});
    upload_at += bytes;
  }
  pending_masks.erase(pending_masks.begin(), pending_masks.begin() + masks_done);
  size_t done = 0;
  for (; done < pending.size(); ++done) {
    PendingMesh& p = pending[done];
    Mesh& mesh = meshes[p.mesh];
    if (mesh.dead || p.vertices.empty()) continue;
    bool alpha = !p.uvs.empty();
    if (alpha && vk.alpha_used + mesh.triangles > kMaxAlphaTriangles) {
      // Solid cards would cast: left out.
      if (!vk.alpha_full) {
        vk.alpha_full = true;
        XELOGW("scenefx rt: alpha-tested triangle buffer full ({} triangles)", vk.alpha_used);
      }
      mesh.dead = true;
      continue;
    }
    VkDeviceSize vertex_bytes = Align(p.vertices.size() * 4, 16);
    VkDeviceSize index_bytes = Align(p.indices.size() * 4, 16);
    VkDeviceSize normal_bytes = Align(VkDeviceSize(mesh.triangles) * 4, 16);
    VkDeviceSize alpha_bytes = alpha ? VkDeviceSize(mesh.triangles) * 16 : 0;
    VkDeviceSize bytes = vertex_bytes + index_bytes + normal_bytes + alpha_bytes;
    if (bytes > kUploadSegment) {
      mesh.dead = true;
      continue;
    }
    if (upload_at + bytes > kUploadSegment) break;
    VkAccelerationStructureGeometryKHR& g = batch->geometries.emplace_back();
    g = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    g.flags = alpha ? 0 : VK_GEOMETRY_OPAQUE_BIT_KHR;
    auto& t = g.geometry.triangles;
    t.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    t.vertexData.deviceAddress = vk.upload.address + slot * kUploadSegment + upload_at;
    t.vertexStride = 12;
    t.maxVertex = uint32_t(p.vertices.size() / 3 - 1);
    t.indexType = VK_INDEX_TYPE_UINT32;
    t.indexData.deviceAddress = t.vertexData.deviceAddress + vertex_bytes;
    VkAccelerationStructureBuildGeometryInfoKHR info = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = 1;
    info.pGeometries = &g;
    VkAccelerationStructureBuildSizesInfoKHR sizes = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vk.sizes(d, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info,
             &mesh.triangles, &sizes);
    VkDeviceSize scratch = Align(sizes.buildScratchSize, 256);
    VkBuffer storage;
    VkDeviceSize storage_offset;
    if (scratch_at + scratch > kScratchSize) {
      batch->geometries.pop_back();
      if (scratch > kScratchSize - vk.tlas_scratch) {
        mesh.dead = true;
        continue;
      }
      break;
    }
    if (!AllocatePool(sizes.accelerationStructureSize, storage, storage_offset)) {
      batch->geometries.pop_back();
      mesh.dead = true;
      continue;
    }
    VkAccelerationStructureCreateInfoKHR create = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    create.buffer = storage;
    create.offset = storage_offset;
    create.size = sizes.accelerationStructureSize;
    create.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    if (vk.create(d, &create, nullptr, &mesh.blas) != VK_SUCCESS) {
      batch->geometries.pop_back();
      mesh.dead = true;
      continue;
    }
    VkAccelerationStructureDeviceAddressInfoKHR address = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    address.accelerationStructure = mesh.blas;
    mesh.address = vk.as_address(d, &address);
    uint8_t* to = vk.upload.mapping + slot * kUploadSegment + upload_at;
    std::memcpy(to, p.vertices.data(), p.vertices.size() * 4);
    std::memcpy(to + vertex_bytes, p.indices.data(), p.indices.size() * 4);
    // Its triangles' normals (object space; the shader turns them towards
    // the ray), for shading what reflection and bounce rays hit.
    if (vk.normals_used + mesh.triangles <= kMaxNormals) {
      uint32_t* normals = reinterpret_cast<uint32_t*>(to + vertex_bytes + index_bytes);
      for (uint32_t t = 0; t < mesh.triangles; ++t) {
        const float* a = &p.vertices[size_t(p.indices[t * 3]) * 3];
        const float* b = &p.vertices[size_t(p.indices[t * 3 + 1]) * 3];
        const float* c = &p.vertices[size_t(p.indices[t * 3 + 2]) * 3];
        float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        float v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        normals[t] = PackNormal(u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
                                u[0] * v[1] - u[1] * v[0]);
      }
      mesh.normal_base = vk.normals_used;
      vk.normals_used += mesh.triangles;
      batch->normal_copies.push_back(
          {slot * kUploadSegment + upload_at + vertex_bytes + index_bytes,
           VkDeviceSize(mesh.normal_base) * 4, VkDeviceSize(mesh.triangles) * 4});
    } else if (!vk.normals_full) {
      vk.normals_full = true;
      XELOGW("scenefx rt: triangle normal buffer full ({} triangles)", vk.normals_used);
    }
    if (alpha) {
      // Its corners' texture coordinates and its mask, per triangle.
      uint32_t* entries =
          reinterpret_cast<uint32_t*>(to + vertex_bytes + index_bytes + normal_bytes);
      for (uint32_t t = 0; t < mesh.triangles; ++t) {
        for (int k = 0; k < 3; ++k) {
          const float* uv = &p.uvs[size_t(p.indices[t * 3 + k]) * 2];
          entries[t * 4 + k] = ToHalf(uv[0]) | ToHalf(uv[1]) << 16;
        }
        entries[t * 4 + 3] = p.mask;
      }
      mesh.alpha_base = vk.alpha_used;
      vk.alpha_used += mesh.triangles;
      batch->normal_copies.push_back(
          {slot * kUploadSegment + upload_at + vertex_bytes + index_bytes + normal_bytes,
           (VkDeviceSize(kAlphaBase) + VkDeviceSize(mesh.alpha_base) * 4) * 4, alpha_bytes});
    }
    upload_at += bytes;
    info.dstAccelerationStructure = mesh.blas;
    info.scratchData.deviceAddress = vk.scratch.address + scratch_at;
    scratch_at += scratch;
    batch->infos.push_back(info);
    VkAccelerationStructureBuildRangeInfoKHR& range = batch->ranges.emplace_back();
    range = {};
    range.primitiveCount = mesh.triangles;
    total_triangles += mesh.triangles;
  }
  pending.erase(pending.begin(), pending.begin() + done);
  for (size_t i = 0; i < batch->infos.size(); ++i) {
    batch->infos[i].pGeometries = &batch->geometries[i];
  }

  // This frame's instances: what was drawn, plus static objects seen before
  // that are out of view now.
  uint32_t drawn = 0;
  for (Draw& draw : draws) {
    if (draw.depth_target != depth_target) continue;
    if (draw.world_from_view) {
      // world = (clip x, y, w -> world) * (object -> clip x, y, w); clip z
      // is left out, it differs where another near plane is used.
      static const int kRows[3] = {0, 1, 3};
      for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
          double sum = world_from_clip[r][3] * (c == 3 ? 1.0 : 0.0);
          for (int k = 0; k < 3; ++k) sum += world_from_clip[r][k] * draw.wvp[kRows[k] * 4 + c];
          draw.w[r * 4 + c] = float(sum);
        }
      }
      // Only rigid placements (another field of view, or a draw for another
      // camera, gives a skewed matrix).
      bool rigid = true;
      for (int a = 0; a < 3; ++a) {
        for (int b = a; b < 3; ++b) {
          double dot = 0.0;
          for (int r = 0; r < 3; ++r) dot += double(draw.w[r * 4 + a]) * draw.w[r * 4 + b];
          rigid &= std::abs(dot - (a == b ? 1.0 : 0.0)) < 0.02;
        }
      }
      if (!rigid) continue;
      for (int r = 0; r < 3; ++r) draw.w[r * 4 + 3] += draw.w[r * 4 + 1] * draw.height;
      // Not the sea surface (shader 4ECF2B44278A9027 on Wake Island: an
      // exactly flat grid kilometres wide), which the effects model as a
      // plane; a mesh of it reflects into the water from below.
      const Mesh& m = meshes[draw.mesh];
      if (m.bmax[1] - m.bmin[1] < 0.01f &&
          std::max(m.bmax[0] - m.bmin[0], m.bmax[2] - m.bmin[2]) > 1000.0f) {
        continue;
      }
    }
    uint64_t h = Fnv(draw.w, sizeof(draw.w), Fnv(&draw.mesh, sizeof(draw.mesh)));
    auto [it, added] = instances.try_emplace(h);
    Instance& instance = it->second;
    if (added) {
      instance.mesh = draw.mesh;
      std::memcpy(instance.w, draw.w, sizeof(draw.w));
      WorldBounds(meshes[draw.mesh], draw.w, instance.wmin, instance.wmax);
      instance.first_frame = frame;
    }
    instance.last_frame = frame;
  }
  draws.clear();
  auto* out = reinterpret_cast<VkAccelerationStructureInstanceKHR*>(
      vk.instance_data.mapping +
      slot * kMaxInstances * sizeof(VkAccelerationStructureInstanceKHR));
  uint32_t count = 0;
  for (auto it = instances.begin(); it != instances.end();) {
    Instance& instance = it->second;
    const Mesh& mesh = meshes[instance.mesh];
    bool keep;
    if (mesh.dead) {
      keep = false;
    } else if (instance.last_frame == frame) {
      keep = true;
      ++drawn;
    } else if (instance.first_frame == instance.last_frame) {
      // Seen in one frame only: moving.
      keep = false;
    } else {
      // Static: kept while out of view; in view but not drawn, it is gone.
      keep = frame - instance.last_frame < kStaticKeepFrames &&
             !InView(instance.wmin, instance.wmax, view_proj);
    }
    if (!keep) {
      it = instances.erase(it);
      continue;
    }
    if (mesh.address && count < kMaxInstances) {
      VkAccelerationStructureInstanceKHR& o = out[count++];
      std::memcpy(o.transform.matrix, instance.w, sizeof(instance.w));
      o.instanceCustomIndex = mesh.normal_base;
      o.mask = mesh.alpha_base == kNoAlpha ? kMaskObjects : kMaskFoliage;
      o.instanceShaderBindingTableRecordOffset = mesh.alpha_base == kNoAlpha ? 0 : mesh.alpha_base;
      o.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
      o.accelerationStructureReference = mesh.address;
    }
    ++it;
  }

  // The terrain, always; its back faces (seen from under the ground, where a
  // coarser drawn patch lies below the heightmap) can be culled.
  for (const Terrain& terrain : terrains)
  for (uint32_t id : terrain.tiles) {
    const Mesh& mesh = meshes[id];
    if (mesh.dead || !mesh.address || count >= kMaxInstances) continue;
    VkAccelerationStructureInstanceKHR& o = out[count++];
    static const float kIdentity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    std::memcpy(o.transform.matrix, kIdentity, sizeof(kIdentity));
    o.instanceCustomIndex = mesh.normal_base;
    o.mask = kMaskTerrain;
    o.instanceShaderBindingTableRecordOffset = 0;
    o.flags = 0;
    o.accelerationStructureReference = mesh.address;
  }

  batch->tlas_geometry = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
  batch->tlas_geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
  auto& instances_data = batch->tlas_geometry.geometry.instances;
  instances_data.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
  instances_data.arrayOfPointers = VK_FALSE;
  instances_data.data.deviceAddress =
      vk.instance_data.address +
      slot * kMaxInstances * sizeof(VkAccelerationStructureInstanceKHR);
  batch->tlas_info = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  batch->tlas_info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  batch->tlas_info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
  batch->tlas_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  batch->tlas_info.dstAccelerationStructure = vk.tlas;
  batch->tlas_info.geometryCount = 1;
  batch->tlas_info.pGeometries = &batch->tlas_geometry;
  batch->tlas_info.scratchData.deviceAddress = vk.scratch.address;
  batch->tlas_range = {};
  batch->tlas_range.primitiveCount = count;

  auto build = vk.build;
  auto barrier = vk.device->functions().vkCmdPipelineBarrier;
  auto copy = vk.device->functions().vkCmdCopyBuffer;
  VkBuffer upload = vk.upload.buffer, normals = vk.normals.buffer;
  command_processor.deferred_command_buffer().CmdCallback(
      [batch, build, barrier, copy, upload, normals](VkCommandBuffer cb) {
        const VkPipelineStageFlags as_stage =
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        const VkAccessFlags as_rw = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                                    VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        if (!batch->normal_copies.empty()) {
          // New regions only: no shader reads them before this frame.
          copy(cb, upload, normals, uint32_t(batch->normal_copies.size()),
               batch->normal_copies.data());
          VkMemoryBarrier nb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
          nb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
          nb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
          barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &nb, 0, nullptr, 0,
                  nullptr);
        }
        // After the last frame's builds and ray queries.
        VkMemoryBarrier b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        b.srcAccessMask = as_rw | VK_ACCESS_SHADER_READ_BIT;
        b.dstAccessMask = as_rw;
        barrier(cb, as_stage | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, as_stage,
                0, 1, &b, 0, nullptr, 0, nullptr);
        if (!batch->infos.empty()) {
          std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> ranges;
          for (const auto& range : batch->ranges) ranges.push_back(&range);
          build(cb, uint32_t(batch->infos.size()), batch->infos.data(),
                ranges.data());
          b.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
          b.dstAccessMask = as_rw;
          barrier(cb, as_stage, as_stage, 0, 1, &b, 0, nullptr, 0, nullptr);
        }
        const VkAccelerationStructureBuildRangeInfoKHR* tlas_range =
            &batch->tlas_range;
        build(cb, 1, &batch->tlas_info, &tlas_range);
        b.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        b.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        barrier(cb, as_stage, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b,
                0, nullptr, 0, nullptr);
      });

  static uint32_t logs = 0;
  if ((frame == 2 || frame % 600 == 0) && logs++ < 40) {
    size_t live = 0;
    for (const Mesh& m : meshes) live += !m.dead && m.address;
    XELOGI(
        "scenefx rt: frame {}: {} meshes ({} triangles built), {} pending, {} "
        "instances ({} drawn, {} kept), pool {} MB; draws skipped: alpha {}, "
        "kill {}, position {}, c255 {}, primitive {}, decode {}",
        frame, live, total_triangles, pending.size(), count, drawn,
        instances.size(),
        ((vk.pool.empty() ? 0 : (vk.pool.size() - 1) * kPoolChunk) + vk.pool_used) >> 20,
        skipped_alpha, skipped_kill, skipped_position, skipped_c255, skipped_primitive,
        skipped_decode);
  }
  vk.last_count = count;
  return count;
}

}  // namespace rt
}  // namespace scenefx
}  // namespace gpu
}  // namespace xe
