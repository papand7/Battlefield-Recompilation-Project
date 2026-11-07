// Linux console server: the parts the server does without, as no-ops. No GPU
// (the toy GPU path only counts command packets), no audio device, no PC menus
// or key icons, no bots yet (they need shared memory and process launch).
// Built only on Linux; empty on Windows.
#ifndef _WIN32

#include <filesystem>
#include <string>

#include "kernel.h"
#include "pc_settings.h"
#include "posix_input.h"
#include "xenia_bridge.h"

// Guest frames presented (VdSwap); the window title's fps on Windows.
extern "C" std::atomic<uint64_t> g_title_swaps{0};

// ---- GPU bridge: --toy-gpu (forced by --dedicated) never starts it ----------
namespace rt::xgpu {
bool Start(uint8_t*, bool, InterruptFn, uint32_t) { return false; }
void SetOptions(int, uint32_t, const char*) {}
void SetDisplayOptions(int, int, int, int, int) {}
void SetLook(int) {}
void SetHdr(bool, float, float) {}
void SetHdrStretch(bool) {}
void SetOverlay(const uint8_t*, uint32_t, uint32_t) {}
float HdrWhite() { return 0.0f; }
void SetHdrWhite(float) {}
void ApplySceneFx(const char*) {}
bool CameraEye(float*) { return false; }
int MenuRows(float (*)[4], int, int* selected) {
  if (selected) *selected = -1;
  return 0;
}
void AddBackIcon(uint32_t, uint32_t, uint32_t, float, float, float, float) {}
bool OnBackPrompt(float, float) { return false; }
void SetGore(bool, uint32_t, uint32_t) {}
void GoreHit(const float*, float) {}
void GoreDeath(const float*, float, bool, const float*) {}
bool Running() { return false; }
void InitializeRingBuffer(uint32_t, uint32_t) {}
void EnableReadPointerWriteBack(uint32_t, uint32_t) {}
void SetInterruptCallback(uint32_t, uint32_t) {}
void WriteRegister(uint32_t, uint32_t) {}
void Vblank() {}
void TriggerWatches(uint32_t, uint32_t) {}
bool CaptureFrame(const char*, FrameStats*) { return false; }
}  // namespace rt::xgpu

// ---- audio: no device. XMA contexts report their input used up and no
// output, so the game's mixer moves on without sound. -----------------------
namespace rt::xapu {
bool Start(uint8_t*, uint32_t (*)(uint32_t, uint32_t)) { return false; }
uint32_t ContextArray() { return 0; }
void WriteRegister(uint32_t, uint32_t) {}
uint32_t ReadRegister(uint32_t) { return 0; }
uint32_t XMACreateContext(uint32_t) { return 0; }
uint32_t XMAReleaseContext(uint32_t) { return 0; }
uint32_t XMAInitializeContext(uint32_t, uint32_t) { return 0; }
uint32_t XMAIsInputBuffer0Valid(uint32_t) { return 0; }
uint32_t XMAIsInputBuffer1Valid(uint32_t) { return 0; }
uint32_t XMAIsOutputBufferValid(uint32_t) { return 0; }
uint32_t XMAGetOutputBufferReadOffset(uint32_t) { return 0; }
uint32_t XMAGetOutputBufferWriteOffset(uint32_t) { return 0; }
uint32_t XMAGetInputBufferReadOffset(uint32_t) { return 0; }
uint32_t XMASetInputBuffer0Valid(uint32_t, uint32_t) { return 0; }
uint32_t XMASetInputBuffer1Valid(uint32_t, uint32_t) { return 0; }
uint32_t XMASetOutputBufferValid(uint32_t, uint32_t) { return 0; }
uint32_t XMASetOutputBufferReadOffset(uint32_t, uint32_t) { return 0; }
uint32_t XMASetInputBufferReadOffset(uint32_t, uint32_t) { return 0; }
uint32_t XMASetInputBuffer0(uint32_t, uint32_t, uint32_t) { return 0; }
uint32_t XMASetInputBuffer1(uint32_t, uint32_t, uint32_t) { return 0; }
uint32_t XMAEnableContext(uint32_t) { return 0; }
uint32_t XMADisableContext(uint32_t, uint32_t) { return 0; }
int OpenClient() { return -1; }
void CloseClient(int) {}
bool TakeFrameSlot(int) { return false; }
void SubmitFrame(int, uint32_t) {}
}  // namespace rt::xapu

// ---- Xenia GPU shims (texture pack, PBR, scene effects): not on the server --
namespace xe::gpu {
namespace texrep {
size_t SetDirectory(const std::filesystem::path&) { return 0; }
void SetLogMisses(bool) {}
void SetTrace(const std::string&) {}
}  // namespace texrep
namespace pbr {
size_t Load(const std::filesystem::path&) { return 0; }
void SetPomScale(float) {}
}  // namespace pbr
namespace scenefx {
void Configure(const char*) {}
}  // namespace scenefx
}  // namespace xe::gpu

// ---- PC menus, key icons and wording: no window on the server --------------
namespace rt {
void InstallPcUi() {}
void PatchPcText(uint8_t*, size_t) {}
}  // namespace rt
namespace rt::pcset {
void Load() {}
int Key(Action, int) { return 0; }
bool IsOpen() { return false; }
void Open() {}
void OpenHost() {}
void OpenJoin() {}
bool Update(HWND__*, bool) { return false; }
bool ControlsPad() { return false; }
uint16_t PuppetButtons() { return 0; }
}  // namespace rt::pcset

// ---- bots: Windows only for now ----------------------------------------------
namespace rt {
std::string StartBotDirector(int count) {
  if (count) RT_WARN("bots: not on Linux yet; the server runs without them");
  return {};
}
void SetBotSlot(const std::string&, int) {}
bool BotLookAxis(int, float&) { return false; }
bool AddBotInput(XINPUT_GAMEPAD&) { return false; }
int LocalVehicle() { return kOnFoot; }
}  // namespace rt

#endif  // !_WIN32
