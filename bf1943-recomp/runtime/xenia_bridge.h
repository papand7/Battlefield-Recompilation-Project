// Narrow interface between the runtime and Xenia's GPU (runtime/xenia_shims/bridge.cc),
// so runtime code never includes Xenia headers.
#pragma once
#include <cstdint>

namespace rt::xgpu {
using InterruptFn = void (*)(uint32_t source, uint32_t cpu);
// Creates the Vulkan graphics system. present: open a window and show frames.
bool Start(uint8_t* guest_base, bool present, InterruptFn interrupt, uint32_t title_id);
// Before Start: forced anisotropic filtering (-1 = game's own, 0..5 = 1x..16x)
// and the texture cache budget in MB (0 = Xenia's default 384/768).
void SetOptions(int anisotropic, uint32_t texture_memory_mb, const char* shader_dump_dir = nullptr);
// Internal resolution (1 = 1280x720 as on the console), presentation effect
// (0 = bilinear, 1 = AMD CAS sharpening, 2 = AMD FSR upscaling + CAS) and the
// window's client size. Before Start.
// shadowmap: the sun shadowmap size, kept at 1x (its render target pitch).
void SetDisplayOptions(int resolution_scale, int effect, int window_width, int window_height, int shadowmap);
// The look of the final frame, HUD included (0 the game's, 1 toon, 2 noir).
void SetLook(int look);
// BRIGHTNESS (-0.3...0.3) and GAMMA (0.7...1.3) of the final frame (live).
void SetPicture(float brightness, float gamma);
// VSync: the swapchain waits for the screen's refresh (from its next creation).
void SetVsync(bool on);
// HDR10 output where the display has it (Windows HDR on), before Start: the
// game's white and the highlights' peak in nits.
void SetHdr(bool on, float paper_white, float peak);
// HDR highlights stretched to the peak (in a level) or not (front end).
void SetHdrStretch(bool on);
// The PC settings page: an RGBA8 image over the whole window (null: none).
void SetOverlay(const uint8_t* rgba, uint32_t width, uint32_t height);
// A small RGBA panel in the window's top-left corner (the FPS counter), its
// size in pixels at 1080 lines (scaled with the window); null hides it.
void SetCornerOverlay(const uint8_t* rgba, uint32_t width, uint32_t height);
// HDR output: the nits of the frame's white now (0 for SDR), and setting it.
float HdrWhite();
void SetHdrWhite(float nits);
// Borderless fullscreen on the window's monitor (or windowed); before the
// window opens it applies when it does. Alt+Enter toggles it too.
void SetFullscreen(bool on);
bool IsFullscreen();
int FullscreenToggles();  // Alt+Enter presses so far (the setting follows them)
// Our explosion smoke and debris (PC settings: EXPLOSION SMOKE).
void SetExplosionEffects(bool on);
// Soft particles in the game's particle shaders (translated from now on).
void SetSoftParticles(bool on);
// The scene effects' settings again ("key=value,..."), if they are on.
void ApplySceneFx(const char* settings);
// The eye (world position) of the level view now; false if none drawn lately.
bool CameraEye(float out[3]);
// The front end menu's rows in the last frame (gpu/vulkan/bf_menu.h): up to max
// boxes {x0, y0, x1, y1} (0-1 of the frame, y down), top to bottom, and the
// highlighted one's index (-1 if no highlighted list is on screen).
int MenuRows(float (*rows)[4], int max, int* selected);// A UI atlas cell holding the Back key icon (pc_ui.cpp), and whether the
// point (0-1 of the frame) is on a Back prompt drawn in the last frame.
void AddBackIcon(uint32_t physical, uint32_t width, uint32_t height, float u0, float v0,
                 float u1, float v1);
bool OnBackPrompt(float x, float y);
// Blood on hit (--blood, any look): on before Start; hits and deaths of
// soldiers at their feet (world position), damage in health points.
// scratch: guest physical memory for the index lists of bodies that came apart.
void SetGore(bool on, uint32_t scratch_physical, uint32_t scratch_size);
void GoreHit(const float feet[3], float damage);
// An explosion at pos (world): smoke, dust and debris.
void ExplosionAt(const float pos[3]);
// push: the direction of the killing blow (0 if unknown).
void GoreDeath(const float feet[3], float damage, bool gib, const float push[3]);
bool Running();
void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2);
void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2);
void SetInterruptCallback(uint32_t callback, uint32_t user_data);
// A CPU write to GPU register `index` (seen by diffing the register block).
void WriteRegister(uint32_t index, uint32_t value);
void Vblank();
// The runtime is about to write guest physical memory itself (file reads).
void TriggerWatches(uint32_t physical_address, uint32_t length);
// Saves the current guest output (what the game last swapped) as a PNG.
// Returns false if nothing has been presented yet. Fills simple pixel stats.
struct FrameStats {
  uint32_t width = 0, height = 0;
  double non_black = 0;        // fraction of pixels that are not near-black
  uint32_t distinct_colors = 0;  // in a coarse 4-bit-per-channel histogram
};
bool CaptureFrame(const char* png_path, FrameStats* stats);
}  // namespace rt::xgpu

// Audio: Xenia's XMA decoder (FFmpeg xmaframes) and XAudio2 output (apu_bridge.cc).
namespace rt::xapu {
// The output's volume (0...1): the master volume, 0 muted in the background.
void SetOutputVolume(float volume);
// alloc_physical returns guest physical-heap memory (for the XMA context array).
bool Start(uint8_t* guest_base, uint32_t (*alloc_physical)(uint32_t size, uint32_t align));
uint32_t ContextArray();
// A guest store to the XMA register block (0x7FEA0000), value as stored by stw.
void WriteRegister(uint32_t address, uint32_t value);
uint32_t ReadRegister(uint32_t address);
// XMA* kernel exports: same arguments and results as on the console.
uint32_t XMACreateContext(uint32_t context_out);
uint32_t XMAReleaseContext(uint32_t context);
uint32_t XMAInitializeContext(uint32_t context, uint32_t init);
uint32_t XMAIsInputBuffer0Valid(uint32_t context);
uint32_t XMAIsInputBuffer1Valid(uint32_t context);
uint32_t XMAIsOutputBufferValid(uint32_t context);
uint32_t XMAGetOutputBufferReadOffset(uint32_t context);
uint32_t XMAGetOutputBufferWriteOffset(uint32_t context);
uint32_t XMAGetInputBufferReadOffset(uint32_t context);
uint32_t XMASetInputBuffer0Valid(uint32_t context, uint32_t unused);
uint32_t XMASetInputBuffer1Valid(uint32_t context, uint32_t unused);
uint32_t XMASetOutputBufferValid(uint32_t context, uint32_t unused);
uint32_t XMASetOutputBufferReadOffset(uint32_t context, uint32_t value);
uint32_t XMASetInputBufferReadOffset(uint32_t context, uint32_t value);
uint32_t XMASetInputBuffer0(uint32_t context, uint32_t buffer, uint32_t packet_count);
uint32_t XMASetInputBuffer1(uint32_t context, uint32_t buffer, uint32_t packet_count);
uint32_t XMAEnableContext(uint32_t context);
uint32_t XMADisableContext(uint32_t context, uint32_t wait);
// Render driver clients: one XAudio2 voice each (48 kHz, 6 channels, 256-sample frames).
int OpenClient();  // -1 on failure
void CloseClient(int index);
bool TakeFrameSlot(int index);  // true if the output queue has room for one more frame
void SubmitFrame(int index, uint32_t guest_samples);
}  // namespace rt::xapu
