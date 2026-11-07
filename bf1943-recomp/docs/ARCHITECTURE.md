# BF1943 native port: architecture (Vulkan-only)

Battlefield 1943 (XBLA, Frostbite 1.x) running as a native Windows x64 program:
PowerPC code is **statically recompiled** to C++ (XenonRecomp), the Xbox kernel/XAM
APIs are reimplemented natively, and graphics go to **Vulkan 1.3 only**. There is no
D3D11/D3D12 code, no backend abstraction, and no CPU emulation.

```
default.xex ──XenonAnalyse/XenonRecomp──► generated/ppc/*.cpp (ignored, proprietary)
                                              │  guest calls
runtime/  (guest memory, threads, kernel/XAM HLE, file system over original/)
                                              │  D3D calls / ring-buffer writes
renderer/xenos/  Xenos state + shader + texture translation
renderer/vulkan/ Vulkan 1.3 device, frames, swapchain, caches   ──► GPU
```

## Milestone 1: what was found (checked in `research/`)

An earlier note said no reusable Xenos→Vulkan code exists. **That is wrong.**

| Source (licence) | What is reusable | Notes |
|---|---|---|
| **xenia-canary** `src/xenia/gpu` (BSD-3) | Full Xenos→**SPIR-V** shader translator (`spirv_shader_translator*.cc`, ~14k lines incl. ALU, fetch, memexport, render-backend/EDRAM), PM4 command processor, register table, `texture_info_formats`, `texture_address.h` (tiling/untiling), `render_target_cache`, `primitive_processor`, `vulkan/` backend (pipeline cache, texture cache, shared memory, RT cache) | Proven, generic. Tied to xenia `base/`, `ui/vulkan` and its memory/threading model, so it is not drop-in: it has to be cut out with those dependencies. Shipping it means keeping the BSD notice. |
| **XenosRecomp** (MIT) | Offline Xenos microcode → HLSL; DXC turns that into **SPIR-V** (`-spirv`, `dxc_compiler.cpp`) | Narrower, built for static recomp (Unleashed Recompiled). Needs DXC (ships with the Vulkan SDK). |
| **UnleashedRecomp** (GPL-3) | Example of HLE'ing the XDK D3D layer in a recomp (`gpu/video.cpp`), threading/kernel HLE | **GPL-3: reference only, do not copy code** unless the project goes GPL. |
| **plume** (MIT) | Vulkan backend code (`plume_vulkan.cpp`) | It is a multi-backend RHI, which this project rules out. Borrow Vulkan code only, not the abstraction. |
| **XenonRecomp** (MIT) | PPC→C++ recompiler, XEX loader, `ppc_context.h` | The CPU path. |
| VulkanMemoryAllocator (MIT) | Memory allocation | Already integrated. |

### Chosen approach (narrow first, generic as the fallback)

1. **Primary: HLE the game's D3D layer.** Frostbite statically links the XDK D3D
   library. Find its `D3DDevice_*`, `D3D::*` and resolve functions in the recompiled
   code (by pattern, as UnleashedRecomp does), replace them through XenonRecomp's
   function-hook config, and turn the calls into Vulkan commands. This avoids EDRAM
   emulation and most PM4 work.
2. **Shaders:** gather the Xenos microcode the game uploads (`CreateVertexShader` /
   `CreatePixelShader` and shader blobs in the game archives), translate it **offline**
   with XenosRecomp to SPIR-V, and keep a hash→SPIR-V cache. Shaders not seen yet get
   compiled at runtime on a worker thread (async), using Xenia's SPIR-V translator as
   the backup.
3. **Textures:** untile and convert formats using Xenia's `texture_address.h` and
   `texture_info_formats` logic (BSD), in a compute shader where it is worth it. BCn
   stays BCn.
4. **Fallback:** if the game writes PM4 straight into the ring buffer for parts of the
   frame (some engines do), cut out Xenia's command processor, register file and
   render-target cache for those paths. `analysis` decides this before any code is
   written.

### Still missing / to verify
- Whether Frostbite 1.x on 360 goes through D3D for every draw or builds PM4 itself
  (check the call graph of the ring-buffer write functions in the recompiled output).
- Where the shaders live (inside the XEX or in the game archives) and how many there are.
- Memexport and EDRAM resolve paths (shadow maps, post-processing).
- Toolchain: the renderer builds with MSVC; the recompiled code (`game/`) builds with
  clang-cl (LLVM 23, `C:\Program Files\LLVM`).

## CPU recompilation status
- `generated/bf1943_config.toml` (v1.0.6 XEX): helper addresses (savegpr/restgpr/
  savefpr/restfpr/savevmx/restvmx, setjmp/longjmp), switch tables from XenonAnalyse,
  and two leaf functions with no `.pdata` entry declared by hand (`functions = [...]`).
- XenonRecomp output: 76,396 functions, no unrecognised instructions, no runaway
  analysis, no missing switch tables.
- Local changes to `thirdparty/XenonRecomp` (MIT):
  - `XenonAnalyse/function.cpp`: a runaway guard prints `ANALYZE_RUNAWAY base=...`
    instead of running out of memory (`bad_alloc`, the earlier 0xC0000409 crash).
  - `XenonRecomp/recompiler.cpp`: added instructions BF1943 uses that upstream
    lacked: update-form loads/stores (lhzu/lhau/sthu/lfsu/stfsu/lfdu/stfdu and the
    `*ux` forms), bdzf (with the real CR bit), addc, addme, subfze, eqv, mulhd,
    mulhdu, frsqrte, dcbst, vandc, vnor, vsel128, vsrb, vslh, vcmpgtuw,
    vctuxs/vcfpuxws128, vpkswss128. CTR branches (bdz/bdnz/bdzf/bdnzf) whose
    target is outside the function become a conditional tail call, as upstream
    already does for `bc`.
- `tools/msvc_compat/crash_diag.h`: force-include for diagnostic tool builds; prints
  a symbolised stack on abort, terminate or SEH.

### Analysis fixes on top of XenonAnalyse (all regenerable)
- `tools/find_offset_switches.py`: jump tables XenonAnalyse misses: 16-bit/8-bit
  relative tables (`l{b,h}zx` + `nop`/scale) and absolute `lwzx` tables with no bounds
  check next to them (292 tables). Output is appended to XenonAnalyse's list as
  `analysis/bf1943_switch_tables_all.toml`.
- `tools/fix_truncated_functions.py` + `tools/apply_function_fixes.py`: functions
  with no `.pdata` entry (expat, CRT) are cut at their first `bctr`, so every switch
  target past the cut became `// ERROR` + `return` (1297 sites, which broke XML
  parsing silently). The tools extend those functions in the config's `functions`
  list; repeat XenonRecomp until no `// ERROR` is left. Never declare a function
  inside another declared one: that turns jump-table data into "code".
- `game/ppc_overrides.h` (force-included): checks every indirect call and reports the
  guest target if it is not a recompiled function.

## Native runtime (`runtime/`, in progress)
- `memory.cpp`: 4 GB guest space reserved; pages are committed on demand, so wild
  pointers fault and are reported as guest addresses. Heaps: virtual 0x40000000,
  system/kernel 0x70000000, physical 0xA0000000 (GPU physical = addr & 0x1FFFFFFF).
- `loader.cpp`: XEX through XenonUtils, indirect-call table, data imports.
- `threads.cpp`: one host thread per guest thread; per-thread KPCR (r13), KTHREAD,
  static TLS from the XEX template.
- `objects.cpp`: dispatcher objects keep their state in the guest header; one kernel
  lock and condition variable; timers on a helper thread; handle table.
- `xboxkrnl.cpp`, `files.cpp`, `xam.cpp`, `gpu.cpp`: imports. `game:\` maps to
  `original/v106`; input uses host XInput; network is offline. Imports with no
  implementation get logged stubs generated at build time (`tools/gen_import_stubs.py`).
- `gpu.cpp`: the game's XDK D3D writes PM4 into a ring buffer. A minimal command
  processor executes the packets the CPU waits on (memory/event writes, scratch
  write-back, waits, interrupts) and counts draws. Translating draws to Vulkan is next.
- GPU handshake facts: `VdInitializeRingBuffer` size is `1 << (log2 + 3)` bytes;
  GPU memory accesses are little-endian with the swap mode in the address's low
  2 bits; D3D sets SCRATCH_ADDR/UMSK through MMIO; the GPU writes a CPU bitmask to
  scratch0 and raises an interrupt, and D3D's handler clears the bit of the CPU it
  runs on (KPCR+0x10C), so interrupts are delivered once per CPU in the packet's
  mask; the vblank handler only runs when register 0x1951 bit 0 is set.
- Recompiler additions: vupkd3d128 types 3/5 (half floats), vcmpbfp.
- Recompiler config: `non_argument_as_local` and `non_volatile_as_local` are off.
  SEH `__finally` blocks are called with the frame in r12 and read the parent's
  r14-r31, which host locals break (first crash, at sub_827DD350).

## GPU: Xenia's Vulkan backend (milestone 3/4)
The game's statically linked XDK D3D emits PM4, so the renderer works at the PM4
level, like Xenia. `thirdparty/xenia/` holds the needed parts of xenia-canary
(BSD-3, LICENSE kept): `base/`, `gpu/` (no D3D12/DXBC/trace), `gpu/vulkan/`, `ui/`,
`ui/vulkan/`, glslang's `SPIRV/` builder, and header-only fmt, xxhash, utfcpp,
xbyak, disruptorplus, cxxopts, date, renderdoc, stb, FidelityFX CAS/FSR. Changes
to vendored files are marked `bf1943:` (clang constexpr fix, no ImGui include,
discrete GPU first, no guest stack traces).
- `tools/build_xenia_shaders.py`: Xenia's internal shaders -> SPIR-V headers in
  `build/xenia_shaders` (SDK glslangValidator + spirv-opt).
- `runtime/xenia_shims/`: stand-ins for what Xenia's GPU expects from its emulator:
  `xe::Memory` (physical memory = guest 0xA0000000 window; CPU-write detection by
  page protection + vectored handler), `GraphicsSystem`, kernel/cpu/apu stubs, and
  `bridge.cc` (the runtime's only entry point, `runtime/xenia_bridge.h`).
- `runtime/gpu.cpp`: `VdSwap` writes Xenia's XE_SWAP packet with the front-buffer
  fetch constant (virtual address made physical). A mirror loop forwards CPU writes
  to the GPU register block. `--toy-gpu` keeps the old counting-only processor.
- Verification: `--capture N` saves the presented guest output as PNG every N
  seconds and logs non-black fraction and colour count.

## Vulkan foundation (Milestone 2, done, tested)

`renderer/vulkan/`:
- `vk_device`: 1.3 instance/device, picks the discrete GPU, validation layer + debug
  messenger counting errors, VMA (memory budget, BDA), optional caps (descriptor
  indexing, descriptor buffer, EDS3, fragment shader interlock for the ROV-style
  EDRAM path, f16/i8), **persistent pipeline cache** with a header check against the
  device/driver UUID and an atomic save.
- `vk_frame`: 2 frames in flight, one transient command pool per frame, a **single
  timeline semaphore** for pacing (no fences), `vkQueueSubmit2`.
- `vk_swapchain`: Win32 surface, UNORM format (Xenos output is already gamma-encoded),
  FIFO or mailbox, recreated on resize/out-of-date, one present semaphore per image.
- Dynamic rendering + synchronization2 everywhere (no render-pass objects).

Tests (`ctest` in `build/vk`):
- `vk_headless_test`: draws a triangle offscreen, reads pixels back and checks them
  (centre 255,0,0,255 and corner 0,0,255,255), requires 0 validation errors, and
  reloads the pipeline cache on the second run.
- `vk_window_test`: presents 120 swapchain frames, requires 0 validation errors.

## Optimisation plan (Milestone 6, designed for now)
Offline shader precompile plus async compile of unseen variants; the pipeline cache
already persists; bindless texture array when `descriptor_indexing` is present;
ring-buffer uploads through VMA; timeline-semaphore pacing instead of `vkDeviceWaitIdle`;
secondary command buffers only if profiling shows recording is the bottleneck.

## Later enhancements (after the original image is correct)
Internal resolution scale and ultrawide (viewport/projection patches through hooks),
bigger shadow maps, anisotropic filtering, texture-pack replacement keyed on the
texture hash, post-processing pass after the game's own resolve. All optional and
off by default.

## Repository rules
`original/` (game files) and `generated/` (recompiled output) are git-ignored and
never published. `research/` holds upstream clones for reference only.
