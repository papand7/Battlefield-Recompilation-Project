// bf1943: Xenia's profiler is not built (no microprofile); calls are no-ops.
#include "xenia/base/profiling.h"
namespace xe {
void Profiler::Flip() {}
}  // namespace xe
