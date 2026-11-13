// Guest threads: each one is a host thread with its own PPCContext, guest stack,
// processor control region (r13) and X_KTHREAD, plus the TLS imports.
//
// Layout facts used (shared with Xenia's documentation of the 360 kernel):
//   KPCR:    +0x00 tls_ptr, +0x30 self, +0x70 stack_base, +0x74 stack_limit,
//            +0x100 current_thread, +0x10C current_cpu
//   KTHREAD: +0x00 dispatcher header (type 6), +0x5C stack_base, +0x60 stack_limit,
//            +0x68 tls_address, +0x14C thread_id
#include "kernel.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#endif

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <string>
#include <condition_variable>
#include <map>
#include <set>
#include <thread>
#include <vector>

namespace rt {

struct GuestThread : HostObject {
  uint32_t id = 0;
  uint32_t kthread = 0, pcr = 0;
  uint32_t stack_lo = 0, stack_hi = 0;
  uint32_t tls_static = 0;
  uint32_t xapi_startup = 0, start = 0, param = 0;
  std::string name;
  PPCContext ctx{};
  std::mutex suspend_mutex;
  std::condition_variable suspend_cv;
  int suspend_count = 0;
  std::vector<uint32_t> tls_slots;
  HANDLE host = nullptr;  // for --dump-threads
};

namespace {
thread_local GuestThread* t_current = nullptr;
std::atomic<uint32_t> g_next_thread_id{1};
std::mutex g_threads_mutex;
std::vector<std::shared_ptr<GuestThread>> g_threads;

struct ThreadExit {
  uint32_t code;
};

// Current hardware thread (0-5), as guest code reads it: KPCR+0x10C, KTHREAD+0xBF.
// Per-CPU structures (D3D interrupts, the audio mixer's buffers) are indexed by it.
static void SetThreadCpu(GuestThread& t, uint8_t cpu) {
  St8(t.pcr + 0x10C, cpu);
  St8(t.kthread + 0xBF, cpu);
}
// Processor mask (one bit per hardware thread) -> index; 0 = no preference.
static int CpuFromMask(uint32_t mask) {
  mask &= 0x3F;
  return mask ? 31 - std::countl_zero(mask) : -1;
}

std::shared_ptr<GuestThread> NewThread(uint32_t stack_size, const char* name) {
  auto t = std::make_shared<GuestThread>();
  t->id = g_next_thread_id++;
  t->name = name ? name : "guest";
  stack_size = (std::max<uint32_t>(stack_size, 0x10000) + 0xFFF) & ~0xFFFu;
  t->stack_lo = g_system_heap->Alloc(stack_size, 0x1000);
  t->stack_hi = t->stack_lo + stack_size;

  t->pcr = g_system_heap->Alloc(0x1000, 0x1000);
  memset(G(t->pcr), 0, 0x1000);
  t->kthread = g_system_heap->Alloc(0xC00, 0x10);
  memset(G(t->kthread), 0, 0xC00);
  InitDispatcherHeader(t->kthread, kThread, 0);

  // Static TLS block from the XEX template.
  uint32_t tls_size = std::max<uint32_t>(g_module.tls_data_size, 0x10);
  t->tls_static = g_system_heap->Alloc(tls_size, 0x10);
  memset(G(t->tls_static), 0, tls_size);
  if (g_module.tls_raw_address && g_module.tls_raw_size)
    memcpy(G(t->tls_static), G(g_module.tls_raw_address), g_module.tls_raw_size);
  t->tls_slots.assign(std::max<uint32_t>(g_module.tls_slot_count, 64), 0);

  St32(t->pcr + 0x00, t->tls_static);
  St32(t->pcr + 0x30, t->pcr);
  St32(t->pcr + 0x70, t->stack_hi);
  St32(t->pcr + 0x74, t->stack_lo);
  St32(t->pcr + 0x100, t->kthread);
  // Hardware thread: inherited from the creator (as on the console), round robin
  // for host-created threads. ExCreateThread / KeSetAffinityThread override it.
  static std::atomic<uint8_t> next_cpu{0};
  uint8_t cpu = t_current ? *G<uint8_t>(t_current->pcr + 0x10C) : uint8_t(next_cpu++ % 6);
  SetThreadCpu(*t, cpu);
  St32(t->kthread + 0x5C, t->stack_hi);
  St32(t->kthread + 0x60, t->stack_lo);
  St32(t->kthread + 0x68, t->tls_static);
  St32(t->kthread + 0x14C, t->id);

  t->ctx.r1.u64 = t->stack_hi - 0x100;
  t->ctx.r13.u64 = t->pcr;
  std::lock_guard lock(g_threads_mutex);
  g_threads.push_back(t);
  return t;
}

void SetHostThreadName(const std::string& name) {
#ifdef _WIN32
  std::wstring w(name.begin(), name.end());
  SetThreadDescription(GetCurrentThread(), w.c_str());
#else
  pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
#endif
}

void AttachHostThread(GuestThread& t) {
#ifdef _WIN32
  ::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(), ::GetCurrentProcess(), &t.host,
                  THREAD_ALL_ACCESS, FALSE, 0);
#else
  (void)t;
#endif
}

void ThreadMain(std::shared_ptr<GuestThread> t) {
  t_current = t.get();
  AttachHostThread(*t);
  SetHostThreadName(t->name);
  {
    std::unique_lock lock(t->suspend_mutex);
    t->suspend_cv.wait(lock, [&] { return t->suspend_count <= 0; });
  }
  PPCContext& ctx = t->ctx;
  ctx.fpscr.loadFromHost();
  uint32_t code = 0;
  try {
    if (t->xapi_startup) code = uint32_t(GuestCall(ctx, t->xapi_startup, {t->start, t->param}));
    else code = uint32_t(GuestCall(ctx, t->start, {t->param}));
  } catch (const ThreadExit& e) {
    code = e.code;
  }
  RT_INFO("thread %u (%s) exited with %X", t->id, t->name.c_str(), code);
  St32(t->kthread + 0x150, code);  // exit status (informational)
  SetEvent(t->kthread);            // threads signal like notification events
}
}  // namespace

GuestThread* CurrentThread() { return t_current; }
PPCContext* CurrentContext() { return t_current ? &t_current->ctx : nullptr; }
uint32_t CurrentKThread() { return t_current ? t_current->kthread : 0; }
uint32_t CurrentThreadId() { return t_current ? t_current->id : 0; }

uint32_t CreateGuestThread(uint32_t stack_size, uint32_t xapi_startup, uint32_t start,
                           uint32_t param, bool suspended, uint32_t* thread_id, const char* name) {
  auto t = NewThread(stack_size ? stack_size : g_module.stack_size, name);
  t->xapi_startup = xapi_startup;
  t->start = start;
  t->param = param;
  t->suspend_count = suspended ? 1 : 0;
  if (thread_id) *thread_id = t->id;
  uint32_t handle = CreateHandle(t->kthread, t);
  RT_INFO("thread %u created: start %08X param %08X xapi %08X stack %X%s", t->id, start, param,
          xapi_startup, t->stack_hi - t->stack_lo, suspended ? " (suspended)" : "");
  std::thread(ThreadMain, t).detach();
  return handle;
}

void RunMainThread() {
  auto t = NewThread(g_module.stack_size, "main");
  t_current = t.get();
  AttachHostThread(*t);
  SetHostThreadName("guest main");
  PPCContext& ctx = t->ctx;
  ctx.fpscr.loadFromHost();
  RT_INFO("calling entry point %08X", g_module.entry);
  uint32_t code = 0;
  try {
    code = uint32_t(GuestCall(ctx, g_module.entry, {}));
  } catch (const ThreadExit& e) {
    code = e.code;
  }
  RT_INFO("main thread returned %X", code);
  fflush(nullptr);
  ExitProcess(code);
}

PPCContext& AdoptHostThread(const char* name, uint32_t stack_size) {
  auto t = NewThread(stack_size, name);
  t_current = t.get();
  AttachHostThread(*t);
  SetHostThreadName(name);
  t->ctx.fpscr.loadFromHost();
  return t->ctx;
}

uint32_t ResumeThread(GuestThread* t) {
  std::lock_guard lock(t->suspend_mutex);
  int prev = t->suspend_count;
  if (t->suspend_count > 0 && --t->suspend_count == 0) t->suspend_cv.notify_all();
  return uint32_t(prev);
}

#ifdef _WIN32
// --dump-threads: native call stacks of every guest thread, symbolised from the
// PDB, so a hang shows which recompiled sub_ functions each thread sits in.
// Native call stack of a suspended-able thread, symbolised ("a < b < c").
static std::string StackOf(HANDLE h, bool sym) {
  std::string line;
  if (SuspendThread(h) == DWORD(-1)) return line;
  CONTEXT c{};
  c.ContextFlags = CONTEXT_FULL;
  if (GetThreadContext(h, &c)) {
    for (int depth = 0; depth < 48 && c.Rip; ++depth) {
      char buf[sizeof(SYMBOL_INFO) + 256]{};
      auto* si = reinterpret_cast<SYMBOL_INFO*>(buf);
      si->SizeOfStruct = sizeof(SYMBOL_INFO);
      si->MaxNameLen = 255;
      DWORD64 disp = 0;
      if (sym && SymFromAddr(GetCurrentProcess(), c.Rip, &disp, si)) {
        std::string n = si->Name;
        if (n.rfind("__imp__", 0) == 0) n = n.substr(7);
        if (line.size() < n.size() || line.compare(line.size() - n.size(), n.size(), n) != 0)
          line += (line.empty() ? "" : " < ") + n;
      }
      DWORD64 image = 0;
      auto* fe = RtlLookupFunctionEntry(c.Rip, &image, nullptr);
      if (!fe) {  // leaf function
        c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
        c.Rsp += 8;
        continue;
      }
      void* handler_data;
      DWORD64 frame;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, fe, &c, &handler_data, &frame, nullptr);
    }
  }
  ::ResumeThread(h);
  return line;
}

// --dump-threads: native call stacks of every guest thread, symbolised from the
// PDB, so a hang shows which recompiled sub_ functions each thread sits in; then
// the other host threads (GPU command processor, UI, audio...) by name.
void DumpGuestThreads() {
  static std::mutex m;
  std::lock_guard dump_lock(m);
  static bool sym = [] {
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    return SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
  }();
  std::vector<std::shared_ptr<GuestThread>> threads;
  {
    std::lock_guard lock(g_threads_mutex);
    threads = g_threads;
  }
  std::vector<DWORD> seen{GetCurrentThreadId()};
  for (auto& t : threads) {
    if (!t->host || WaitForSingleObject(t->host, 0) == WAIT_OBJECT_0) continue;
    seen.push_back(GetThreadId(t->host));
    RT_INFO("stack of thread %u (%s): %s", t->id, t->name.c_str(), StackOf(t->host, sym).c_str());
  }
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 te{sizeof(te)};
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != GetCurrentProcessId() ||
        std::find(seen.begin(), seen.end(), te.th32ThreadID) != seen.end())
      continue;
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                          te.th32ThreadID);
    if (!h) continue;
    std::string name = "?";
    PWSTR desc = nullptr;
    if (SUCCEEDED(GetThreadDescription(h, &desc)) && desc) {
      char n[128];
      WideCharToMultiByte(CP_UTF8, 0, desc, -1, n, sizeof(n), nullptr, nullptr);
      if (n[0]) name = n;
      LocalFree(desc);
    }
    std::string stack = StackOf(h, sym);
    ::CloseHandle(h);
    RT_INFO("stack of host thread %lu (%s): %s", te.th32ThreadID, name.c_str(), stack.c_str());
  }
  ::CloseHandle(snap);
}

// BF_PROFILE=<seconds> (testing): that long after the start, every guest
// thread's native stack sampled every 5 ms for 15 s; per thread, its samples
// and the functions in most of them (inclusive), to see where a slow frame goes.
// The host's own threads too (the GPU command processor, the presenter,
// drivers'...), numbered 0x80000000 + their Windows thread id: only the
// function they are in (drivers' code can lack the unwind data a stack needs).
const bool g_profile_started = [] {
  const char* e = getenv("BF_PROFILE");
  if (!e) return false;
  int after = atoi(e);
  std::thread([after] {
    std::this_thread::sleep_for(std::chrono::seconds(after));
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    std::map<uint32_t, std::pair<std::string, std::map<std::string, int>>> per;
    std::map<uint32_t, int> samples;
    std::map<uint32_t, std::map<std::string, int>> leaf;
    // The host threads alive now (not the guest ones, not this one).
    std::vector<std::pair<uint32_t, HANDLE>> host;
    std::map<uint32_t, std::string> host_names;
    {
      std::set<DWORD> guest{GetCurrentThreadId()};
      {
        std::lock_guard lock(g_threads_mutex);
        for (auto& t : g_threads)
          if (t->host) guest.insert(GetThreadId(t->host));
      }
      HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
      THREADENTRY32 te{sizeof(te)};
      for (BOOL ok = snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || guest.count(te.th32ThreadID)) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                              te.th32ThreadID);
        if (!h) continue;
        std::string name = "host";
        PWSTR desc = nullptr;
        if (SUCCEEDED(GetThreadDescription(h, &desc)) && desc) {
          char n[128];
          WideCharToMultiByte(CP_UTF8, 0, desc, -1, n, sizeof(n), nullptr, nullptr);
          if (n[0]) name = n;
          LocalFree(desc);
        }
        uint32_t id = 0x80000000u | te.th32ThreadID;
        host.push_back({id, h});
        host_names[id] = name;
      }
      if (snap != INVALID_HANDLE_VALUE) ::CloseHandle(snap);
    }
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < end) {
      for (auto& [id, h] : host) {
        if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0) continue;
        std::string st = "?";
        // (Symbols looked up after it runs again: a thread stopped holding the
        // heap's or the loader's lock would deadlock this one.)
        if (::SuspendThread(h) != DWORD(-1)) {
          CONTEXT c{};
          c.ContextFlags = CONTEXT_CONTROL;
          bool got = ::GetThreadContext(h, &c) != FALSE;
          ::ResumeThread(h);
          char buf[sizeof(SYMBOL_INFO) + 256]{};
          auto* si = reinterpret_cast<SYMBOL_INFO*>(buf);
          si->SizeOfStruct = sizeof(SYMBOL_INFO);
          si->MaxNameLen = 255;
          DWORD64 disp = 0;
          if (got && SymFromAddr(GetCurrentProcess(), c.Rip, &disp, si)) st = si->Name;
        }
        auto& e2 = per[id];
        e2.first = host_names[id];
        samples[id]++;
        std::set<std::string> once;
        leaf[id][st.substr(0, st.find(" < "))]++;
        size_t pos = 0;
        while (pos < st.size()) {
          size_t q = st.find(" < ", pos);
          std::string f = st.substr(pos, q == std::string::npos ? std::string::npos : q - pos);
          if (once.insert(f).second) e2.second[f]++;
          if (q == std::string::npos) break;
          pos = q + 3;
        }
      }
      std::vector<std::shared_ptr<GuestThread>> threads;
      {
        std::lock_guard lock(g_threads_mutex);
        threads = g_threads;
      }
      for (auto& t : threads) {
        if (!t->host || WaitForSingleObject(t->host, 0) == WAIT_OBJECT_0) continue;
        std::string st = StackOf(t->host, true);
        auto& e2 = per[t->id];
        e2.first = t->name;
        samples[t->id]++;
        std::set<std::string> once;
        leaf[t->id][st.substr(0, st.find(" < "))]++;
        size_t pos = 0;
        while (pos < st.size()) {
          size_t q = st.find(" < ", pos);
          std::string f = st.substr(pos, q == std::string::npos ? std::string::npos : q - pos);
          if (once.insert(f).second) e2.second[f]++;
          if (q == std::string::npos) break;
          pos = q + 3;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    for (auto& [id, h] : host) ::CloseHandle(h);
    for (auto& [id, e2] : per) {
      std::vector<std::pair<int, std::string>> top;
      for (auto& [f, n] : e2.second) top.push_back({n, f});
      std::sort(top.rbegin(), top.rend());
      std::string o;
      for (size_t i = 0; i < top.size() && i < 40; ++i)
        o += " " + top[i].second + ":" + std::to_string(top[i].first * 100 / std::max(1, samples[id])) + "%";
      RT_INFO("profile: thread %u (%s), %d samples:%s", id, e2.first.c_str(), samples[id], o.c_str());
      std::vector<std::pair<int, std::string>> lt;
      for (auto& [f, n] : leaf[id]) lt.push_back({n, f});
      std::sort(lt.rbegin(), lt.rend());
      o.clear();
      for (size_t i = 0; i < lt.size() && i < 25; ++i)
        o += " " + lt[i].second + ":" + std::to_string(lt[i].first * 100 / std::max(1, samples[id])) + "%";
      RT_INFO("profile: thread %u leaves:%s", id, o.c_str());
    }
  }).detach();
  return true;
}();

// DEBUG --watch-write addr,size,seconds: logs the recompiled call stack of
// every write to guest [addr, addr+size) (page made read-only, single step).
namespace {
uint8_t* g_watch_page = nullptr;
uint32_t g_watch_addr = 0, g_watch_size = 0;
std::atomic<int> g_watch_hits{0};
std::string SymbolStack(CONTEXT c, int frames) {
  std::string line;
  for (int depth = 0; depth < frames && c.Rip; ++depth) {
    char buf[sizeof(SYMBOL_INFO) + 256]{};
    auto* si = reinterpret_cast<SYMBOL_INFO*>(buf);
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 255;
    DWORD64 disp = 0;
    if (SymFromAddr(GetCurrentProcess(), c.Rip, &disp, si)) {
      std::string n = si->Name;
      if (n.rfind("__imp__", 0) == 0) n = n.substr(7);
      line += (line.empty() ? "" : " < ") + n;
    }
    DWORD64 image = 0;
    auto* fe = RtlLookupFunctionEntry(c.Rip, &image, nullptr);
    if (!fe) {
      c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
      c.Rsp += 8;
      continue;
    }
    void* handler_data;
    DWORD64 frame;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, fe, &c, &handler_data, &frame, nullptr);
  }
  return line;
}
LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* e) {
  DWORD code = e->ExceptionRecord->ExceptionCode;
  static thread_local bool stepping = false;
  if (code == EXCEPTION_ACCESS_VIOLATION && e->ExceptionRecord->NumberParameters >= 2 &&
      e->ExceptionRecord->ExceptionInformation[0] == 1) {
    auto* p = reinterpret_cast<uint8_t*>(e->ExceptionRecord->ExceptionInformation[1]);
    if (p < g_watch_page || p >= g_watch_page + 0x1000) return EXCEPTION_CONTINUE_SEARCH;
    uint32_t guest = uint32_t(p - g_base);
    if (guest + 8 > g_watch_addr && guest < g_watch_addr + g_watch_size && g_watch_hits++ < 40)
      RT_INFO("watch: write %08X (now %08X): %s", guest, Ld32(g_watch_addr & ~3u),
              SymbolStack(*e->ContextRecord, 6).c_str());
    DWORD old;
    VirtualProtect(g_watch_page, 0x1000, PAGE_READWRITE, &old);
    e->ContextRecord->EFlags |= 0x100;  // single step, then protect again
    stepping = true;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (code == EXCEPTION_SINGLE_STEP && stepping) {
    stepping = false;
    DWORD old;
    VirtualProtect(g_watch_page, 0x1000, PAGE_READONLY, &old);
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

void StartWriteWatch(uint32_t addr, uint32_t size, double at) {
  std::thread([=] {
    std::this_thread::sleep_for(std::chrono::milliseconds(int64_t(at * 1000)));
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    g_watch_addr = addr, g_watch_size = size;
    g_watch_page = g_base + (addr & ~0xFFFu);
    AddVectoredExceptionHandler(1, WatchHandler);
    DWORD old;
    VirtualProtect(g_watch_page, 0x1000, PAGE_READONLY, &old);
    RT_INFO("watch: %08X+%u from %.1f s", addr, size, at);
  }).detach();
}
#else
// --dump-threads and --watch-write are Windows debugging aids (DbgHelp, SEH).
void DumpGuestThreads() { RT_WARN("--dump-threads: Windows only"); }
void StartWriteWatch(uint32_t, uint32_t, double) { RT_WARN("--watch-write: Windows only"); }
#endif

}  // namespace rt

using namespace rt;

static GuestThread* ThreadFromHandle(uint32_t handle) {
  if (handle == kCurrentThread) return CurrentThread();
  return dynamic_cast<GuestThread*>(HandleToHost(handle).get());
}

static GuestThread* ThreadFromKThread(uint32_t kthread) {
  std::lock_guard lock(g_threads_mutex);
  for (auto& t : g_threads)
    if (t->kthread == kthread) return t.get();
  return nullptr;
}

// ExCreateThread(handle*, stack_size, thread_id*, xapi_startup, start, param, flags)
GUEST_IMPORT(ExCreateThread) {
  uint32_t id = 0;
  uint32_t flags = ARG32(9);
  uint32_t h = CreateGuestThread(ARG32(4), ARG32(6), ARG32(7), ARG32(8), flags & 1, &id);
  if (int cpu = CpuFromMask(flags >> 24); cpu >= 0) {
    SetThreadCpu(*ThreadFromHandle(h), uint8_t(cpu));
    RT_INFO("thread %u: created on hardware thread %d", id, cpu);
  }
  Out32(ARG32(3), h);
  Out32(ARG32(5), id);
  RETURN(STATUS_SUCCESS);
}

GUEST_IMPORT(ExTerminateThread) { throw ThreadExit{ARG32(3)}; }

GUEST_IMPORT(NtResumeThread) {
  GuestThread* t = ThreadFromHandle(ARG32(3));
  if (!t) RETURN(STATUS_INVALID_HANDLE);
  Out32(ARG32(4), ResumeThread(t));
  RETURN(STATUS_SUCCESS);
}

GUEST_IMPORT(KeResumeThread) {
  GuestThread* t = ThreadFromKThread(ARG32(3));
  RETURN(t ? ResumeThread(t) : 0);
}

GUEST_IMPORT(KeSetBasePriorityThread) { RETURN(0); }
// KeSetAffinityThread(kthread, affinity, previous_affinity*)
GUEST_IMPORT(KeSetAffinityThread) {
  GuestThread* t = ThreadFromKThread(ARG32(3));
  int cpu = CpuFromMask(ARG32(4));
  if (cpu < 0) RETURN(STATUS_INVALID_PARAMETER);
  if (!t) RETURN(STATUS_INVALID_HANDLE);
  Out32(ARG32(5), 1u << *G<uint8_t>(t->pcr + 0x10C));
  SetThreadCpu(*t, uint8_t(cpu));
  RT_INFO("thread %u: affinity -> hardware thread %d", t->id, cpu);
  RETURN(STATUS_SUCCESS);
}
GUEST_IMPORT(NtYieldExecution) {
  std::this_thread::yield();
  RETURN(STATUS_SUCCESS);
}

// KeDelayExecutionThread(mode, alertable, interval*). Delays under 2 ms use a
// high-resolution waitable timer: Sleep() rounds them up to a whole millisecond.
GUEST_IMPORT(KeDelayExecutionThread) {
  int64_t t = int64_t(Ld64(ARG32(5)));
  if (g_time_speed != 1.0) t = int64_t(double(t) / g_time_speed);  // (--speed)
  if (t < 0) {
    int64_t us = -t / 10;
    if (us <= 0) {
      std::this_thread::yield();
#ifdef _WIN32
    } else if (us < 2000) {
      thread_local HANDLE timer =
          CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
      LARGE_INTEGER due;
      due.QuadPart = t;  // relative, 100 ns units
      if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
        WaitForSingleObject(timer, INFINITE);
      else
        std::this_thread::sleep_for(std::chrono::microseconds(us));
#endif
    } else {
      std::this_thread::sleep_for(std::chrono::microseconds(us));
    }
  }
  RETURN(STATUS_SUCCESS);
}

// ---- TLS ------------------------------------------------------------------------
static std::mutex g_tls_mutex;
static std::vector<bool> g_tls_used(64, false);

GUEST_IMPORT(KeTlsAlloc) {
  std::lock_guard lock(g_tls_mutex);
  for (uint32_t i = 0; i < g_tls_used.size(); ++i)
    if (!g_tls_used[i]) {
      g_tls_used[i] = true;
      RETURN(i);
    }
  RETURN(0xFFFFFFFF);
}

GUEST_IMPORT(KeTlsFree) {
  std::lock_guard lock(g_tls_mutex);
  uint32_t i = ARG32(3);
  if (i >= g_tls_used.size()) RETURN(0);
  g_tls_used[i] = false;
  RETURN(1);
}

GUEST_IMPORT(KeTlsGetValue) {
  auto* t = CurrentThread();
  uint32_t i = ARG32(3);
  RETURN(t && i < t->tls_slots.size() ? t->tls_slots[i] : 0);
}

GUEST_IMPORT(KeTlsSetValue) {
  auto* t = CurrentThread();
  uint32_t i = ARG32(3);
  if (!t || i >= t->tls_slots.size()) RETURN(0);
  t->tls_slots[i] = ARG32(4);
  RETURN(1);
}

// A guest TLS slot of another thread (the main one, 1, else the first that
// has it set), and the current thread's set (returning what it was): for
// runtime code calling game code on a thread the game didn't set up for it
// (bot_players.cpp: a per-thread allocator).
namespace rt {
uint32_t TlsFromAnotherThread(uint32_t slot);
uint32_t SetCurrentTls(uint32_t slot, uint32_t value);
}  // namespace rt
uint32_t rt::TlsFromAnotherThread(uint32_t slot) {
  std::lock_guard lock(g_threads_mutex);
  uint32_t any = 0;
  for (auto& t : g_threads) {
    if (slot >= t->tls_slots.size() || !t->tls_slots[slot]) continue;
    if (t->id == 1) return t->tls_slots[slot];
    if (!any) any = t->tls_slots[slot];
  }
  return any;
}
uint32_t rt::SetCurrentTls(uint32_t slot, uint32_t value) {
  auto* t = CurrentThread();
  if (!t || slot >= t->tls_slots.size()) return 0;
  uint32_t old = t->tls_slots[slot];
  t->tls_slots[slot] = value;
  return old;
}

GUEST_IMPORT(KeGetCurrentProcessType) { RETURN(1); }  // title process
