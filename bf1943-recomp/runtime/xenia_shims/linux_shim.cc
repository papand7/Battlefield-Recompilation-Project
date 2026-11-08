// Linux server: the pieces of Xenia's shims that are Windows-specific
// elsewhere (graphics_system_shim.cc, base/system_win.cc). Empty on Windows.
#ifndef _WIN32
#include <pthread.h>

#include <cstdio>
#include <string>
#include <string_view>
#include <thread>

#include "xenia/base/system.h"
#include "xenia/kernel/xthread.h"

namespace xe {
namespace kernel {
void XHostThread::Create() {
  thread_ = std::thread([this] {
    id_ = std::this_thread::get_id();
    pthread_setname_np(pthread_self(), name_.substr(0, 15).c_str());
    fn_();
  });
  while (id_ == std::thread::id()) std::this_thread::yield();
}
}  // namespace kernel

// No desktop on the server: to the console instead.
void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message) {
  fprintf(stderr, "[%s] %.*s\n", type == SimpleMessageBoxType::Error ? "error" : "note", int(message.size()),
          message.data());
}
}  // namespace xe
#endif  // !_WIN32
