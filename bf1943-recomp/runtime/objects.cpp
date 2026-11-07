// Dispatcher objects, waits and handles. Object state lives in the guest's
// X_DISPATCHER_HEADER (so guest code that initialises objects inline, such as
// KeInitializeEvent, stays consistent); one kernel lock serialises state changes.
// Each waiting thread registers on the objects it waits for, and a signal wakes only
// those threads (not every waiter in the process).
#include "kernel.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

namespace rt {

namespace {
std::mutex g_klock;

// Guarded by g_klock. Each thread waits on its own condition variable.
struct Waiter {
  std::condition_variable cv;
};
std::unordered_map<uint32_t, std::vector<Waiter*>> g_waiters;

void Wake(uint32_t o) {
  auto it = g_waiters.find(o);
  if (it == g_waiters.end()) return;
  for (Waiter* w : it->second) w->cv.notify_one();
}

uint8_t Type(uint32_t o) { return *G<uint8_t>(o); }
int32_t State(uint32_t o) { return int32_t(Ld32(o + 4)); }
void SetState(uint32_t o, int32_t v) { St32(o + 4, uint32_t(v)); }

// X_KMUTANT: header, list entry (+0x10), owner thread (+0x18), abandoned (+0x1C).
uint32_t MutantOwner(uint32_t o) { return Ld32(o + 0x18); }

bool Signaled(uint32_t o) {
  if (Type(o) == kMutant) return State(o) > 0 || MutantOwner(o) == CurrentKThread();
  return State(o) > 0;
}

void Acquire(uint32_t o) {
  switch (Type(o)) {
    case kSynchronizationEvent:
    case kSynchronizationTimer: SetState(o, 0); break;
    case kSemaphore: SetState(o, State(o) - 1); break;
    case kMutant:
      SetState(o, State(o) - 1);
      St32(o + 0x18, CurrentKThread());
      break;
    default: break;
  }
}

// Guest timeout -> host deadline. Returns false for "infinite".
bool Deadline(uint32_t timeout_ptr, std::chrono::steady_clock::time_point* out) {
  if (!timeout_ptr) return false;
  int64_t t = int64_t(Ld64(timeout_ptr));
  auto now = std::chrono::steady_clock::now();
  if (t <= 0) {
    *out = now + std::chrono::nanoseconds(-t * 100);
  } else {
    // Absolute FILETIME: compare against the host system time.
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    int64_t host = (int64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    *out = now + std::chrono::nanoseconds(std::max<int64_t>(0, t - host) * 100);
  }
  return true;
}
}  // namespace

void InitDispatcherHeader(uint32_t o, uint8_t type, int32_t signal_state) {
  memset(G(o), 0, 16);
  St8(o, type);
  SetState(o, signal_state);
  St32(o + 8, o + 8);  // empty wait list points at itself
  St32(o + 12, o + 8);
}

void SignalObject(uint32_t o) {
  std::lock_guard lock(g_klock);
  Wake(o);
}

uint32_t SetEvent(uint32_t e) {
  std::lock_guard lock(g_klock);
  int32_t prev = State(e);
  SetState(e, 1);
  Wake(e);
  return uint32_t(prev);
}

uint32_t ResetEvent(uint32_t e) {
  std::lock_guard lock(g_klock);
  int32_t prev = State(e);
  SetState(e, 0);
  return uint32_t(prev);
}

uint32_t PulseEvent(uint32_t e) {
  int32_t prev;
  {
    std::lock_guard lock(g_klock);
    prev = State(e);
    SetState(e, 1);
    Wake(e);
  }
  std::this_thread::yield();
  std::lock_guard lock(g_klock);
  SetState(e, 0);
  return uint32_t(prev);
}

uint32_t ReleaseSemaphore(uint32_t s, int32_t increment, int32_t) {
  std::lock_guard lock(g_klock);
  int32_t prev = State(s);
  int32_t limit = int32_t(Ld32(s + 0x10));
  if (limit > 0 && prev + increment > limit) return STATUS_SEMAPHORE_LIMIT_EXCEEDED | 0x80000000u;
  SetState(s, prev + increment);
  Wake(s);
  return uint32_t(prev);
}

uint32_t ReleaseMutant(uint32_t m, bool abandon, bool) {
  std::lock_guard lock(g_klock);
  int32_t prev = State(m);
  if (!abandon && MutantOwner(m) != CurrentKThread()) return STATUS_MUTANT_NOT_OWNED | 0x80000000u;
  SetState(m, prev + 1);
  if (State(m) > 0) {
    SetState(m, 1);
    St32(m + 0x18, 0);
  }
  Wake(m);
  return uint32_t(prev);
}

uint32_t WaitMultiple(const uint32_t* objs, uint32_t count, bool wait_all, uint32_t timeout_ptr,
                      bool) {
  std::chrono::steady_clock::time_point deadline;
  bool timed = Deadline(timeout_ptr, &deadline);
  // Returns the wait status if the wait is satisfied, or -1.
  auto try_satisfy = [&]() -> int64_t {
    if (wait_all) {
      for (uint32_t i = 0; i < count; ++i)
        if (!Signaled(objs[i])) return -1;
      for (uint32_t i = 0; i < count; ++i) Acquire(objs[i]);
      return STATUS_SUCCESS;
    }
    for (uint32_t i = 0; i < count; ++i)
      if (Signaled(objs[i])) {
        Acquire(objs[i]);
        return STATUS_SUCCESS + i;
      }
    return -1;
  };
  std::unique_lock lock(g_klock);
  if (int64_t r = try_satisfy(); r >= 0) return uint32_t(r);
  if (timed && std::chrono::steady_clock::now() >= deadline) return STATUS_TIMEOUT;
  thread_local Waiter self;
  for (uint32_t i = 0; i < count; ++i) g_waiters[objs[i]].push_back(&self);
  uint32_t result;
  for (;;) {
    if (timed) {
      if (std::chrono::steady_clock::now() >= deadline) { result = STATUS_TIMEOUT; break; }
      self.cv.wait_until(lock, deadline);
    } else {
      // Bounded sleep: guest code can change signal_state without a kernel
      // call (inline KeInitializeEvent on a reused object), so re-check.
      self.cv.wait_for(lock, std::chrono::milliseconds(50));
    }
    if (int64_t r = try_satisfy(); r >= 0) { result = uint32_t(r); break; }
  }
  for (uint32_t i = 0; i < count; ++i) {
    auto& list = g_waiters[objs[i]];
    if (auto it = std::find(list.begin(), list.end(), &self); it != list.end()) list.erase(it);
  }
  return result;
}

// ---- timers ------------------------------------------------------------------
namespace {
struct TimerState {
  std::chrono::steady_clock::time_point due;
  int32_t period_ms = 0;
  uint64_t generation = 0;
};
std::unordered_map<uint32_t, TimerState> g_timers;
std::condition_variable g_timer_cv;
std::once_flag g_timer_thread_once;

void TimerThread() {
  std::unique_lock lock(g_klock);
  for (;;) {
    auto now = std::chrono::steady_clock::now();
    auto next = now + std::chrono::hours(1);
    for (auto& [obj, t] : g_timers) {
      if (t.generation == 0) continue;
      if (t.due <= now) {
        SetState(obj, 1);
        Wake(obj);
        if (t.period_ms > 0) t.due += std::chrono::milliseconds(t.period_ms);
        else t.generation = 0;
      }
      if (t.generation && t.due < next) next = t.due;
    }
    g_timer_cv.wait_until(lock, next);
  }
}
}  // namespace

uint32_t SetTimer(uint32_t timer, uint32_t due_ptr, int32_t period_ms) {
  std::call_once(g_timer_thread_once, [] { std::thread(TimerThread).detach(); });
  std::chrono::steady_clock::time_point due;
  if (!Deadline(due_ptr, &due)) due = std::chrono::steady_clock::now();
  std::lock_guard lock(g_klock);
  auto& t = g_timers[timer];
  bool was_set = t.generation != 0;
  t.due = due;
  t.period_ms = period_ms;
  t.generation++;
  if (t.generation == 0) t.generation = 1;
  SetState(timer, 0);
  g_timer_cv.notify_all();
  return was_set;
}

uint32_t CancelTimer(uint32_t timer) {
  std::lock_guard lock(g_klock);
  auto it = g_timers.find(timer);
  bool was_set = it != g_timers.end() && it->second.generation != 0;
  if (it != g_timers.end()) it->second.generation = 0;
  return was_set;
}

// ---- handles ------------------------------------------------------------------
namespace {
struct HandleEntry {
  uint32_t object;
  std::shared_ptr<HostObject> host;
};
std::mutex g_handle_lock;
std::map<uint32_t, HandleEntry> g_handles;
uint32_t g_next_handle = 0x1000;
}  // namespace

uint32_t CreateHandle(uint32_t object, std::shared_ptr<HostObject> host) {
  std::lock_guard lock(g_handle_lock);
  uint32_t h = g_next_handle;
  g_next_handle += 4;
  g_handles[h] = {object, std::move(host)};
  return h;
}

bool CloseHandle(uint32_t h) {
  std::lock_guard lock(g_handle_lock);
  return g_handles.erase(h) != 0;
}

uint32_t DuplicateHandle(uint32_t h) {
  std::lock_guard lock(g_handle_lock);
  uint32_t object = 0;
  std::shared_ptr<HostObject> host;
  if (h == kCurrentThread) object = CurrentKThread();
  else {
    auto it = g_handles.find(h);
    if (it == g_handles.end()) return 0;
    object = it->second.object;
    host = it->second.host;
  }
  uint32_t n = g_next_handle;
  g_next_handle += 4;
  g_handles[n] = {object, host};
  return n;
}

uint32_t HandleToObject(uint32_t h) {
  if (h == kCurrentThread) return CurrentKThread();
  std::lock_guard lock(g_handle_lock);
  auto it = g_handles.find(h);
  return it == g_handles.end() ? 0 : it->second.object;
}

std::shared_ptr<HostObject> HandleToHost(uint32_t h) {
  std::lock_guard lock(g_handle_lock);
  auto it = g_handles.find(h);
  return it == g_handles.end() ? nullptr : it->second.host;
}

}  // namespace rt
