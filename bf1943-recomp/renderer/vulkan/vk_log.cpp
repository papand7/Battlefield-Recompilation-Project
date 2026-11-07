#include "vk_log.h"

#include <chrono>
#include <cstdio>
#include <mutex>

namespace bf::vk {
namespace {
std::mutex g_mutex;
FILE* g_file = nullptr;
const auto g_start = std::chrono::steady_clock::now();

const char* LevelName(LogLevel l) {
  switch (l) {
    case LogLevel::Debug: return "D";
    case LogLevel::Info: return "I";
    case LogLevel::Warn: return "W";
    case LogLevel::Error: return "E";
  }
  return "?";
}
}  // namespace

void SetLogFile(const char* path) {
  std::lock_guard lock(g_mutex);
  if (g_file) fclose(g_file);
  g_file = path ? fopen(path, "w") : nullptr;
}

void Log(LogLevel level, const char* fmt, ...) {
  char msg[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
  std::lock_guard lock(g_mutex);
  fprintf(stderr, "[%9.3f][vk][%s] %s\n", t, LevelName(level), msg);
  if (g_file) {
    fprintf(g_file, "[%9.3f][vk][%s] %s\n", t, LevelName(level), msg);
    fflush(g_file);
  }
}

}  // namespace bf::vk
