// See nav_autogen.h.
#include "nav_autogen.h"

#include "kernel.h"  // (before windows.h, as the rest of the runtime)

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace rt::navauto {

std::filesystem::path ExeDir() {
#ifdef _WIN32
  wchar_t buf[MAX_PATH];
  DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return std::filesystem::path(std::wstring(buf, n)).parent_path();
#else
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf));
  return n > 0 ? std::filesystem::path(std::string(buf, size_t(n))).parent_path() : std::filesystem::path(".");
#endif
}

FILE* Open(const std::filesystem::path& p, const char* mode) {
#ifdef _WIN32
  std::wstring wmode(mode, mode + std::strlen(mode));
  return _wfopen(p.c_str(), wmode.c_str());
#else
  return fopen(p.c_str(), mode);
#endif
}

std::string Utf8(const std::filesystem::path& p) {
  const auto s = p.u8string();
  return std::string(s.begin(), s.end());
}

std::filesystem::path Find(const std::string& file) {
  for (const std::filesystem::path& dir : {ExeDir() / "nav", std::filesystem::path("nav")}) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(dir / file, ec)) return dir / file;
  }
  return {};
}

namespace {

std::filesystem::path ExePath() {
#ifdef _WIN32
  wchar_t buf[MAX_PATH];
  DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return std::filesystem::path(std::wstring(buf, n));
#else
  std::error_code ec;
  return std::filesystem::canonical("/proc/self/exe", ec);
#endif
}

// tools/make_navs.py: BF_MAKE_NAVS, or up from the exe (the repo's
// bf1943-recomp/build/game, bfbc1-recomp/build/game, a release's <root>/game
// and <root>/game-bfbc1 next to <root>/src, build_linux.sh's <out>/game).
std::filesystem::path FindScript() {
  std::error_code ec;
  if (const char* e = getenv("BF_MAKE_NAVS"); e && *e && std::filesystem::is_regular_file(e, ec)) return e;
  std::filesystem::path d = ExeDir();
  for (int up = 0; up < 5 && !d.empty(); ++up) {
    for (const char* rel : {"tools/make_navs.py", "src/tools/make_navs.py", "bf1943-recomp/tools/make_navs.py"}) {
      std::filesystem::path p = d / std::filesystem::path(rel).make_preferred();
      if (std::filesystem::is_regular_file(p, ec)) return p;
    }
    if (d == d.parent_path()) break;
    d = d.parent_path();
  }
  return {};
}

// Python 3: BF_PYTHON, else (Windows) python.exe on PATH, the per-user and
// all-users installs (the launcher's winget Python) or the py launcher;
// (Linux) python3 from PATH. The arguments before the script.
std::vector<std::filesystem::path::string_type> FindPython() {
  using S = std::filesystem::path::string_type;
  if (const char* e = getenv("BF_PYTHON"); e && *e) return {std::filesystem::path(e).native()};
#ifdef _WIN32
  std::error_code ec;
  std::vector<std::filesystem::path> cands;
  if (const wchar_t* path = _wgetenv(L"PATH")) {
    std::wstring all = path;
    for (size_t p = 0; p <= all.size();) {
      size_t q = all.find(L';', p);
      if (q == std::wstring::npos) q = all.size();
      std::wstring dir = all.substr(p, q - p);
      std::wstring lower = dir;
      std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
      // (not the Microsoft Store's placeholder)
      if (!dir.empty() && lower.find(L"windowsapps") == std::wstring::npos) cands.push_back(std::filesystem::path(dir) / L"python.exe");
      p = q + 1;
    }
  }
  auto installs = [&](const wchar_t* base) {
    std::vector<std::filesystem::path> found;
    for (auto it = std::filesystem::directory_iterator(base, ec); !ec && it != std::filesystem::directory_iterator();
         it.increment(ec)) {
      std::wstring n = it->path().filename().native();
      if (n.rfind(L"Python3", 0) == 0) found.push_back(it->path() / L"python.exe");
    }
    std::sort(found.rbegin(), found.rend());  // (the newest first)
    cands.insert(cands.end(), found.begin(), found.end());
  };
  if (const wchar_t* local = _wgetenv(L"LOCALAPPDATA")) installs((std::wstring(local) + L"\\Programs\\Python").c_str());
  if (const wchar_t* pf = _wgetenv(L"ProgramFiles")) installs(pf);
  installs(L"C:\\");
  for (const auto& c : cands)
    if (std::filesystem::is_regular_file(c, ec)) return {c.native()};
  wchar_t win[MAX_PATH];
  UINT n = ::GetWindowsDirectoryW(win, MAX_PATH);
  std::filesystem::path py = std::filesystem::path(std::wstring(win, n)) / L"py.exe";
  if (std::filesystem::is_regular_file(py, ec)) return {py.native(), S(L"-3")};
  return {};
#else
  return {S("python3")};
#endif
}

#ifdef _WIN32
// One argument for CommandLineToArgvW / the C runtime's parser.
std::wstring Quote(const std::wstring& a) {
  if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
  std::wstring q = L"\"";
  size_t slashes = 0;
  for (wchar_t c : a) {
    if (c == L'\\') {
      ++slashes;
      continue;
    }
    q.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
    slashes = 0;
    q += c;
  }
  q.append(slashes * 2, L'\\');
  return q + L"\"";
}
#endif

std::mutex g_mutex;
std::set<std::string> g_requested;

}  // namespace

void Request(const char* title, const std::string& map) {
  if (map.empty()) return;
  {
    std::lock_guard lock(g_mutex);
    if (!g_requested.insert(std::string(title) + "/" + map).second) return;
  }
  for (const char* off : {"BF_MAKE_NAVS_CHILD", "BF_BOT_DUMP_COLLISION", "BF_NO_NAV_AUTOGEN"})
    if (const char* v = getenv(off); v && *v) return;
  if (!g_dedicated && !g_lan_host) return;
  std::filesystem::path script = FindScript();
  auto python = FindPython();
  if (script.empty() || python.empty()) {
    RT_WARN("bot nav: no navigation files for %s, and %s to make them: the bots play without routes "
            "(the launcher makes them, or tools/make_navs.py)", map.c_str(),
            script.empty() ? "tools/make_navs.py isn't found" : "Python isn't found");
    return;
  }
  // Into <exe dir>/nav (where both games look first), else nav/ here.
  std::error_code ec;
  std::filesystem::path nav_dir = ExeDir() / "nav";
  std::filesystem::create_directories(nav_dir, ec);
  if (ec || !std::filesystem::is_directory(nav_dir, ec)) nav_dir = std::filesystem::absolute("nav", ec);
  std::filesystem::path log = g_user_root / ("make_navs_" + map + ".log");
  using P = std::filesystem::path;
  std::vector<P::string_type> args(python.begin(), python.end());
  for (const P& a : {P("-I"), script, P("--title"), P(title), P("--game"), g_game_root, P("--exe"), ExePath(),
                     P("--nav-dir"), nav_dir, P("--maps"), P(map)})
    args.push_back(a.native());
  RT_INFO("bot nav: no navigation files for %s: making them in the background (tools/make_navs.py, a minute or "
          "a few; log %s); the bots play without routes until then", map.c_str(), Utf8(log).c_str());
#ifdef _WIN32
  std::wstring cmd;
  for (const auto& a : args) cmd += (cmd.empty() ? L"" : L" ") + Quote(a);
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE out = ::CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr);
  STARTUPINFOW si{sizeof(si)};
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = si.hStdError = out;
  si.hStdInput = nullptr;
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> line(cmd.begin(), cmd.end());
  line.push_back(0);
  // Behind the game; ended with it (a job closed with this process).
  bool ok = ::CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE,
                           CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | CREATE_SUSPENDED, nullptr, nullptr, &si,
                           &pi);
  if (out != INVALID_HANDLE_VALUE) ::CloseHandle(out);
  if (!ok) {
    RT_WARN("bot nav: could not start Python for tools/make_navs.py (%lu)", ::GetLastError());
    return;
  }
  static HANDLE job = [] {
    HANDLE j = ::CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim{};
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (j) ::SetInformationJobObject(j, JobObjectExtendedLimitInformation, &lim, sizeof(lim));
    return j;
  }();
  if (job) ::AssignProcessToJobObject(job, pi.hProcess);
  ::ResumeThread(pi.hThread);
  ::CloseHandle(pi.hThread);
  HANDLE proc = pi.hProcess;
  std::thread([proc, map] {
    ::WaitForSingleObject(proc, INFINITE);
    DWORD code = 1;
    ::GetExitCodeProcess(proc, &code);
    ::CloseHandle(proc);
    if (code == 0) RT_INFO("bot nav: the navigation files for %s are made", map.c_str());
    else RT_WARN("bot nav: making the navigation files for %s failed (exit %lu, see its log)", map.c_str(), code);
  }).detach();
#else
  std::string log_s = log.string();
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);
  pid_t pid = fork();
  if (pid < 0) {
    RT_WARN("bot nav: could not start tools/make_navs.py (fork)");
    return;
  }
  if (pid == 0) {
    prctl(PR_SET_PDEATHSIG, SIGKILL);  // (ends with this process)
    setpriority(PRIO_PROCESS, 0, 10);  // (behind the game)
    int fd = open(log_s.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) dup2(fd, 1), dup2(fd, 2), close(fd);
    int in = open("/dev/null", O_RDONLY);
    if (in >= 0) dup2(in, 0), close(in);
    execvp(argv[0], argv.data());
    _exit(127);
  }
  std::thread([pid, map] {
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    if (code == 0) RT_INFO("bot nav: the navigation files for %s are made", map.c_str());
    else RT_WARN("bot nav: making the navigation files for %s failed (exit %d, see its log)", map.c_str(), code);
  }).detach();
#endif
}

}  // namespace rt::navauto
