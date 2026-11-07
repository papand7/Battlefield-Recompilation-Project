// Loads default.xex (decrypt + decompress through XenonUtils), maps it at its
// base address, builds the indirect-call table and resolves data imports.
#include "kernel.h"

#include <file.h>
#include <image.h>
#include <xex.h>

#include <unordered_map>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#endif
#include "../tools/msvc_compat/crash_diag.h"
#include <unordered_set>

extern std::unordered_map<size_t, const char*> XamExports;
extern std::unordered_map<size_t, const char*> XboxKernelExports;

namespace rt {

ModuleInfo g_module;

// Indirect call to an address that is not a recompiled function start
// (called from the check in game/ppc_overrides.h).
extern "C" void PPCBadIndirectCall(PPCContext& ctx, uint32_t target) {
  RT_ERROR("indirect call to %08X, which is not a recompiled function (r3=%08X r4=%08X r1=%08X)",
           target, ctx.r3.u32, ctx.r4.u32, ctx.r1.u32);
  bf_crash_diag::PrintStack(nullptr);
  Fatal("bad indirect call");
}

static void ResolveImports(const uint8_t* xex, const Image& image) {
  auto* imports = static_cast<const Xex2ImportHeader*>(getOptHeaderPtr(xex, XEX_HEADER_IMPORT_LIBRARIES));
  if (!imports) return;
  std::vector<const char*> names;
  const char* str = reinterpret_cast<const char*>(imports + 1);
  for (size_t i = 0, off = 0; i < imports->numImports; i++) {
    names.push_back(str + off);
    off += ((strlen(str + off) + 1) + 3) & ~size_t(3);
  }
  auto* lib = reinterpret_cast<const Xex2ImportLibrary*>(
      reinterpret_cast<const char*>(imports) + sizeof(Xex2ImportHeader) + imports->sizeOfStringTable);
  int variables = 0;
  std::unordered_set<std::string> function_names;
  for (const auto& sym : image.symbols) function_names.insert(sym.name);
  for (size_t i = 0; i < names.size(); i++) {
    auto* desc = reinterpret_cast<const Xex2ImportDescriptor*>(lib + 1);
    const auto* table = !strcmp(names[i], "xam.xex") ? &XamExports
                        : !strcmp(names[i], "xboxkrnl.exe") ? &XboxKernelExports : nullptr;
    for (size_t k = 0; k < lib->numberOfImports; k++) {
      uint32_t thunk = desc[k].firstThunk;
      // ParseImage byte-swapped the thunk record in its copy; functions were
      // replaced by stubs the recompiler turned into direct calls.
      // Function imports have two records: the thunk (replaced by a stub that
      // starts with a nop, 0x60000000) and an address slot whose name matches
      // a function symbol. Only what is left over is data.
      uint32_t record = *static_cast<const uint32_t*>(image.Find(thunk));
      uint32_t type = record >> 24, ordinal = record & 0xFFFF;
      if (type != 0 || record == 0x00000060) continue;
      const char* name = "?";
      if (table) {
        auto it = table->find(ordinal);
        if (it != table->end()) name = it->second;
      }
      if (function_names.count(name)) continue;
      St32(thunk, ResolveVariableImport(names[i], name, ordinal));
      ++variables;
    }
    lib = reinterpret_cast<const Xex2ImportLibrary*>(
        reinterpret_cast<const char*>(lib + 1) + lib->numberOfImports * sizeof(Xex2ImportDescriptor));
  }
  RT_INFO("loader: %d data imports resolved", variables);
}

bool LoadExecutable(const std::filesystem::path& path) {
  auto file = LoadFile(path);
  if (file.empty()) {
    RT_ERROR("loader: cannot read %s", path.string().c_str());
    return false;
  }
  Image image = Image::ParseImage(file.data(), file.size());
  if (!image.data || image.base != PPC_IMAGE_BASE || image.size > PPC_IMAGE_SIZE) {
    RT_ERROR("loader: %s does not match the recompiled image (base %zX size %X)",
             path.string().c_str(), image.base, image.size);
    return false;
  }
  Commit(PPC_IMAGE_BASE, PPC_IMAGE_SIZE);
  memcpy(G(PPC_IMAGE_BASE), image.data.get(), image.size);

  // Indirect-call table: one host pointer per guest instruction slot.
  const uint32_t table = PPC_IMAGE_BASE + PPC_IMAGE_SIZE;
  Commit(table, uint32_t(PPC_CODE_SIZE * 2));
  size_t count = 0;
  for (auto* m = PPCFuncMappings; m->host; ++m, ++count) PPC_LOOKUP_FUNC(g_base, m->guest) = m->host;
  RT_INFO("loader: image %zX+%X, %zu functions, entry %zX", image.base, image.size, count,
          image.entry_point);

  // Guest copy of the XEX header (RtlImageXexHeaderField, XexGetModuleHandle).
  auto* hdr = reinterpret_cast<const Xex2Header*>(file.data());
  uint32_t header_size = hdr->headerSize;
  g_module.xex_header = g_system_heap->Alloc(header_size, 0x1000);
  memcpy(G(g_module.xex_header), file.data(), header_size);
  g_module.ldr_entry = g_system_heap->Alloc(0x100);
  memset(G(g_module.ldr_entry), 0, 0x100);
  St32(g_module.ldr_entry + 0x18, PPC_IMAGE_BASE);  // DllBase
  St32(g_module.ldr_entry + 0x1C, uint32_t(image.entry_point));
  St32(g_module.ldr_entry + 0x20, image.size);
  St32(g_module.ldr_entry + 0x58, g_module.xex_header);

  g_module.entry = uint32_t(image.entry_point);
  if (auto* stack = static_cast<const be<uint32_t>*>(getOptHeaderPtr(file.data(), XEX_HEADER_DEFAULT_STACK_SIZE)))
    g_module.stack_size = std::max<uint32_t>(stack->get(), 0x40000);
  if (auto* tls = static_cast<const be<uint32_t>*>(getOptHeaderPtr(file.data(), XEX_HEADER_TLS_INFO))) {
    g_module.tls_slot_count = tls[0].get();
    g_module.tls_raw_address = tls[1].get();
    g_module.tls_data_size = tls[2].get();
    g_module.tls_raw_size = tls[3].get();
  }
  RT_INFO("loader: stack %X, tls slots %u data %X (raw %X @ %08X)", g_module.stack_size,
          g_module.tls_slot_count, g_module.tls_data_size, g_module.tls_raw_size,
          g_module.tls_raw_address);

  ResolveImports(file.data(), image);
  return true;
}

}  // namespace rt
