// Linux: the Win32 file calls files.cpp uses, on POSIX file descriptors, so the
// (Windows-tested) file code stays as it is. Paths are matched without regard
// to case, as on Windows and the console: the game asks for "Levels\Wake_..."
// where the dump may say "levels/wake_...".
#pragma once
#ifndef _WIN32

#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>

#define INVALID_HANDLE_VALUE (reinterpret_cast<HANDLE>(intptr_t(-1)))
using LONG = int32_t;

struct WIN32_FILE_ATTRIBUTE_DATA {
  DWORD dwFileAttributes;
  FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
  DWORD nFileSizeHigh, nFileSizeLow;
};
struct WIN32_FIND_DATAW {
  DWORD dwFileAttributes;
  FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
  DWORD nFileSizeHigh, nFileSizeLow;
  wchar_t cFileName[260];
};
struct OVERLAPPED {
  DWORD Offset, OffsetHigh;
};
union LARGE_INTEGER {
  int64_t QuadPart;
};

enum : DWORD {
  CREATE_NEW = 1, CREATE_ALWAYS = 2, OPEN_EXISTING = 3, OPEN_ALWAYS = 4, TRUNCATE_EXISTING = 5,
  GENERIC_READ = 0x80000000u, GENERIC_WRITE = 0x40000000u,
  FILE_SHARE_READ = 1, FILE_SHARE_WRITE = 2, FILE_SHARE_DELETE = 4,
  FILE_ATTRIBUTE_DIRECTORY = 0x10, FILE_ATTRIBUTE_NORMAL = 0x80,
  FILE_BEGIN = 0, FILE_CURRENT = 1, FILE_END = 2,
  ERROR_FILE_NOT_FOUND = 2, ERROR_PATH_NOT_FOUND = 3, ERROR_ACCESS_DENIED = 5,
  ERROR_HANDLE_EOF = 38, ERROR_FILE_EXISTS = 80, ERROR_ALREADY_EXISTS = 183,
};
enum GET_FILEEX_INFO_LEVELS { GetFileExInfoStandard };

namespace posixfs {

inline thread_local DWORD last_error = 0;
inline void SetError(int e) {
  last_error = e == ENOENT ? ERROR_FILE_NOT_FOUND
               : e == ENOTDIR ? ERROR_PATH_NOT_FOUND
               : e == EACCES || e == EPERM || e == EROFS ? ERROR_ACCESS_DENIED
               : e == EEXIST ? ERROR_FILE_EXISTS
                             : DWORD(e);
}

// The path as it is on disk: each missing component looked up without case.
inline std::filesystem::path Resolve(const std::filesystem::path& p) {
  std::error_code ec;
  if (std::filesystem::exists(p, ec)) return p;
  std::filesystem::path out = p.root_path();
  bool lost = false;
  for (const auto& part : p.relative_path()) {
    std::filesystem::path next = out / part;
    if (!lost && !std::filesystem::exists(next, ec)) {
      bool found = false;
      for (const auto& e : std::filesystem::directory_iterator(out.empty() ? "." : out, ec)) {
        if (strcasecmp(e.path().filename().c_str(), part.c_str()) == 0) {
          next = out / e.path().filename();
          found = true;
          break;
        }
      }
      if (!found) lost = true;  // keep the rest as given (a file being created)
    }
    out = next;
  }
  return out;
}

inline FILETIME ToFileTime(const struct timespec& t) {
  // 100 ns ticks since 1601.
  uint64_t v = (uint64_t(t.tv_sec) + 11644473600ull) * 10000000ull + uint64_t(t.tv_nsec) / 100;
  return FILETIME{DWORD(v), DWORD(v >> 32)};
}
inline void FromStat(const struct stat& st, DWORD& attrs, FILETIME& c, FILETIME& a, FILETIME& w, DWORD& hi,
                     DWORD& lo) {
  attrs = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
  c = ToFileTime(st.st_ctim);
  a = ToFileTime(st.st_atim);
  w = ToFileTime(st.st_mtim);
  hi = DWORD(uint64_t(st.st_size) >> 32);
  lo = DWORD(st.st_size);
}

struct Find {
  std::vector<std::string> names;
  std::filesystem::path dir;
  size_t next = 0;
};
inline bool FillFind(Find* f, WIN32_FIND_DATAW* fd) {
  while (f->next < f->names.size()) {
    const std::string& n = f->names[f->next++];
    struct stat st{};
    if (stat((f->dir / n).c_str(), &st) != 0) continue;
    FromStat(st, fd->dwFileAttributes, fd->ftCreationTime, fd->ftLastAccessTime, fd->ftLastWriteTime,
             fd->nFileSizeHigh, fd->nFileSizeLow);
    size_t i = 0;
    for (; i < n.size() && i < 259; ++i) fd->cFileName[i] = wchar_t(static_cast<unsigned char>(n[i]));
    fd->cFileName[i] = 0;
    return true;
  }
  return false;
}

}  // namespace posixfs

inline DWORD GetLastErrorFile() { return posixfs::last_error; }

inline HANDLE CreateFileW(const char* path, DWORD access, DWORD, void*, DWORD disposition, DWORD, void*) {
  std::filesystem::path p = posixfs::Resolve(path);
  int flags = (access & GENERIC_WRITE) ? O_RDWR : O_RDONLY;
  switch (disposition) {
    case CREATE_NEW: flags |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS: flags |= O_CREAT | O_TRUNC; break;
    case OPEN_ALWAYS: flags |= O_CREAT; break;
    case TRUNCATE_EXISTING: flags |= O_TRUNC; break;
    default: break;
  }
  int fd = open(p.c_str(), flags | O_CLOEXEC, 0644);
  if (fd < 0) {
    posixfs::SetError(errno);
    return INVALID_HANDLE_VALUE;
  }
  return reinterpret_cast<HANDLE>(intptr_t(fd));
}
inline int FdOf(HANDLE h) { return int(reinterpret_cast<intptr_t>(h)); }

inline BOOL ReadFile(HANDLE h, void* buf, DWORD len, DWORD* got, OVERLAPPED* ov) {
  ssize_t n = ov ? pread(FdOf(h), buf, len, off_t((uint64_t(ov->OffsetHigh) << 32) | ov->Offset))
                 : read(FdOf(h), buf, len);
  if (n < 0) {
    posixfs::SetError(errno);
    *got = 0;
    return 0;
  }
  *got = DWORD(n);
  if (n == 0 && len) posixfs::last_error = ERROR_HANDLE_EOF;
  return 1;
}
inline BOOL WriteFile(HANDLE h, const void* buf, DWORD len, DWORD* put, OVERLAPPED* ov) {
  ssize_t n = ov ? pwrite(FdOf(h), buf, len, off_t((uint64_t(ov->OffsetHigh) << 32) | ov->Offset))
                 : write(FdOf(h), buf, len);
  if (n < 0) {
    posixfs::SetError(errno);
    *put = 0;
    return 0;
  }
  *put = DWORD(n);
  return 1;
}
inline BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER to, LARGE_INTEGER* now, DWORD method) {
  off_t r = lseek(FdOf(h), off_t(to.QuadPart), method == FILE_BEGIN ? SEEK_SET : method == FILE_CURRENT ? SEEK_CUR : SEEK_END);
  if (r < 0) return 0;
  if (now) now->QuadPart = int64_t(r);
  return 1;
}
inline BOOL CloseHandle(HANDLE h) { return close(FdOf(h)) == 0; }

inline BOOL GetFileAttributesExW(const char* path, GET_FILEEX_INFO_LEVELS, WIN32_FILE_ATTRIBUTE_DATA* d) {
  struct stat st{};
  if (stat(posixfs::Resolve(path).c_str(), &st) != 0) {
    posixfs::SetError(errno);
    return 0;
  }
  posixfs::FromStat(st, d->dwFileAttributes, d->ftCreationTime, d->ftLastAccessTime, d->ftLastWriteTime,
                    d->nFileSizeHigh, d->nFileSizeLow);
  return 1;
}

// FindFirstFileW("dir/pattern"): the directory's entries matching the pattern
// (* and ?, without case), with . and .. as Windows lists them.
inline HANDLE FindFirstFileW(const char* path_pattern, WIN32_FIND_DATAW* fd) {
  std::filesystem::path pp(path_pattern);
  auto* f = new posixfs::Find;
  f->dir = posixfs::Resolve(pp.parent_path());
  std::string pat = pp.filename().string();
  f->names = {".", ".."};
  if (DIR* d = opendir(f->dir.c_str())) {
    while (dirent* e = readdir(d)) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
      if (fnmatch(pat.c_str(), e->d_name, FNM_CASEFOLD) == 0) f->names.push_back(e->d_name);
    }
    closedir(d);
  }
  if (!posixfs::FillFind(f, fd)) {
    delete f;
    posixfs::last_error = ERROR_FILE_NOT_FOUND;
    return INVALID_HANDLE_VALUE;
  }
  return f;
}
inline BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW* fd) { return posixfs::FillFind(static_cast<posixfs::Find*>(h), fd); }
inline BOOL FindClose(HANDLE h) {
  delete static_cast<posixfs::Find*>(h);
  return 1;
}

#endif  // !_WIN32
