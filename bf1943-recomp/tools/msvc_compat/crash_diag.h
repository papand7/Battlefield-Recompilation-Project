// Force-included (/FI) into diagnostic builds of the tools: on abort,
// std::terminate or an SEH crash, prints a symbolised stack trace to stderr.
// Also makes stdout/stderr unbuffered so progress output survives a fail-fast.
#pragma once
#if defined(_MSC_VER) && defined(__cplusplus)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <typeinfo>
#pragma comment(lib, "dbghelp.lib")

namespace bf_crash_diag {

inline void PrintStack(CONTEXT* ctx_in) {
  HANDLE proc = GetCurrentProcess();
  SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(proc, nullptr, TRUE);
  CONTEXT ctx;
  if (ctx_in) ctx = *ctx_in; else RtlCaptureContext(&ctx);
  STACKFRAME64 f{};
  f.AddrPC.Offset = ctx.Rip;
  f.AddrFrame.Offset = ctx.Rbp;
  f.AddrStack.Offset = ctx.Rsp;
  f.AddrPC.Mode = f.AddrFrame.Mode = f.AddrStack.Mode = AddrModeFlat;
  alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512];
  for (int i = 0; i < 64; ++i) {
    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &f, &ctx, nullptr,
                     SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
        !f.AddrPC.Offset)
      break;
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 511;
    DWORD64 disp = 0;
    IMAGEHLP_LINE64 line{sizeof(line)};
    DWORD ldisp = 0;
    bool has_sym = SymFromAddr(proc, f.AddrPC.Offset, &disp, sym);
    bool has_line = SymGetLineFromAddr64(proc, f.AddrPC.Offset, &ldisp, &line);
    fprintf(stderr, "  #%02d %016llx %s", i, f.AddrPC.Offset, has_sym ? sym->Name : "?");
    if (has_line) fprintf(stderr, "  %s:%lu", line.FileName, line.LineNumber);
    fputc('\n', stderr);
  }
  fflush(stderr);
}

inline LONG WINAPI OnSeh(EXCEPTION_POINTERS* ep) {
  fprintf(stderr, "\n*** SEH exception 0x%08lx at %p\n", ep->ExceptionRecord->ExceptionCode,
          ep->ExceptionRecord->ExceptionAddress);
  PrintStack(ep->ContextRecord);
  return EXCEPTION_EXECUTE_HANDLER;
}

inline void OnAbort(int) {
  fprintf(stderr, "\n*** abort()\n");
  PrintStack(nullptr);
  _exit(3);
}

inline void OnTerminate() {
  fprintf(stderr, "\n*** std::terminate");
  if (auto e = std::current_exception()) {
    try { std::rethrow_exception(e); }
    catch (const std::exception& x) { fprintf(stderr, ": %s: %s", typeid(x).name(), x.what()); }
    catch (...) { fprintf(stderr, ": non-std exception"); }
  }
  fputc('\n', stderr);
  PrintStack(nullptr);
  _exit(3);
}

inline const bool kInstalled = [] {
  setvbuf(stdout, nullptr, _IONBF, 0);
  setvbuf(stderr, nullptr, _IONBF, 0);
  SetUnhandledExceptionFilter(OnSeh);
  // No vectored continue handler: it also fires for faults that were handled
  // (the GPU's write-watch pages), which made the log useless.
  signal(SIGABRT, OnAbort);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  std::set_terminate(OnTerminate);
  return true;
}();

}  // namespace bf_crash_diag
#elif defined(__linux__) && defined(__cplusplus)
// Linux: the native stack from glibc's backtrace (symbols with -rdynamic).
#include <execinfo.h>
namespace bf_crash_diag {
inline void PrintStack(void*) {
  void* frames[64];
  int n = backtrace(frames, 64);
  backtrace_symbols_fd(frames, n, 2);
}
}  // namespace bf_crash_diag
#endif
