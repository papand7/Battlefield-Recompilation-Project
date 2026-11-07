// Video imports (Vd*) and a minimal PM4 command processor.
//
// The game's statically linked XDK D3D writes PM4 packets into the ring buffer it
// registers with VdInitializeRingBuffer and bumps CP_RB_WPTR through the GPU
// register block at 0x7FC80000. This processor consumes the ring and executes
// what the CPU side waits on (memory writes, event/fence writes, scratch
// write-back, waits, interrupts). Draw packets are only counted here; turning
// them into Vulkan work is the renderer's job (milestone 3).
// Packet formats follow the public R500/Xenos PM4 layout (as documented by Xenia).
#include "kernel.h"
#include "xenia_bridge.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <algorithm>
#include <deque>
#include <vector>
#include <tuple>
#include <thread>

using namespace rt;

extern "C" std::atomic<uint64_t> g_title_swaps;  // xenia_shims/bridge.cc

namespace rt {
bool g_use_xenia_gpu = true;
bool g_present = true;
uint32_t g_capture_seconds = 0;
uint32_t g_capture_burst = 0;
bool g_blood = false;
}

namespace rt {
uint32_t g_vblank_hz = 60;
}

namespace {

constexpr uint32_t kGpuRegs = 0x7FC80000;
constexpr uint32_t CP_RB_WPTR = 0x01C5;
constexpr uint32_t SCRATCH_UMSK = 0x01DC;
constexpr uint32_t SCRATCH_ADDR = 0x01DD;
constexpr uint32_t SCRATCH_REG0 = 0x0578;
constexpr uint32_t D1MODE_V_COUNTER = 0x1952;

struct GpuState {
  uint32_t ring_base = 0, ring_size_dwords = 0;
  uint32_t rptr_writeback = 0;
  uint32_t rptr = 0;
  uint32_t interrupt_callback = 0, interrupt_user = 0;
  uint32_t regs[0x10000] = {};
  uint64_t swaps = 0, draws = 0, packets = 0;
  uint32_t vsync_count = 0;
} g_gpu;

std::mutex g_interrupt_mutex;
std::condition_variable g_interrupt_cv;
std::deque<std::pair<uint32_t, uint32_t>> g_interrupts;  // (source, cpu mask)
std::once_flag g_gpu_started;

uint32_t PhysToVirt(uint32_t a) { return a < 0x20000000 ? (a | 0xA0000000) : a; }

uint32_t GpuSwap(uint32_t v, uint32_t endian) {
  switch (endian & 3) {
    case 1: return ((v & 0x00FF00FF) << 8) | ((v & 0xFF00FF00) >> 8);  // 8in16
    case 2: return __builtin_bswap32(v);                                // 8in32
    case 3: return (v << 16) | (v >> 16);                               // 16in32
    default: return v;
  }
}

// The GPU accesses memory little-endian; the address's low 2 bits select the
// byte swap applied on top (2 = 8in32, i.e. big-endian data as the CPU sees it).
uint32_t ReadMem(uint32_t addr_with_endian) {
  if (!IsCommitted(PhysToVirt(addr_with_endian & ~3u))) {
    RT_WARN("GPU: read of unmapped %08X", addr_with_endian);
    return 0;
  }
  return GpuSwap(*G<uint32_t>(PhysToVirt(addr_with_endian & ~3u)), addr_with_endian & 3);
}

void WriteMem(uint32_t addr_with_endian, uint32_t value) {
  if ((addr_with_endian & 0x1FFFFFF0) == 0x06311000)
    RT_TRACE("GPU write %08X <- %X", addr_with_endian, value);
  if (!IsCommitted(PhysToVirt(addr_with_endian & ~3u))) {
    RT_WARN("GPU: write of %X to unmapped %08X", value, addr_with_endian);
    return;
  }
  *G<uint32_t>(PhysToVirt(addr_with_endian & ~3u)) = GpuSwap(value, addr_with_endian & 3);
}

void PostInterrupt(uint32_t source, uint32_t cpu_mask) {
  std::lock_guard lock(g_interrupt_mutex);
  g_interrupts.push_back({source, cpu_mask});
  g_interrupt_cv.notify_one();
}

void WriteRegister(uint32_t reg, uint32_t value) {
  reg &= 0xFFFF;
  if ((reg == SCRATCH_ADDR || reg == SCRATCH_UMSK) && g_gpu.regs[reg] != value)
    RT_WARN("GPU: register %04X changed %08X -> %08X from the ring", reg, g_gpu.regs[reg], value);
  g_gpu.regs[reg] = value;
  if (reg >= SCRATCH_REG0 && reg < SCRATCH_REG0 + 8) {
    uint32_t n = reg - SCRATCH_REG0;
    // UMSK/ADDR may have been set by the CPU straight through MMIO.
    if (!g_gpu.regs[SCRATCH_ADDR]) g_gpu.regs[SCRATCH_ADDR] = Ld32(kGpuRegs + SCRATCH_ADDR * 4);
    if (!g_gpu.regs[SCRATCH_UMSK]) g_gpu.regs[SCRATCH_UMSK] = Ld32(kGpuRegs + SCRATCH_UMSK * 4);
    RT_TRACE("GPU scratch%u <- %X (umsk %X addr %08X)", n, value, g_gpu.regs[SCRATCH_UMSK], g_gpu.regs[SCRATCH_ADDR]);
    uint32_t a = PhysToVirt(g_gpu.regs[SCRATCH_ADDR] + n * 4);
    if (g_gpu.regs[SCRATCH_UMSK] & (1u << n)) {
      if (IsCommitted(a)) St32(a, value);
      else RT_WARN("GPU: scratch%u write-back to unmapped %08X", n, a);
    }
  }
}

uint32_t ReadRegister(uint32_t reg) {
  if (reg == D1MODE_V_COUNTER) return g_gpu.vsync_count & 1 ? 0x2D0 : 0;
  return g_gpu.regs[reg & 0xFFFF];
}

// Compare function for WAIT_REG_MEM / COND_WRITE (bits 0-2 of wait_info).
bool Compare(uint32_t fn, uint32_t value, uint32_t ref) {
  switch (fn & 7) {
    case 0: return false;
    case 1: return value < ref;
    case 2: return value <= ref;
    case 3: return value == ref;
    case 4: return value != ref;
    case 5: return value >= ref;
    case 6: return value > ref;
    default: return true;
  }
}

class Reader {
 public:
  Reader(uint32_t base, uint32_t size_dwords, uint32_t start, uint32_t end)
      : base_(base), size_(size_dwords), pos_(start), end_(end) {}
  bool More() const { return pos_ != end_; }
  uint32_t Next() {
    uint32_t v = Ld32(base_ + pos_ * 4);
    pos_ = size_ ? (pos_ + 1) % size_ : pos_ + 1;
    return v;
  }
  void Skip(uint32_t n) { while (n--) Next(); }
  uint32_t pos() const { return pos_; }

 private:
  uint32_t base_, size_, pos_, end_;
};

void Execute(Reader& r, int depth);

void ExecuteType3(Reader& r, uint32_t op, uint32_t count, int depth) {
  switch (op) {
    case 0x3D: {  // MEM_WRITE: address, then values
      uint32_t addr = r.Next();
      for (uint32_t i = 1; i < count; ++i) WriteMem(addr + (i - 1) * 4, r.Next());
      break;
    }
    case 0x58: {  // EVENT_WRITE_SHD: initiator, address, value
      uint32_t initiator = r.Next(), addr = r.Next(), value = r.Next();
      r.Skip(count - 3);
      RT_TRACE("GPU EVENT_WRITE_SHD initiator %08X addr %08X value %X", initiator, addr, value);
      if (initiator & 0x80000000) value = uint32_t(g_gpu.swaps);
      WriteMem(addr, value);
      break;
    }
    case 0x59: {  // EVENT_WRITE_EXT: initiator, address -> screen extents
      r.Next();
      uint32_t addr = r.Next();
      r.Skip(count - 2);
      static const uint16_t kExtents[6] = {0, 1280 / 8, 0, 720 / 8, 0, 1};
      uint32_t a = PhysToVirt(addr & ~3u);
      for (int i = 0; i < 6; ++i) St16(a + i * 2, kExtents[i]);
      break;
    }
    case 0x5B: {  // EVENT_WRITE_ZPD: report a plausible sample count
      r.Skip(count);
      break;
    }
    case 0x3C: {  // WAIT_REG_MEM: wait_info, poll, ref, mask, wait
      uint32_t info = r.Next(), poll = r.Next(), ref = r.Next(), mask = r.Next();
      r.Next();
      r.Skip(count - 5);
      auto start = std::chrono::steady_clock::now();
      for (;;) {
        uint32_t v = (info & 0x10) ? ReadMem(poll) : ReadRegister(poll);
        if (Compare(info, v & mask, ref)) break;
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) {
          RT_WARN("GPU: WAIT_REG_MEM timed out (info %X poll %08X ref %X mask %X value %X)", info,
                  poll, ref, mask, v);
          break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
      break;
    }
    case 0x45: {  // COND_WRITE: wait_info, poll, ref, mask, write addr, value
      uint32_t info = r.Next(), poll = r.Next(), ref = r.Next(), mask = r.Next();
      uint32_t waddr = r.Next(), value = r.Next();
      r.Skip(count - 6);
      uint32_t v = (info & 0x10) ? ReadMem(poll) : ReadRegister(poll);
      if (Compare(info, v & mask, ref)) {
        if (info & 0x100) WriteMem(waddr, value);
        else WriteRegister(waddr, value);
      }
      break;
    }
    case 0x21: {  // REG_RMW: info, and, or
      uint32_t info = r.Next(), and_mask = r.Next(), or_mask = r.Next();
      r.Skip(count - 3);
      uint32_t reg = info & 0x1FFF;
      uint32_t v = ReadRegister(reg);
      if (info & 0x80000000) and_mask = ReadRegister(and_mask & 0x1FFF);
      if (info & 0x40000000) or_mask = ReadRegister(or_mask & 0x1FFF);
      WriteRegister(reg, (v & and_mask) | or_mask);
      break;
    }
    case 0x54: {  // INTERRUPT: cpu mask
      uint32_t mask = r.Next();
      r.Skip(count - 1);
      PostInterrupt(1, mask);
      break;
    }
    case 0x3F:    // INDIRECT_BUFFER
    case 0x37: {  // INDIRECT_BUFFER_PFD
      uint32_t addr = r.Next(), size = r.Next() & 0xFFFFF;
      r.Skip(count - 2);
      if (depth > 4) break;
      Reader ib(PhysToVirt(addr & ~3u), 0, 0, size);
      Execute(ib, depth + 1);
      break;
    }
    case 0x22:  // DRAW_INDX
    case 0x36:  // DRAW_INDX_2
      ++g_gpu.draws;
      r.Skip(count);
      break;
    case 0x2D: {  // SET_CONSTANT: offset/type, values (registers, ALU, fetch, bool, loop)
      uint32_t off_type = r.Next();
      uint32_t index = off_type & 0x7FF, type = (off_type >> 16) & 0xFF;
      static const uint32_t kBase[] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};
      uint32_t base = type < 5 ? kBase[type] : 0;
      for (uint32_t i = 1; i < count; ++i) {
        uint32_t v = r.Next();
        if (base) WriteRegister(base + index + i - 1, v);
      }
      break;
    }
    default:
      r.Skip(count);
      break;
  }
}

void Execute(Reader& r, int depth) {
  while (r.More()) {
    uint32_t h = r.Next();
    ++g_gpu.packets;
    switch (h >> 30) {
      case 0: {
        uint32_t reg = h & 0x7FFF, count = ((h >> 16) & 0x3FFF) + 1;
        bool one_reg = (h >> 15) & 1;
        for (uint32_t i = 0; i < count; ++i) WriteRegister(one_reg ? reg : reg + i, r.Next());
        break;
      }
      case 1: {
        uint32_t r0 = h & 0x7FF, r1 = (h >> 11) & 0x7FF;
        WriteRegister(r0, r.Next());
        WriteRegister(r1, r.Next());
        break;
      }
      case 2:
        break;
      case 3:
        ExecuteType3(r, (h >> 8) & 0x7F, ((h >> 16) & 0x3FFF) + 1, depth);
        break;
    }
  }
}

void CommandProcessorThread() {
  SetThreadDescription(GetCurrentThread(), L"gpu command processor");
  auto last_report = std::chrono::steady_clock::now();
  for (;;) {
    if (g_gpu.ring_base && g_gpu.ring_size_dwords) {
      uint32_t wptr = Ld32(kGpuRegs + CP_RB_WPTR * 4) % g_gpu.ring_size_dwords;
      if (wptr != g_gpu.rptr) {
        Reader r(g_gpu.ring_base, g_gpu.ring_size_dwords, g_gpu.rptr, wptr);
        Execute(r, 0);
        g_gpu.rptr = wptr;
        if (g_gpu.rptr_writeback) St32(g_gpu.rptr_writeback, g_gpu.rptr);
      }
    }
    auto now = std::chrono::steady_clock::now();
    if (now - last_report > std::chrono::seconds(2)) {
      last_report = now;
      RT_INFO("GPU: %llu packets, %llu draws, %llu swaps, %u vblanks", g_gpu.packets, g_gpu.draws,
              g_gpu.swaps, g_gpu.vsync_count);
    }
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
}

// With Xenia's command processor: CPU writes to the GPU register block land in
// plain memory, so diff the block against a shadow copy and forward changes.
// CP_RB_WPTR (the ring-buffer kick) also arrives at once through RtGpuMmioWrite32;
// after the first such write the mirror leaves it alone, so a late scan can't move
// the write pointer backwards.
std::atomic<bool> g_mirror_running{false};
std::mutex g_wptr_lock;
bool g_wptr_direct = false;

void RegisterMirrorThread() {
  SetThreadDescription(GetCurrentThread(), L"gpu register mirror");
  static uint32_t shadow[0x8000];
  for (;;) {
    for (uint32_t i = 0; i < 0x8000; ++i) {
      uint32_t v = Ld32(kGpuRegs + i * 4);
      if (v != shadow[i]) {
        shadow[i] = v;
        if (i == CP_RB_WPTR) {
          std::lock_guard<std::mutex> lock(g_wptr_lock);
          if (!g_wptr_direct) xgpu::WriteRegister(i, Ld32(kGpuRegs + i * 4));
          continue;
        }
        xgpu::WriteRegister(i, v);
      }
    }
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
}
}  // namespace

// Called by the PPC_MM_STORE_U32 hook (game/ppc_overrides.h) for MMIO stores to the
// GPU block: D3D's kick goes to the command processor without waiting ~1 ms for the
// next mirror scan.
extern "C" void RtGpuMmioWrite32(uint32_t address, uint32_t value) {
  if ((address & 0xFFFF) != CP_RB_WPTR * 4 || !g_mirror_running.load(std::memory_order_acquire))
    return;
  std::lock_guard<std::mutex> lock(g_wptr_lock);
  g_wptr_direct = true;
  xgpu::WriteRegister(CP_RB_WPTR, value);
}

namespace {
void XeniaInterrupt(uint32_t source, uint32_t cpu) { PostInterrupt(source, 1u << cpu); }

// --blood: every soldier's health is a float (100 when full) at +0x3C of their
// soldier object, right after the vtable of its health part (+0x34, with
// 00011100 at +0x2C and the part's vtable - 0xC at +0x40). The server's
// ServerSoldierEntity (vtable 82013A40, health part 820139F0; games this copy
// hosts) has the feet at +0x180 (x, y up, z), the last hit's impulse at
// +0x280 and the killing blow's at +0x120 (a bazooka at the feet: about 3100,
// mostly up). Every copy of the game also has a client soldier (vtable
// 82007518, health part 820074F0) with the feet at +0x5D0. Found by watching
// floats of 100 drop when the player hit themselves (100 -> 12) and the
// soldier objects around them. The server's soldiers are read where there are
// any, else the clients'. A drop is a hit; a drop to 0 a death, which bursts
// the body apart when the blow was an explosion (an impulse over 1500) or
// took 60 or more.
struct SoldierKind {
  uint32_t vtable, part, feet;
};
constexpr SoldierKind kServerSoldier = {0x82013A40u, 0x820139F0u, 0x180};
constexpr SoldierKind kClientSoldier = {0x82007518u, 0x820074F0u, 0x5D0};

bool HealthObject(uint32_t health, const SoldierKind& kind) {
  return Ld32(health - 8) == kind.part && Ld32(health - 16) == 0x00011100u &&
         Ld32(health + 4) == kind.part - 0xC && Ld32(health - 0x3C) == kind.vtable;
}

std::vector<uint32_t> FindHealthObjects(const SoldierKind& kind) {
  std::vector<uint32_t> found;
  const uint32_t vtable = _byteswap_ulong(kind.part);
  // The heaps (0xC0000000 and up mirror 0xA0000000).
  for (uint64_t a = 0x40000000; a < 0xC0000000ull;) {
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(g_base + a, &mi, sizeof(mi))) break;
    uint64_t end = uint64_t(static_cast<uint8_t*>(mi.BaseAddress) - g_base) + mi.RegionSize;
    if (mi.State == MEM_COMMIT && (mi.Protect & (PAGE_READWRITE | PAGE_READONLY)) &&
        !(mi.Protect & PAGE_GUARD)) {
      const uint32_t* p = reinterpret_cast<const uint32_t*>(g_base + a);
      for (uint64_t n = (std::min<uint64_t>(end, 0xC0000000ull) - a) / 4, i = 16; i + 3 < n; ++i) {
        if (p[i] == vtable && HealthObject(uint32_t(a + i * 4 + 8), kind)) found.push_back(uint32_t(a + i * 4 + 8));
      }
    }
    a = end;
  }
  return found;
}

float HealthAt(uint32_t address) {
  uint32_t v = Ld32(address);
  float f;
  memcpy(&f, &v, 4);
  return f;
}

void HealthWatchThread() {
  struct Watched {
    uint32_t health;
    float last;
  };
  std::vector<Watched> soldiers;
  const SoldierKind* kind = &kClientSoldier;
  auto last_scan = std::chrono::steady_clock::now() - std::chrono::seconds(10);
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(33));
    auto now = std::chrono::steady_clock::now();
    // Again every few seconds for the soldiers spawned since.
    if (now - last_scan > std::chrono::seconds(2)) {
      last_scan = now;
      std::vector<uint32_t> found = FindHealthObjects(kServerSoldier);
      const SoldierKind* found_kind = &kServerSoldier;
      if (found.empty()) {
        found = FindHealthObjects(kClientSoldier);
        found_kind = &kClientSoldier;
      }
      if (found_kind != kind) {
        soldiers.clear();
        kind = found_kind;
        RT_INFO("blood: watching the %s soldiers", kind == &kServerSoldier ? "server's" : "client's");
      }
      for (uint32_t h : found) {
        if (std::none_of(soldiers.begin(), soldiers.end(), [h](const Watched& w) { return w.health == h; }))
          soldiers.push_back({h, HealthAt(h)});
      }
    }
    for (size_t i = 0; i < soldiers.size();) {
      Watched& w = soldiers[i];
      if (!HealthObject(w.health, *kind)) {
        soldiers.erase(soldiers.begin() + i);
        continue;
      }
      ++i;
      float h = HealthAt(w.health);
      if (!(h >= -1000.0f && h <= 1000.0f)) continue;
      float drop = w.last - h;
      if (w.last > 0.0f && drop > 0.5f) {
        uint32_t s = w.health - 0x3C;
        float feet[3] = {HealthAt(s + kind->feet), HealthAt(s + kind->feet + 4), HealthAt(s + kind->feet + 8)};
        // The blow's impulse (the server's only).
        float push[3] = {0.0f, 0.0f, 0.0f};
        float impulse = 0.0f;
        if (kind == &kServerSoldier) {
          uint32_t at = s + (h <= 0.0f ? 0x120 : 0x280);
          float v[3] = {HealthAt(at), HealthAt(at + 4), HealthAt(at + 8)};
          float m = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
          if (std::isfinite(m) && m > 1.0f && m < 1e6f) {
            impulse = m;
            for (int a = 0; a < 3; ++a) push[a] = v[a] / m;
          }
        }
        if (h <= 0.0f) {
          xgpu::GoreDeath(feet, drop, drop >= 60.0f || impulse > 1500.0f, push);
        } else {
          xgpu::GoreHit(feet, drop);
        }
        RT_INFO("blood: soldier %08X %.0f -> %.0f at %.1f %.1f %.1f, impulse %.0f", s, w.last, h, feet[0], feet[1],
                feet[2], impulse);
      }
      w.last = h;
    }
    // Debug: BF_GORE_TEST=1 fakes a hit on every soldier each 1.5 s, every
    // third one a death that bursts the body apart.
    static const bool test = getenv("BF_GORE_TEST") != nullptr;
    static int test_tick = 0;
    if (test && ++test_tick % 45 == 0) {
      for (const Watched& w : soldiers) {
        uint32_t s = w.health - 0x3C;
        float feet[3] = {HealthAt(s + kind->feet), HealthAt(s + kind->feet + 4), HealthAt(s + kind->feet + 8)};
        const float up[3] = {0.0f, 1.0f, 0.0f};
        if (test_tick % 135 == 0) xgpu::GoreDeath(feet, 100.0f, true, up);
        else xgpu::GoreHit(feet, 30.0f);
        RT_INFO("blood: test %08X at %.1f %.1f %.1f", s, feet[0], feet[1], feet[2]);
      }
    }
  }
}

// Interrupts run guest code, so they get a guest identity of their own. D3D's
// handler acknowledges by clearing the bit of the CPU it runs on (KPCR+0x10C),
// so each interrupt is delivered once per CPU in its mask.
void InterruptThread() {
  PPCContext& ctx = AdoptHostThread("gpu interrupts", 0x10000);
  const uint32_t pcr = ctx.r13.u32;
  auto next_vblank = std::chrono::steady_clock::now();
  for (;;) {
    uint32_t source = 0, mask = 1;
    bool have = false;
    {
      std::unique_lock lock(g_interrupt_mutex);
      g_interrupt_cv.wait_until(lock, next_vblank, [] { return !g_interrupts.empty(); });
      if (!g_interrupts.empty()) {
        std::tie(source, mask) = g_interrupts.front();
        g_interrupts.pop_front();
        have = true;
      }
    }
    if (!have) {
      source = 0;  // vblank, delivered on CPU 0
      mask = 1;
      ++g_gpu.vsync_count;
      if (g_use_xenia_gpu) xgpu::Vblank();
      St32(kGpuRegs + 0x1951 * 4, 1);  // vblank pending
      St32(kGpuRegs + D1MODE_V_COUNTER * 4, 720);
      next_vblank += std::chrono::microseconds(1000000 / g_vblank_hz);
    }
    if (!g_gpu.interrupt_callback) continue;
    for (uint32_t cpu = 0; cpu < 6; ++cpu) {
      if (!(mask & (1u << cpu))) continue;
      St8(pcr + 0x10C, uint8_t(cpu));
      GuestCall(ctx, g_gpu.interrupt_callback, {source, g_gpu.interrupt_user});
    }
  }
}

// Registers the CPU reads straight from the MMIO block (values as Xenia reports them).
void SeedStatusRegisters() {
  St32(kGpuRegs + 0x0F00 * 4, 0x08100748);            // RB_EDRAM_TIMING
  St32(kGpuRegs + 0x0F01 * 4, 0x0000200E);            // RB_BC_CONTROL
  St32(kGpuRegs + 0x1951 * 4, 1);                     // D1MODE vblank/interrupt status
  St32(kGpuRegs + 0x1961 * 4, (1280u << 16) | 720u);  // D1MODE_VIEWPORT_SIZE
}

void StartGpu() {
  std::call_once(g_gpu_started, [] {
    SeedStatusRegisters();
    if (g_use_xenia_gpu) {
      if (g_blood) {
        constexpr uint32_t kGoreScratch = 4u << 20;
        uint32_t scratch = g_physical_heap->Alloc(kGoreScratch, 4096);
        xgpu::SetGore(true, scratch & 0x1FFFFFFFu, scratch ? kGoreScratch : 0);
      }
      if (!xgpu::Start(g_base, g_present, XeniaInterrupt, 0)) Fatal("Vulkan GPU failed to start");
      RT_INFO("GPU: Xenia Vulkan command processor started%s", g_present ? "" : " (headless)");
      g_mirror_running = true;
      std::thread(RegisterMirrorThread).detach();
      std::thread(InterruptThread).detach();
      if (g_capture_seconds) {
        std::thread([] {
          std::filesystem::create_directories("frames");
          for (int n = 0;; ) {
            std::this_thread::sleep_for(std::chrono::seconds(g_capture_seconds));
            char path[64];
            snprintf(path, sizeof(path), "frames/frame_%03d.png", n);
            xgpu::FrameStats st;
            if (xgpu::CaptureFrame(path, &st)) {
              RT_INFO("capture %s: %ux%u, %.1f%% non-black, %u colours", path, st.width, st.height,
                      st.non_black * 100, st.distinct_colors);
              ++n;
            } else {
              RT_INFO("capture: no guest output yet (%llu swaps)", (unsigned long long)g_gpu.swaps);
            }
          }
        }).detach();
      }
      if (g_blood) std::thread(HealthWatchThread).detach();
      if (g_capture_burst) {
        std::thread([] {
          std::filesystem::create_directories("frames");
          std::this_thread::sleep_for(std::chrono::seconds(g_capture_burst));
          for (int n = 0; n < 8; ++n) {
            char path[64];
            snprintf(path, sizeof(path), "frames/burst_%d.png", n);
            xgpu::FrameStats st;
            if (xgpu::CaptureFrame(path, &st)) RT_INFO("capture %s", path);
          }
        }).detach();
      }
      return;
    }
    std::thread(CommandProcessorThread).detach();
    std::thread(InterruptThread).detach();
  });
}

void WriteVideoMode(uint32_t m) {
  memset(G(m), 0, 0x30);
  St32(m + 0x00, 1280);
  St32(m + 0x04, 720);
  St32(m + 0x08, 0);  // interlaced
  St32(m + 0x0C, 1);  // widescreen
  St32(m + 0x10, 1);  // HD
  float hz = 60.0f;
  uint32_t bits;
  memcpy(&bits, &hz, 4);
  St32(m + 0x14, bits);
  St32(m + 0x18, 1);  // NTSC
  St32(m + 0x1C, 0x4A);
  St32(m + 0x20, 0x01);
}

// Fill a guest command block with type-3 NOP packets.
void FillNops(uint32_t ptr, uint32_t dwords) {
  if (!ptr || !dwords) return;
  St32(ptr, (3u << 30) | ((dwords - 2) << 16) | (0x10 << 8));
  for (uint32_t i = 1; i < dwords; ++i) St32(ptr + i * 4, 0);
}

}  // namespace

GUEST_IMPORT(VdQueryVideoMode) { WriteVideoMode(ARG32(3)); RETURN(0); }
GUEST_IMPORT(XGetVideoMode) { WriteVideoMode(ARG32(3)); RETURN(0); }
GUEST_IMPORT(VdQueryVideoFlags) { RETURN(0x7); }  // widescreen | HD | 1080-capable
GUEST_IMPORT(XGetAVPack) { RETURN(6); }           // HDMI

GUEST_IMPORT(VdGetCurrentDisplayInformation) {
  uint32_t i = ARG32(3);
  memset(G(i), 0, 0x58);
  St16(i + 0x00, 1280);
  St16(i + 0x02, 720);
  St16(i + 0x08, 1280);
  St16(i + 0x0A, 720);
  St32(i + 0x40, 1280);
  St32(i + 0x44, 720);
  // Display size, refresh and actual width: D3D's display-mode path reads these
  // (BFBC1 divides by them when it sets up the scaler).
  St16(i + 0x48, 1280);
  St16(i + 0x4A, 720);
  float hz = 60.0f;
  uint32_t hz_bits;
  memcpy(&hz_bits, &hz, 4);
  St32(i + 0x4C, hz_bits);
  St16(i + 0x56, 1280);
  RETURN(0);
}

GUEST_IMPORT(VdGetCurrentDisplayGamma) {
  St32(ARG32(3), 1);
  float g = 2.22222233f;
  uint32_t bits;
  memcpy(&bits, &g, 4);
  St32(ARG32(4), bits);
  RETURN(0);
}

// VdGetSystemCommandBuffer(ptr*, value*)
GUEST_IMPORT(VdGetSystemCommandBuffer) {
  static uint32_t buffer = [] {
    uint32_t b = g_physical_heap->Alloc(0x94000, 0x1000);
    memset(G(b), 0, 0x94000);
    return b;
  }();
  Out32(ARG32(3), buffer);
  Out32(ARG32(4), 0xBEEF0000);
  RETURN(0);
}

// VdInitializeRingBuffer(base, size_log2)
GUEST_IMPORT(VdInitializeRingBuffer) {
  g_gpu.ring_base = PhysToVirt(ARG32(3));
  g_gpu.ring_size_dwords = (1u << (ARG32(4) + 3)) / 4;  // size_log2 counts qwords
  g_gpu.rptr = 0;
  RT_INFO("GPU: ring buffer %08X, %u dwords", g_gpu.ring_base, g_gpu.ring_size_dwords);
  StartGpu();
  if (g_use_xenia_gpu) xgpu::InitializeRingBuffer(ARG32(3), ARG32(4));
  RETURN(0);
}

// VdEnableRingBufferRPtrWriteBack(ptr, block_log2)
GUEST_IMPORT(VdEnableRingBufferRPtrWriteBack) {
  g_gpu.rptr_writeback = PhysToVirt(ARG32(3));
  RT_INFO("GPU: rptr write-back at %08X", g_gpu.rptr_writeback);
  if (g_use_xenia_gpu) {
    StartGpu();
    xgpu::EnableReadPointerWriteBack(ARG32(3), ARG32(4));
  }
  RETURN(0);
}

GUEST_IMPORT(VdSetGraphicsInterruptCallback) {
  g_gpu.interrupt_callback = ARG32(3);
  g_gpu.interrupt_user = ARG32(4);
  RT_INFO("GPU: interrupt callback %08X (user %08X)", ARG32(3), ARG32(4));
  StartGpu();
  RETURN(0);
}

// VdSwap(buffer, fetch, unk2, unk3, unk4, frontbuffer*, format*, color_space*,
//        width*, height*): the guest reserved 64 dwords in the ring for the
// kernel's swap packets. Same scheme as Xenia: write the front buffer's fetch
// constant (virtual address made physical) and Xenia's private XE_SWAP packet.
GUEST_IMPORT(VdSwap) {
  uint32_t buffer = ARG32(3), fetch = ARG32(4);
  uint32_t width_ptr = Ld32(ctx.r1.u32 + 0x54), height_ptr = Ld32(ctx.r1.u32 + 0x5C);
  // r6: 0x94-byte display-change result D3D reads back (flags at +8 and +0x1C,
  // scaler parameters at +0x20). Zero = no change; left as stack garbage, BFBC1
  // takes it as a new scaler setup and divides by a zero width.
  if (uint32_t changes = ARG32(6)) memset(G(changes), 0, 0x94);
  ++g_gpu.swaps;
  ++g_title_swaps;
  if (g_max_fps) {
    // Frame cap (--max-fps). The game's own limiter is told "no limit"
    // (game_patches.cpp): with a limit it runs a simulation tick per frame at
    // that rate, and each tick is 1/30 s of game time.
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    const auto period = std::chrono::nanoseconds(1000000000 / g_max_fps);
    auto now = clock::now();
    if (now > next + period) next = now;  // fell behind: don't catch up
    while ((now = clock::now()) < next) {
      if (next - now > std::chrono::microseconds(1500)) Sleep(1);
      else std::this_thread::yield();
    }
    next += period;
  }
  {
    // Guest frame rate (swaps per second), logged every 5 s.
    static auto window_start = std::chrono::steady_clock::now();
    static uint64_t window_swaps = 0;
    ++window_swaps;
    auto now = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(now - window_start).count();
    if (secs >= 5) {
      RT_INFO("fps: %.1f (%u vblanks so far)", window_swaps / secs, g_gpu.vsync_count);
      window_start = now;
      window_swaps = 0;
    }
  }
  if (!g_use_xenia_gpu || !fetch || !width_ptr || !height_ptr) {
    FillNops(buffer, 64);
    RETURN(0);
  }
  uint32_t dw[6];
  for (int i = 0; i < 6; ++i) dw[i] = Ld32(fetch + i * 4);
  uint32_t front_virtual = dw[1] & 0xFFFFF000u;
  uint32_t front_physical = front_virtual & 0x1FFFFFFFu;
  dw[1] = (dw[1] & 0xFFFu) | front_physical;
  uint32_t o = 0;
  auto put = [&](uint32_t v) { St32(buffer + 4 * o++, v); };
  put((5u << 16) | 0x4800);                    // type 0: SHADER_CONSTANT_FETCH_00_0, 6 regs
  for (uint32_t v : dw) put(v);
  put((3u << 30) | (3u << 16) | (0x64u << 8));  // type 3: PM4_XE_SWAP, 4 dwords
  put(0x53574150);                              // 'SWAP'
  put(front_physical);
  put(Ld32(width_ptr));
  put(Ld32(height_ptr));
  while (o < 64) put(2u << 30);                 // type 2 NOPs
  static bool logged = false;
  if (!logged) {
    logged = true;
    RT_INFO("VdSwap: front buffer %08X (%ux%u)", front_virtual, Ld32(width_ptr), Ld32(height_ptr));
  }
  RETURN(0);
}

// VdInitializeScalerCommandBuffer: argument layout not confirmed yet; write nothing.
GUEST_IMPORT(VdInitializeScalerCommandBuffer) {
  RT_WARN("VdInitializeScalerCommandBuffer: ignored");
  RETURN(0);
}

GUEST_IMPORT(VdSetSystemCommandBufferGpuIdentifierAddress) { RETURN(0); }
GUEST_IMPORT(VdPersistDisplay) { Out32(ARG32(4), 0); RETURN(1); }
GUEST_IMPORT(VdSetDisplayMode) { RETURN(0); }
GUEST_IMPORT(VdInitializeEngines) { RETURN(1); }
GUEST_IMPORT(VdShutdownEngines) { RETURN(0); }
GUEST_IMPORT(VdIsHSIOTrainingSucceeded) { RETURN(1); }
GUEST_IMPORT(VdRetrainEDRAM) { RETURN(0); }
GUEST_IMPORT(VdRetrainEDRAMWorker) { RETURN(0); }
GUEST_IMPORT(VdEnableDisableClockGating) { RETURN(0); }
GUEST_IMPORT(VdCallGraphicsNotificationRoutines) { RETURN(0); }
