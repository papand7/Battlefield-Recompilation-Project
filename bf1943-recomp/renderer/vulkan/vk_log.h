// Diagnostic logging for the Vulkan renderer.
#pragma once

#include <cstdarg>

namespace bf::vk {

enum class LogLevel { Debug, Info, Warn, Error };

void Log(LogLevel level, const char* fmt, ...);
// Optional log file (in addition to stderr). Pass nullptr to close.
void SetLogFile(const char* path);

}  // namespace bf::vk

#define BF_VK_INFO(...) ::bf::vk::Log(::bf::vk::LogLevel::Info, __VA_ARGS__)
#define BF_VK_WARN(...) ::bf::vk::Log(::bf::vk::LogLevel::Warn, __VA_ARGS__)
#define BF_VK_ERROR(...) ::bf::vk::Log(::bf::vk::LogLevel::Error, __VA_ARGS__)
#define BF_VK_DEBUG(...) ::bf::vk::Log(::bf::vk::LogLevel::Debug, __VA_ARGS__)

// Evaluate a VkResult; log and return false from the enclosing function on failure.
#define BF_VK_CHECK(expr)                                                        \
  do {                                                                           \
    VkResult bf_vk_r_ = (expr);                                                  \
    if (bf_vk_r_ < 0) {                                                          \
      BF_VK_ERROR("%s failed: %s (%s:%d)", #expr, ::bf::vk::ResultName(bf_vk_r_), \
                  __FILE__, __LINE__);                                           \
      return false;                                                              \
    }                                                                            \
  } while (0)
