// bf1943: runtime side of Xenia's audio (see runtime/xenia_bridge.h, rt::xapu).
// XMA hardware: Xenia's XmaDecoder with the FFmpeg xmaframes decoder. The XMA*
// kernel exports below follow Xenia's xboxkrnl_audio_xma.cc (BSD-3).
// Output: Xenia's XAudio2 driver, one per registered render client.
#include "../xenia_bridge.h"

#include <cstdio>
#include <cstring>
#include <memory>

#ifdef _WIN32
#include "xenia/apu/xaudio2/xaudio2_audio_driver.h"
#elif defined(BF1943_CLIENT)
#include <SDL3/SDL.h>  // (the Linux game's sound: an SDL3 audio stream per client)
#endif
#include "xenia/apu/xma_context.h"
#include "xenia/apu/xma_decoder.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/memory.h"

namespace rt::xapu {
namespace {
using xe::apu::XMA_CONTEXT_DATA;

std::unique_ptr<xe::cpu::Processor> g_processor;
std::unique_ptr<xe::kernel::KernelState> g_kernel;
std::unique_ptr<xe::apu::XmaDecoder> g_xma;
xe::Memory* g_mem = nullptr;

constexpr int kMaxClients = 8;
constexpr int kQueuedFrames = 64;  // Xenia's kMaximumQueuedFrames
struct Client {
  std::unique_ptr<xe::threading::Semaphore> semaphore;
#ifdef _WIN32
  std::unique_ptr<xe::apu::xaudio2::XAudio2AudioDriver> driver;
#elif defined(BF1943_CLIENT)
  // The Linux game: 6 channels (front L/R, centre, LFE, rear L/R: SDL's order
  // too, mixed down by SDL for the device), 32-bit float, 48 kHz.
  struct Stream {
    explicit Stream(SDL_AudioStream* stream) : s(stream) {}
    Stream(const Stream&) = delete;
    ~Stream() { SDL_DestroyAudioStream(s); }
    SDL_AudioStream* s;
  };
  std::unique_ptr<Stream> driver;
#else
  std::unique_ptr<int> driver;  // no sound device (the Linux server)
#endif
};
Client g_clients[kMaxClients];

uint8_t* Ptr(uint32_t guest) { return g_mem->TranslateVirtual(guest); }
uint32_t Phys(uint32_t guest) { return guest ? g_mem->GetPhysicalAddress(guest) : 0; }

// Writes the Kick/Lock/Clear bit of a context, as XMAEnableContext and friends do.
void StoreIndexedRegister(uint32_t base_reg, uint32_t context) {
  uint32_t hw_index =
      (Phys(context) - g_xma->context_array_ptr()) / sizeof(XMA_CONTEXT_DATA);
  g_xma->WriteRegister(base_reg + (hw_index >> 5) * 4, xe::byte_swap(1u << (hw_index & 31)));
}

struct XMA_LOOP_DATA {
  xe::be<uint32_t> loop_start, loop_end;
  uint8_t loop_count, loop_subframe_end, loop_subframe_skip;
};
struct XMA_CONTEXT_INIT {
  xe::be<uint32_t> input_buffer_0_ptr, input_buffer_0_packet_count;
  xe::be<uint32_t> input_buffer_1_ptr, input_buffer_1_packet_count;
  xe::be<uint32_t> input_buffer_read_offset;
  xe::be<uint32_t> output_buffer_ptr, output_buffer_block_count;
  xe::be<uint32_t> work_buffer, subframe_decode_count, channel_count, sample_rate;
  XMA_LOOP_DATA loop_data;
};
static_assert(sizeof(XMA_CONTEXT_INIT) == 56);
}  // namespace

bool Start(uint8_t* guest_base, uint32_t (*alloc_physical)(uint32_t size, uint32_t align)) {
  // Shares the GPU bridge's Memory when it exists (one watch table).
  g_mem = xe::GetMemory();
  static std::unique_ptr<xe::Memory> own;
  if (!g_mem) {
    own = std::make_unique<xe::Memory>(guest_base);
    g_mem = own.get();
  }
  xe::Memory::system_heap_alloc = alloc_physical;
  g_processor = std::make_unique<xe::cpu::Processor>(g_mem);
  g_kernel = std::make_unique<xe::kernel::KernelState>(g_mem);
  g_xma = std::make_unique<xe::apu::XmaDecoder>(g_processor.get());
  return g_xma->Setup(g_kernel.get()) == 0;
}

uint32_t ContextArray() { return g_xma ? g_xma->context_array_ptr() : 0; }

// MMIO at 0x7FEA0000: value as a plain stw would store it (Xenia's convention).
void WriteRegister(uint32_t address, uint32_t value) { g_xma->WriteRegister(address, value); }
uint32_t ReadRegister(uint32_t address) { return g_xma->ReadRegister(address); }

uint32_t XMACreateContext(uint32_t context_out) {
  uint32_t c = g_xma->AllocateContext();
  xe::store_and_swap<uint32_t>(Ptr(context_out), c);
  return c ? 0 : 0xC0000017;  // X_STATUS_NO_MEMORY
}
uint32_t XMAReleaseContext(uint32_t c) {
  g_xma->ReleaseContext(c);
  return 0;
}
uint32_t XMAInitializeContext(uint32_t c, uint32_t init_ptr) {
  auto* init = reinterpret_cast<const XMA_CONTEXT_INIT*>(Ptr(init_ptr));
  std::memset(Ptr(c), 0, sizeof(XMA_CONTEXT_DATA));
  XMA_CONTEXT_DATA x(Ptr(c));
  x.input_buffer_0_ptr = Phys(init->input_buffer_0_ptr);
  x.input_buffer_0_packet_count = init->input_buffer_0_packet_count;
  x.input_buffer_1_ptr = Phys(init->input_buffer_1_ptr);
  x.input_buffer_1_packet_count = init->input_buffer_1_packet_count;
  x.input_buffer_read_offset = init->input_buffer_read_offset;
  x.output_buffer_ptr = Phys(init->output_buffer_ptr);
  x.output_buffer_block_count = init->output_buffer_block_count;
  x.subframe_decode_count = init->subframe_decode_count;
  x.is_stereo = init->channel_count >= 1;
  x.sample_rate = init->sample_rate;
  x.loop_start = init->loop_data.loop_start;
  x.loop_end = init->loop_data.loop_end;
  x.loop_count = init->loop_data.loop_count;
  x.loop_subframe_end = init->loop_data.loop_subframe_end;
  x.loop_subframe_skip = init->loop_data.loop_subframe_skip;
  x.Store(Ptr(c));
  StoreIndexedRegister(0x1A80, c);  // Clear
  return 0;
}

#define XMA_FIELD_GET(name, field)              \
  uint32_t name(uint32_t c) {                   \
    return XMA_CONTEXT_DATA(Ptr(c)).field;      \
  }
#define XMA_FIELD_SET(name, field, value_expr)  \
  uint32_t name(uint32_t c, uint32_t value) {   \
    (void)value;                                \
    XMA_CONTEXT_DATA x(Ptr(c));                 \
    x.field = (value_expr);                     \
    x.Store(Ptr(c));                            \
    return 0;                                   \
  }
XMA_FIELD_GET(XMAIsInputBuffer0Valid, input_buffer_0_valid)
XMA_FIELD_GET(XMAIsInputBuffer1Valid, input_buffer_1_valid)
XMA_FIELD_GET(XMAIsOutputBufferValid, output_buffer_valid)
XMA_FIELD_GET(XMAGetOutputBufferReadOffset, output_buffer_read_offset)
XMA_FIELD_GET(XMAGetOutputBufferWriteOffset, output_buffer_write_offset)
XMA_FIELD_GET(XMAGetInputBufferReadOffset, input_buffer_read_offset)
XMA_FIELD_SET(XMASetInputBuffer0Valid, input_buffer_0_valid, 1)
XMA_FIELD_SET(XMASetInputBuffer1Valid, input_buffer_1_valid, 1)
XMA_FIELD_SET(XMASetOutputBufferValid, output_buffer_valid, 1)
XMA_FIELD_SET(XMASetOutputBufferReadOffset, output_buffer_read_offset, value)
XMA_FIELD_SET(XMASetInputBufferReadOffset, input_buffer_read_offset, value)

uint32_t XMASetInputBuffer0(uint32_t c, uint32_t buffer, uint32_t packet_count) {
  XMA_CONTEXT_DATA x(Ptr(c));
  x.input_buffer_0_ptr = Phys(buffer);
  x.input_buffer_0_packet_count = packet_count;
  x.Store(Ptr(c));
  return 0;
}
uint32_t XMASetInputBuffer1(uint32_t c, uint32_t buffer, uint32_t packet_count) {
  XMA_CONTEXT_DATA x(Ptr(c));
  x.input_buffer_1_ptr = Phys(buffer);
  x.input_buffer_1_packet_count = packet_count;
  x.Store(Ptr(c));
  return 0;
}
uint32_t XMAEnableContext(uint32_t c) {
  StoreIndexedRegister(0x1940, c);  // Kick
  return 0;
}
uint32_t XMADisableContext(uint32_t c, uint32_t wait) {
  StoreIndexedRegister(0x1A40, c);  // Lock
  return g_xma->BlockOnContext(c, !wait) ? 0 : 1;  // X_E_FALSE
}

// ---- render driver clients -------------------------------------------------------
int OpenClient() {
#if !defined(_WIN32) && !defined(BF1943_CLIENT)
  return -1;  // no sound device: the client runs silent (audio.cpp)
#endif
  for (int i = 0; i < kMaxClients; ++i) {
    Client& cl = g_clients[i];
    if (cl.driver) continue;
    cl.semaphore = xe::threading::Semaphore::Create(kQueuedFrames, kQueuedFrames);
#ifdef _WIN32
    cl.driver = std::make_unique<xe::apu::xaudio2::XAudio2AudioDriver>(cl.semaphore.get());
    if (!cl.driver->Initialize()) {
#elif defined(BF1943_CLIENT)
    SDL_InitSubSystem(SDL_INIT_AUDIO);
    SDL_AudioSpec spec{SDL_AUDIO_F32, 6, 48000};
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (stream) {
      SDL_ResumeAudioStreamDevice(stream);
      cl.driver = std::make_unique<Client::Stream>(stream);
    } else {
      fprintf(stderr, "[xapu] no sound device: %s\n", SDL_GetError());
    }
    if (!cl.driver) {
#else
    if (true) {
#endif
      cl.driver.reset();
      cl.semaphore.reset();
      return -1;
    }
    return i;
  }
  return -1;
}
void CloseClient(int index) {
  if (index < 0 || index >= kMaxClients || !g_clients[index].driver) return;
#ifdef _WIN32
  g_clients[index].driver->Shutdown();
#endif
  g_clients[index].driver.reset();
  g_clients[index].semaphore.reset();
}
bool TakeFrameSlot(int index) {
#if !defined(_WIN32) && defined(BF1943_CLIENT)
  // Room while under kQueuedFrames' worth... kept to 8 frames (43 ms): the
  // stream's queue is the device's latency.
  constexpr int kFrameBytes = 256 * 6 * 4;
  auto& d = g_clients[index].driver;
  return d && SDL_GetAudioStreamQueued(d->s) < 8 * kFrameBytes;
#endif
  auto& sem = g_clients[index].semaphore;
  return sem && xe::threading::Wait(sem.get(), false, std::chrono::milliseconds(0)) ==
                    xe::threading::WaitResult::kSuccess;
}
void SubmitFrame(int index, uint32_t samples) {
#ifdef _WIN32
  if (index >= 0 && index < kMaxClients && g_clients[index].driver)
    g_clients[index].driver->SubmitFrame(reinterpret_cast<float*>(Ptr(samples)));
#elif defined(BF1943_CLIENT)
  if (index >= 0 && index < kMaxClients && g_clients[index].driver) {
    SDL_AudioStream* stream = g_clients[index].driver->s;
    // The guest's frame: 6 channels one after another, 256 big-endian floats
    // each -> interleaved, host order.
    const uint32_t* in = reinterpret_cast<const uint32_t*>(Ptr(samples));
    float out[256 * 6];
    for (int n = 0; n < 256; ++n)
      for (int c = 0; c < 6; ++c) {
        uint32_t u = xe::byte_swap(in[c * 256 + n]);
        std::memcpy(&out[n * 6 + c], &u, 4);
      }
    SDL_PutAudioStreamData(stream, out, sizeof(out));
  }
#else
  (void)index, (void)samples;
#endif
}

}  // namespace rt::xapu
