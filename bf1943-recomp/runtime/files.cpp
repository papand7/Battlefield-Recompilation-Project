// File system imports. Guest paths (game:\, d:\, \Device\Cdrom0\, cache:\ ...)
// are mapped onto host folders: the game root is read-only game data, the
// user root holds writable devices. Every open is logged while bring-up is going on.
#include "kernel.h"
#include "xenia_bridge.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include "posix_files.h"  // the Win32 file calls below, on POSIX
#define GetLastError() GetLastErrorFile()
#endif

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace rt {
std::filesystem::path g_game_root;
std::filesystem::path g_user_root;
std::atomic<bool> g_in_menus{true};
std::atomic<bool> g_mainmenu_open{false};  // set once the main menu (mainmenu-00) file is opened
std::atomic<bool> g_level_menu{false};
}  // namespace rt

using namespace rt;

namespace {

struct FileObject : HostObject {
  HANDLE h = INVALID_HANDLE_VALUE;
  bool directory = false;
  bool overlay = false;  // build_overlay-00.fbrb: string tables get PC wording
  std::filesystem::path path;
  std::string guest_path;
  HANDLE find = INVALID_HANDLE_VALUE;  // directory enumeration
  ~FileObject() override {
    if (find != INVALID_HANDLE_VALUE) FindClose(find);
    if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
  }
};

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(tolower(c)); });
  return s;
}

std::mutex g_content_mutex;
std::vector<std::pair<std::string, std::filesystem::path>> g_content_roots;  // lower-case root

// A guest path's rest ("dist\\xenon\\x.fbrb") as a host path: on Linux the
// backslashes would stay part of the name.
std::filesystem::path HostRest(std::string rest) {
#ifndef _WIN32
  std::replace(rest.begin(), rest.end(), '\\', '/');
#endif
  return rest;
}

// Returns an empty path for devices we do not know.
std::filesystem::path MapPath(std::string guest) {
  std::replace(guest.begin(), guest.end(), '/', '\\');
  std::string low = Lower(guest);
  struct Prefix { const char* guest; bool game; };
  static const Prefix kPrefixes[] = {
      {"game:\\", true}, {"d:\\", true}, {"\\device\\cdrom0\\", true}, {"\\??\\game:\\", true},
      {"\\??\\d:\\", true}, {"game:", true}, {"d:", true},
      {"cache:\\", false}, {"\\device\\harddisk0\\cache0\\", false}, {"update:\\", false},
      {"\\device\\harddisk0\\partition1\\", false}, {"hdd:\\", false}, {"e:\\", false},
  };
  {
    // Content roots mounted by XamContentCreateEx ("save:\...").
    std::lock_guard lock(g_content_mutex);
    for (auto& [root, host] : g_content_roots) {
      if (low.compare(0, root.size(), root) != 0 || low.size() <= root.size() || low[root.size()] != ':')
        continue;
      std::string rest = guest.substr(root.size() + 1);
      while (!rest.empty() && rest[0] == '\\') rest.erase(0, 1);
      return rest.empty() ? host : host / HostRest(rest);
    }
  }
  for (auto& p : kPrefixes) {
    size_t n = strlen(p.guest);
    if (low.compare(0, n, p.guest) != 0) continue;
    std::string rest = guest.substr(n);
    while (!rest.empty() && rest[0] == '\\') rest.erase(0, 1);
    std::filesystem::path root = p.game ? g_game_root : g_user_root / "hdd";
    // The engine's optional settings file lives with the user's files, not the game's.
    if (p.game && Lower(rest) == "xenongamesettings.cfg") return g_user_root / HostRest(rest);
    // Game files patched for the PC (tools/menu_patch.py) take the place of
    // the originals from <user>/override/.
    if (p.game && !rest.empty()) {
      std::error_code ec;
      std::filesystem::path over = g_user_root / "override" / HostRest(rest);
      if (std::filesystem::is_regular_file(over, ec)) return over;
    }
    return rest.empty() ? root : root / HostRest(rest);
  }
  return {};
}

uint64_t FileTime(const FILETIME& ft) { return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; }

uint32_t Win32ToStatus(DWORD e) {
  switch (e) {
    case ERROR_FILE_NOT_FOUND: return STATUS_OBJECT_NAME_NOT_FOUND;
    case ERROR_PATH_NOT_FOUND: return STATUS_OBJECT_PATH_NOT_FOUND;
    case ERROR_ACCESS_DENIED: return STATUS_ACCESS_DENIED;
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return STATUS_OBJECT_NAME_COLLISION;
    default: return STATUS_UNSUCCESSFUL;
  }
}

void SetIosb(uint32_t iosb, uint32_t status, uint32_t info) {
  if (!iosb) return;
  St32(iosb, status);
  St32(iosb + 4, info);
}

std::string GuestPathFromAttributes(uint32_t attrs) {
  std::string name = ReadAnsiString(Ld32(attrs + 4));
  uint32_t root = Ld32(attrs);
  if (root) {
    if (auto f = std::dynamic_pointer_cast<FileObject>(HandleToHost(root)))
      name = f->guest_path + (f->guest_path.empty() || f->guest_path.back() == '\\' ? "" : "\\") + name;
  }
  return name;
}

uint32_t OpenFile(uint32_t handle_out, uint32_t access, uint32_t attrs, uint32_t iosb,
                  uint32_t disposition, uint32_t options) {
  std::string guest = GuestPathFromAttributes(attrs);
  std::filesystem::path host = MapPath(guest);
  if (host.empty()) {
    RT_WARN("open '%s': unknown device", guest.c_str());
    SetIosb(iosb, STATUS_OBJECT_PATH_NOT_FOUND, 0);
    return STATUS_OBJECT_PATH_NOT_FOUND;
  }
  bool want_write = access & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA);
  bool is_game = host.native().rfind(g_game_root.native(), 0) == 0;
  std::error_code ec;
  bool exists = std::filesystem::exists(host, ec);
  bool is_dir = exists && std::filesystem::is_directory(host, ec);
  if ((options & 1) || is_dir) {  // FILE_DIRECTORY_FILE
    if (!exists && (disposition == 2 || disposition == 3) && !is_game)
      std::filesystem::create_directories(host, ec), exists = true;
    if (!exists) {
      RT_INFO("open dir '%s' -> not found", guest.c_str());
      SetIosb(iosb, STATUS_OBJECT_NAME_NOT_FOUND, 0);
      return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    auto f = std::make_shared<FileObject>();
    f->directory = true;
    f->path = host;
    f->guest_path = guest;
    St32(handle_out, CreateHandle(0, f));
    RT_INFO("open dir '%s' -> ok", guest.c_str());
    SetIosb(iosb, STATUS_SUCCESS, 1);
    return STATUS_SUCCESS;
  }
  if (is_game && want_write) {
    RT_WARN("open '%s' for write on read-only game data: read-only instead", guest.c_str());
    want_write = false;
  }
  static const DWORD kDisposition[] = {CREATE_ALWAYS, OPEN_EXISTING, CREATE_NEW, OPEN_ALWAYS,
                                       TRUNCATE_EXISTING, CREATE_ALWAYS};
  DWORD disp = disposition < 6 ? kDisposition[disposition] : OPEN_EXISTING;
  if (is_game) disp = OPEN_EXISTING;
  if (want_write) std::filesystem::create_directories(host.parent_path(), ec);
  HANDLE h = CreateFileW(host.c_str(), GENERIC_READ | (want_write ? GENERIC_WRITE : 0),
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, disp,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    uint32_t status = Win32ToStatus(GetLastError());
    RT_INFO("open '%s' -> %08X", guest.c_str(), status);
    SetIosb(iosb, status, 0);
    return status;
  }
  auto f = std::make_shared<FileObject>();
  f->h = h;
  f->path = host;
  f->guest_path = guest;
  f->overlay = Lower(host.filename().string()) == "build_overlay-00.fbrb";
  St32(handle_out, CreateHandle(0, f));
  RT_INFO("open '%s' -> ok%s", guest.c_str(), want_write ? " (write)" : "");
  // The front end's menus load mainmenu-00, a level loads reduced-level-00
  // (bf1943) or levels\<name>\level-00 (bfbc1).
  std::string low = Lower(guest);
  if (low.find("\\menu\\main\\mainmenu-00.fbrb") != std::string::npos) g_in_menus = true, g_mainmenu_open = true;
  if (low.find("reduced-level-00.fbrb") != std::string::npos || low.find("\\level-00.fbrb") != std::string::npos) {
    g_in_menus = false;
    ApplyShadowSettings();
  }
  static bool logged = true;
  if (bool now = g_in_menus; now != logged) {
    logged = now;
    xgpu::SetHdrStretch(!now);
    RT_INFO("input: %s", now ? "front-end menus (mouse: wheel/click)" : "in a level (mouse: look/fire)");
  }
  SetIosb(iosb, STATUS_SUCCESS, exists ? 1 /*FILE_OPENED*/ : 2 /*FILE_CREATED*/);
  return STATUS_SUCCESS;
}

std::shared_ptr<FileObject> File(uint32_t handle) {
  return std::dynamic_pointer_cast<FileObject>(HandleToHost(handle));
}

void SignalCompletion(uint32_t event_handle) {
  if (!event_handle) return;
  if (uint32_t o = HandleToObject(event_handle)) SetEvent(o);
}

bool Attributes(const std::filesystem::path& p, WIN32_FILE_ATTRIBUTE_DATA* d) {
  return GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, d) != 0;
}

// FILE_NETWORK_OPEN_INFORMATION (0x38 bytes)
void WriteNetworkOpenInfo(uint32_t out, const WIN32_FILE_ATTRIBUTE_DATA& d) {
  uint64_t size = (uint64_t(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
  St64(out + 0x00, FileTime(d.ftCreationTime));
  St64(out + 0x08, FileTime(d.ftLastAccessTime));
  St64(out + 0x10, FileTime(d.ftLastWriteTime));
  St64(out + 0x18, FileTime(d.ftLastWriteTime));
  St64(out + 0x20, (size + 0xFFF) & ~0xFFFull);
  St64(out + 0x28, size);
  St32(out + 0x30, (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0x10 : 0x80);
  St32(out + 0x34, 0);
}

}  // namespace

// NtCreateFile(handle*, access, attrs*, iosb*, alloc_size*, file_attrs, share, disposition, options)
GUEST_IMPORT(NtCreateFile) {
  RETURN(OpenFile(ARG32(3), ARG32(4), ARG32(5), ARG32(6), ARG32(10),
                  uint32_t(Ld32(ctx.r1.u32 + 0x54))));  // 9th argument is on the stack
}

// NtOpenFile(handle*, access, attrs*, iosb*, share, options)
GUEST_IMPORT(NtOpenFile) { RETURN(OpenFile(ARG32(3), ARG32(4), ARG32(5), ARG32(6), 1, ARG32(8))); }

// NtReadFile(handle, event, apc, apc_ctx, iosb*, buffer, length, offset*)
GUEST_IMPORT(NtReadFile) {
  auto f = File(ARG32(3));
  uint32_t iosb = ARG32(7), buf = ARG32(8), len = ARG32(9), off = ARG32(10);
  if (!f || f->directory) RETURN(STATUS_INVALID_HANDLE);
  OVERLAPPED ov{};
  OVERLAPPED* pov = nullptr;
  if (off) {
    uint64_t o = Ld64(off);
    if (o != 0xFFFFFFFFFFFFFFFEull) {  // FILE_USE_FILE_POINTER_POSITION
      ov.Offset = DWORD(o);
      ov.OffsetHigh = DWORD(o >> 32);
      pov = &ov;
    }
  }
  DWORD got = 0;
  // Reading into GPU-watched physical memory: let the GPU drop its copies first
  // (ReadFile does not fault on write-protected pages, it fails).
  if (buf >= 0xA0000000 && buf < 0xC0000000) xgpu::TriggerWatches(buf & 0x1FFFFFFF, len);
  BOOL ok = ReadFile(f->h, G(buf), len, &got, pov);
  if (ok && got && f->overlay) PatchPcText(G<uint8_t>(buf), got);
  if (pov && ok) {  // keep the file pointer in step like the kernel does
    LARGE_INTEGER pos;
    pos.QuadPart = ((int64_t(ov.OffsetHigh) << 32) | ov.Offset) + got;
    SetFilePointerEx(f->h, pos, nullptr, FILE_BEGIN);
  }
  uint32_t status = (!ok && GetLastError() != ERROR_HANDLE_EOF) ? STATUS_UNSUCCESSFUL
                    : (got == 0 && len != 0)                     ? STATUS_END_OF_FILE
                                                                 : STATUS_SUCCESS;
  SetIosb(iosb, status, got);
  RT_TRACE("NtReadFile(%s, off %llX, %X) -> %X bytes, %08X", f->guest_path.c_str(), off ? (unsigned long long)Ld64(off) : ~0ull, len, got, status);
  SignalCompletion(ARG32(4));
  RETURN(status);
}

// NtWriteFile(handle, event, apc, apc_ctx, iosb*, buffer, length, offset*)
GUEST_IMPORT(NtWriteFile) {
  auto f = File(ARG32(3));
  uint32_t iosb = ARG32(7), buf = ARG32(8), len = ARG32(9), off = ARG32(10);
  if (!f || f->directory) RETURN(STATUS_INVALID_HANDLE);
  OVERLAPPED ov{};
  OVERLAPPED* pov = nullptr;
  if (off && Ld64(off) != 0xFFFFFFFFFFFFFFFEull) {
    uint64_t o = Ld64(off);
    ov.Offset = DWORD(o);
    ov.OffsetHigh = DWORD(o >> 32);
    pov = &ov;
  }
  DWORD put = 0;
  BOOL ok = WriteFile(f->h, G(buf), len, &put, pov);
  uint32_t status = ok ? STATUS_SUCCESS : STATUS_ACCESS_DENIED;
  SetIosb(iosb, status, put);
  SignalCompletion(ARG32(4));
  RETURN(status);
}

GUEST_IMPORT(NtReadFileScatter) {
  RT_WARN("NtReadFileScatter: not implemented");
  RETURN(STATUS_NOT_IMPLEMENTED);
}

// NtQueryInformationFile(handle, iosb*, info*, length, class)
GUEST_IMPORT(NtQueryInformationFile) {
  auto f = File(ARG32(3));
  uint32_t iosb = ARG32(4), out = ARG32(5), cls = ARG32(7);
  if (!f) RETURN(STATUS_INVALID_HANDLE);
  RT_TRACE("NtQueryInformationFile(%s, class %u)", f->guest_path.c_str(), cls);
  WIN32_FILE_ATTRIBUTE_DATA d{};
  Attributes(f->path, &d);
  uint64_t size = (uint64_t(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
  uint32_t info = 0;
  switch (cls) {
    case 4:  // FileBasicInformation
      St64(out + 0x00, FileTime(d.ftCreationTime));
      St64(out + 0x08, FileTime(d.ftLastAccessTime));
      St64(out + 0x10, FileTime(d.ftLastWriteTime));
      St64(out + 0x18, FileTime(d.ftLastWriteTime));
      St32(out + 0x20, f->directory ? 0x10 : 0x80);
      St32(out + 0x24, 0);
      info = 0x28;
      break;
    case 5:  // FileStandardInformation
      St64(out + 0, (size + 0xFFF) & ~0xFFFull);
      St64(out + 8, size);
      St32(out + 16, 1);
      St8(out + 20, 0);
      St8(out + 21, f->directory ? 1 : 0);
      info = 0x18;
      break;
    case 14: {  // FilePositionInformation
      LARGE_INTEGER pos{}, zero{};
      if (f->h != INVALID_HANDLE_VALUE) SetFilePointerEx(f->h, zero, &pos, FILE_CURRENT);
      St64(out, uint64_t(pos.QuadPart));
      info = 8;
      break;
    }
    case 34:  // FileNetworkOpenInformation
      WriteNetworkOpenInfo(out, d);
      info = 0x38;
      break;
    default:
      RT_WARN("NtQueryInformationFile(%s): class %u not implemented", f->guest_path.c_str(), cls);
      SetIosb(iosb, STATUS_NOT_IMPLEMENTED, 0);
      RETURN(STATUS_NOT_IMPLEMENTED);
  }
  SetIosb(iosb, STATUS_SUCCESS, info);
  RETURN(STATUS_SUCCESS);
}

// NtSetInformationFile(handle, iosb*, info*, length, class)
GUEST_IMPORT(NtSetInformationFile) {
  auto f = File(ARG32(3));
  uint32_t iosb = ARG32(4), in = ARG32(5), cls = ARG32(7);
  if (!f) RETURN(STATUS_INVALID_HANDLE);
  RT_TRACE("NtSetInformationFile(%s, class %u, value %llX)", f->guest_path.c_str(), cls,
           (unsigned long long)Ld64(in));
  switch (cls) {
    case 14: {  // FilePositionInformation
      LARGE_INTEGER pos;
      pos.QuadPart = int64_t(Ld64(in));
      SetFilePointerEx(f->h, pos, nullptr, FILE_BEGIN);
      break;
    }
    case 20: {  // FileEndOfFileInformation
      LARGE_INTEGER pos;
      pos.QuadPart = int64_t(Ld64(in));
      SetFilePointerEx(f->h, pos, nullptr, FILE_BEGIN);
      SetEndOfFile(f->h);
      break;
    }
    case 4:   // FileBasicInformation: timestamps, ignored
    case 19:  // FileAllocationInformation
      break;
    default:
      RT_WARN("NtSetInformationFile(%s): class %u ignored", f->guest_path.c_str(), cls);
      break;
  }
  SetIosb(iosb, STATUS_SUCCESS, 0);
  RETURN(STATUS_SUCCESS);
}

// NtQueryFullAttributesFile(attrs*, FILE_NETWORK_OPEN_INFORMATION*)
GUEST_IMPORT(NtQueryFullAttributesFile) {
  std::string guest = GuestPathFromAttributes(ARG32(3));
  std::filesystem::path host = MapPath(guest);
  WIN32_FILE_ATTRIBUTE_DATA d{};
  if (host.empty() || !Attributes(host, &d)) {
    RT_INFO("attributes '%s' -> not found", guest.c_str());
    RETURN(STATUS_OBJECT_NAME_NOT_FOUND);
  }
  WriteNetworkOpenInfo(ARG32(4), d);
  RETURN(STATUS_SUCCESS);
}

// NtQueryVolumeInformationFile(handle, iosb*, info*, length, class)
GUEST_IMPORT(NtQueryVolumeInformationFile) {
  uint32_t iosb = ARG32(4), out = ARG32(5), len = ARG32(6), cls = ARG32(7);
  memset(G(out), 0, len);
  uint32_t info = 0;
  switch (cls) {
    case 1:  // FileFsVolumeInformation
      St32(out + 8, 0x1943);  // serial
      info = std::min<uint32_t>(len, 0x18);
      break;
    case 3:  // FileFsSizeInformation: plenty of free space
      St64(out + 0, 0x100000);
      St64(out + 8, 0x80000);
      St32(out + 16, 32);
      St32(out + 20, 512);
      info = 0x18;
      break;
    case 5:  // FileFsAttributeInformation
      St32(out + 0, 0x6);
      St32(out + 4, 255);
      St32(out + 8, 4);
      memcpy(G(out + 12), "FATX", 4);
      info = 16;
      break;
    default:
      RT_WARN("NtQueryVolumeInformationFile: class %u not implemented", cls);
      RETURN(STATUS_NOT_IMPLEMENTED);
  }
  SetIosb(iosb, STATUS_SUCCESS, info);
  RETURN(STATUS_SUCCESS);
}

// NtQueryDirectoryFile(handle, event, apc, apc_ctx, iosb*, info*, length, pattern*, restart)
// Returns one FILE_DIRECTORY_INFORMATION (ANSI name) per call.
GUEST_IMPORT(NtQueryDirectoryFile) {
  auto f = File(ARG32(3));
  uint32_t iosb = ARG32(7), out = ARG32(8), len = ARG32(9), pattern = ARG32(10);
  bool restart = Ld32(ctx.r1.u32 + 0x54) != 0;
  if (!f || !f->directory) RETURN(STATUS_INVALID_HANDLE);
  WIN32_FIND_DATAW fd;
  bool found;
  if (restart && f->find != INVALID_HANDLE_VALUE) {
    FindClose(f->find);
    f->find = INVALID_HANDLE_VALUE;
  }
  do {
    if (f->find == INVALID_HANDLE_VALUE) {
      std::string pat = pattern ? ReadAnsiString(pattern) : "*";
      if (pat.empty()) pat = "*";
      f->find = FindFirstFileW((f->path / pat).c_str(), &fd);
      found = f->find != INVALID_HANDLE_VALUE;
    } else {
      found = FindNextFileW(f->find, &fd) != 0;
    }
  } while (found && (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")));
  if (!found) {
    SetIosb(iosb, STATUS_NO_MORE_FILES, 0);
    RETURN(STATUS_NO_MORE_FILES);
  }
  std::string name = std::filesystem::path(fd.cFileName).string();
  uint32_t need = 0x40 + uint32_t(name.size());
  if (need > len) RETURN(STATUS_INVALID_PARAMETER);
  memset(G(out), 0, 0x40);
  uint64_t size = (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
  St64(out + 0x08, FileTime(fd.ftCreationTime));
  St64(out + 0x10, FileTime(fd.ftLastAccessTime));
  St64(out + 0x18, FileTime(fd.ftLastWriteTime));
  St64(out + 0x20, FileTime(fd.ftLastWriteTime));
  St64(out + 0x28, size);
  St64(out + 0x30, (size + 0xFFF) & ~0xFFFull);
  St32(out + 0x38, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0x10 : 0x80);
  St32(out + 0x3C, uint32_t(name.size()));
  memcpy(G(out + 0x40), name.data(), name.size());
  SetIosb(iosb, STATUS_SUCCESS, need);
  RETURN(STATUS_SUCCESS);
}

GUEST_IMPORT(NtFlushBuffersFile) { RETURN(STATUS_SUCCESS); }
GUEST_IMPORT(NtDeviceIoControlFile) {
  RT_WARN("NtDeviceIoControlFile(handle %X, code %08X): not implemented", ARG32(3), ARG32(8));
  RETURN(STATUS_NOT_IMPLEMENTED);
}
GUEST_IMPORT(IoDismountVolume) { RETURN(STATUS_SUCCESS); }
GUEST_IMPORT(IoDismountVolumeByFileHandle) { RETURN(STATUS_SUCCESS); }
GUEST_IMPORT(StfsCreateDevice) {
  RT_WARN("StfsCreateDevice: not implemented");
  RETURN(STATUS_SUCCESS);
}
GUEST_IMPORT(StfsControlDevice) { RETURN(STATUS_SUCCESS); }

namespace rt {
void MountContentRoot(const std::string& root, const std::filesystem::path& host) {
  std::lock_guard lock(g_content_mutex);
  std::string low = Lower(root);
  std::erase_if(g_content_roots, [&](auto& r) { return r.first == low; });
  g_content_roots.emplace_back(low, host);
}
bool UnmountContentRoot(const std::string& root) {
  std::lock_guard lock(g_content_mutex);
  std::string low = Lower(root);
  return std::erase_if(g_content_roots, [&](auto& r) { return r.first == low; }) != 0;
}
}  // namespace rt
