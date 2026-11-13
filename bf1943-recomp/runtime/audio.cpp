// Audio imports: XAudio render driver clients (game mixer -> XAudio2) and the XMA
// hardware decoder (Xenia's XmaDecoder + FFmpeg), both behind rt::xapu.
//
// The console's audio driver calls each registered client every 256 samples
// (5.33 ms at 48 kHz); the client mixes and hands back one 6-channel frame with
// XAudioSubmitRenderDriverFrame. A host thread does the calling here, skipping a
// tick when XAudio2 already has enough frames queued.
#include "kernel.h"
#include "xenia_bridge.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>
#endif

#include <algorithm>
#include <bit>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <thread>

using namespace rt;

namespace rt {
std::string g_audio_wav;
}

namespace {
// Output check: frames submitted, how many were not silent, loudest sample, and
// XMA register writes; logged every 10 s. --audio-wav also records front L/R.
std::atomic<uint64_t> g_frames{0}, g_loud_frames{0}, g_xma_writes{0};
std::atomic<float> g_peak{0};
FILE* g_wav = nullptr;
uint32_t g_wav_samples = 0;

void WavHeader() {
  uint32_t data = g_wav_samples * 4, riff = 36 + data;
  uint16_t pcm = 1, ch = 2, bits = 16, align = 4;
  uint32_t fmt_size = 16, rate = 48000, bps = 48000 * 4;
  fseek(g_wav, 0, SEEK_SET);
  fwrite("RIFF", 1, 4, g_wav); fwrite(&riff, 4, 1, g_wav); fwrite("WAVEfmt ", 1, 8, g_wav);
  fwrite(&fmt_size, 4, 1, g_wav); fwrite(&pcm, 2, 1, g_wav); fwrite(&ch, 2, 1, g_wav);
  fwrite(&rate, 4, 1, g_wav); fwrite(&bps, 4, 1, g_wav); fwrite(&align, 2, 1, g_wav);
  fwrite(&bits, 2, 1, g_wav); fwrite("data", 1, 4, g_wav); fwrite(&data, 4, 1, g_wav);
  fseek(g_wav, 0, SEEK_END);
}

void InspectFrame(uint32_t samples) {
  // Guest frame: 6 channels one after another, 256 big-endian floats each.
  float peak = 0;
  int16_t pcm[256 * 2];
  for (int n = 0; n < 256; ++n) {
    float l = std::bit_cast<float>(Ld32(samples + n * 4));
    float r = std::bit_cast<float>(Ld32(samples + (256 + n) * 4));
    for (int c = 0; c < 6; ++c) peak = std::max(peak, std::fabs(std::bit_cast<float>(Ld32(samples + (c * 256 + n) * 4))));
    pcm[n * 2] = int16_t(std::clamp(l, -1.0f, 1.0f) * 32767);
    pcm[n * 2 + 1] = int16_t(std::clamp(r, -1.0f, 1.0f) * 32767);
  }
  ++g_frames;
  if (peak > 1e-4f) ++g_loud_frames;
  if (peak > g_peak.load()) g_peak = peak;
  if (g_wav) {
    fwrite(pcm, sizeof(pcm), 1, g_wav);
    g_wav_samples += 256;
    if ((g_wav_samples & 0x3FFF) == 0) WavHeader();  // keep the file playable if killed
  }
}

void StatsThread() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    float peak = g_peak.exchange(0);
    RT_INFO("audio: %llu frames (%llu not silent), peak %.1f dBFS, %llu XMA register writes",
            (unsigned long long)g_frames.exchange(0), (unsigned long long)g_loud_frames.exchange(0),
            peak > 0 ? 20 * std::log10(peak) : -999.0, (unsigned long long)g_xma_writes.exchange(0));
  }
}

std::once_flag g_audio_started;
bool g_audio_ok = false;

void StartAudio() {
  std::call_once(g_audio_started, [] {
    // --no-audio: as without a sound device (the Linux server): XMA still
    // decodes (the game waits on it), the mixer runs at the console's rate,
    // nothing is played.
    if (g_no_audio) RT_INFO("audio: no output (--no-audio)");
    g_audio_ok = xapu::Start(g_base, [](uint32_t size, uint32_t align) {
      uint32_t a = g_physical_heap->Alloc(size, align < 0x1000 ? 0x1000 : align);
      memset(G(a), 0, size);
      return a;
    });
    if (!g_audio_ok) {
      RT_ERROR("audio: XMA decoder failed to start");
      return;
    }
    // Register block is plain memory for reads: ContextArrayAddress, read with lwbrx.
    *reinterpret_cast<uint32_t*>(G(0x7FEA1800)) = xapu::ContextArray();
    RT_INFO("audio: XMA decoder up, context array at physical %08X", xapu::ContextArray());
    if (!g_audio_wav.empty() && (g_wav = fopen(g_audio_wav.c_str(), "wb"))) WavHeader();
    std::thread(StatsThread).detach();
  });
}

constexpr int kMaxClients = 8;
struct Client {
  std::atomic<bool> active{false};
  uint32_t callback = 0;
  uint32_t wrapped_arg = 0;  // guest dword holding the client's argument
  int output = -1;           // xapu client index, -1 = silent
  uint8_t cpu = 0;           // hardware thread the client registered from
};
Client g_clients[kMaxClients];
std::once_flag g_pump_started;

}  // namespace
void (*rt::g_audio_tick)(PPCContext& ctx) = nullptr;
namespace {

void PumpThread() {
  PPCContext& ctx = AdoptHostThread("audio driver", 0x10000);
  timeBeginPeriod(1);
  using clock = std::chrono::steady_clock;
  constexpr auto kFrame = std::chrono::microseconds(5333);  // 256 samples at 48 kHz
  auto next = clock::now();
  for (;;) {
    next += kFrame;  // (--speed: still in real time; four times as many mixes slowed the server down 4x)
    auto now = clock::now();
    if (next < now - 20 * kFrame) next = now;  // fell far behind (debugger, load): resync
    std::this_thread::sleep_until(next);
    for (Client& c : g_clients) {
      if (!c.active.load(std::memory_order_acquire)) continue;
      // With no output device, still run the mixer at the console's rate.
      if (c.output >= 0 && !xapu::TakeFrameSlot(c.output)) continue;
      // The mixer keeps per-CPU buffers (indexed by KPCR+0x10C), so the callback
      // must run as the hardware thread the client registered from.
      *G<uint8_t>(ctx.r13.u32 + 0x10C) = c.cpu;
      if (rt::g_audio_tick) rt::g_audio_tick(ctx);
      GuestCall(ctx, c.callback, {c.wrapped_arg});
    }
  }
}
}  // namespace

// Called by the PPC_MM_STORE_U32 hook (game/ppc_overrides.h) for stores to 0x7FEA0000.
extern "C" void RtMmioWrite32(uint32_t address, uint32_t value) {
  StartAudio();
  ++g_xma_writes;
  if (g_audio_ok) xapu::WriteRegister(address, value);
}

GUEST_IMPORT(XAudioGetSpeakerConfig) {
  St32(ARG32(3), 0x00010001);  // stereo
  RETURN(0);
}
// XAudioRegisterRenderDriverClient(callback_info* {callback, arg}, driver*)
GUEST_IMPORT(XAudioRegisterRenderDriverClient) {
  uint32_t callback = Ld32(ARG32(3)), arg = Ld32(ARG32(3) + 4);
  if (!callback) RETURN(0x80070057);  // E_INVALIDARG
  StartAudio();
  for (int i = 0; i < kMaxClients; ++i) {
    Client& c = g_clients[i];
    if (c.active) continue;
    c.callback = callback;
    c.wrapped_arg = g_system_heap->Alloc(4, 4);
    St32(c.wrapped_arg, arg);
    c.cpu = *G<uint8_t>(ctx.r13.u32 + 0x10C);
    c.output = g_no_audio ? -1 : xapu::OpenClient();
    if (c.output < 0) RT_WARN("audio: no XAudio2 output, client %d runs silent", i);
    c.active.store(true, std::memory_order_release);
    St32(ARG32(4), 0x41550000 | i);
    RT_INFO("audio: render client %d registered (callback %08X, arg %08X, hardware thread %u)", i, callback, arg, c.cpu);
    std::call_once(g_pump_started, [] { std::thread(PumpThread).detach(); });
    RETURN(0);
  }
  RETURN(0x8007000E);  // E_OUTOFMEMORY
}
GUEST_IMPORT(XAudioUnregisterRenderDriverClient) {
  uint32_t i = ARG32(3) & 0xFFFF;
  if (i < kMaxClients && g_clients[i].active) {
    g_clients[i].active = false;
    xapu::CloseClient(g_clients[i].output);
    g_clients[i].output = -1;
  }
  RETURN(0);
}
// XAudioSubmitRenderDriverFrame(driver, samples): 6 channels x 256 big-endian floats.
GUEST_IMPORT(XAudioSubmitRenderDriverFrame) {
  uint32_t i = ARG32(3) & 0xFFFF;
  if (i == 0) InspectFrame(ARG32(4));
  if (i < kMaxClients && g_clients[i].output >= 0) xapu::SubmitFrame(g_clients[i].output, ARG32(4));
  RETURN(0);
}
GUEST_IMPORT(XAudioGetVoiceCategoryVolumeChangeMask) {
  St32(ARG32(4), 0);
  RETURN(0);
}
GUEST_IMPORT(XAudioGetVoiceCategoryVolume) {
  float one = 1.0f;
  uint32_t bits;
  memcpy(&bits, &one, 4);
  St32(ARG32(4), bits);
  RETURN(0);
}

// ---- XMA ----------------------------------------------------------------------------
#define XMA_IMPORT1(name)                                                          \
  GUEST_IMPORT(name) {                                                             \
    StartAudio();                                                                  \
    RETURN(g_audio_ok ? xapu::name(ARG32(3)) : 0);                                 \
  }
#define XMA_IMPORT2(name)                                                          \
  GUEST_IMPORT(name) {                                                             \
    StartAudio();                                                                  \
    RETURN(g_audio_ok ? xapu::name(ARG32(3), ARG32(4)) : 0);                       \
  }
#define XMA_IMPORT3(name)                                                          \
  GUEST_IMPORT(name) {                                                             \
    StartAudio();                                                                  \
    RETURN(g_audio_ok ? xapu::name(ARG32(3), ARG32(4), ARG32(5)) : 0);             \
  }
XMA_IMPORT1(XMACreateContext)
XMA_IMPORT1(XMAReleaseContext)
XMA_IMPORT2(XMAInitializeContext)
XMA_IMPORT1(XMAIsInputBuffer0Valid)
XMA_IMPORT1(XMAIsInputBuffer1Valid)
XMA_IMPORT1(XMAIsOutputBufferValid)
XMA_IMPORT1(XMAGetOutputBufferReadOffset)
XMA_IMPORT1(XMAGetOutputBufferWriteOffset)
XMA_IMPORT1(XMAGetInputBufferReadOffset)
XMA_IMPORT2(XMASetInputBuffer0Valid)
XMA_IMPORT2(XMASetInputBuffer1Valid)
XMA_IMPORT2(XMASetOutputBufferValid)
XMA_IMPORT2(XMASetOutputBufferReadOffset)
XMA_IMPORT2(XMASetInputBufferReadOffset)
XMA_IMPORT3(XMASetInputBuffer0)
XMA_IMPORT3(XMASetInputBuffer1)
XMA_IMPORT1(XMAEnableContext)
XMA_IMPORT2(XMADisableContext)
