// bf1943: native host for the recompiled Battlefield 1943 code.
// Usage: bf1943 [--game <dir with default.xex>] [--user <writable dir>] [--trace]
#include "kernel.h"
#include "lan.h"
#include "pc_settings.h"
#include "texture_replacement.h"  // xenia_shims
#include "pbr_shaders.h"          // xenia_shims
#include "xenia_bridge.h"
namespace xe::gpu::scenefx { void Configure(const char* settings); }  // xenia gpu/vulkan/scene_fx.h

#include "../tools/msvc_compat/crash_diag.h"  // symbolised stack on crash

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <timeapi.h>
#else
#include <arpa/inet.h>
#include <csignal>
#include <fcntl.h>
#include <sys/prctl.h>
#include <vector>
#endif

#include <algorithm>
#include <cstring>
#include <thread>

using namespace rt;

// A fault in guest memory: what and where, with the guest registers.
static void ReportGuestFault(uint8_t* addr, bool write) {
    if (addr >= g_base && addr < g_base + PPC_MEMORY_SIZE + 0x10000) {
      RT_ERROR("guest memory fault: %s of guest address %08llX (thread %u)",
               write ? "write" : "read", (unsigned long long)(addr - g_base),
               CurrentThreadId());
      RT_ERROR("  page %scommitted; physical heap %X in use, virtual heap %X in use",
               IsCommitted(uint32_t(addr - g_base)) ? "" : "not ", g_physical_heap->used_bytes(),
               g_virtual_heap->used_bytes());
      if (PPCContext* c = CurrentContext()) {
        const PPCRegister* r[] = {&c->r0, &c->r1, &c->r2, &c->r3, &c->r4, &c->r5, &c->r6, &c->r7,
                                  &c->r8, &c->r9, &c->r10, &c->r11, &c->r12, &c->r13, &c->r14, &c->r15,
                                  &c->r16, &c->r17, &c->r18, &c->r19, &c->r20, &c->r21, &c->r22, &c->r23,
                                  &c->r24, &c->r25, &c->r26, &c->r27, &c->r28, &c->r29, &c->r30, &c->r31};
        for (int i = 0; i < 32; i += 4)
          RT_ERROR("  r%-2d %08X  r%-2d %08X  r%-2d %08X  r%-2d %08X", i, r[i]->u32, i + 1, r[i + 1]->u32,
                   i + 2, r[i + 2]->u32, i + 3, r[i + 3]->u32);
        for (const PPCRegister* reg : {&c->r3, &c->r30, &c->r31}) {
          uint32_t a = reg->u32 & ~3u;
          if (a < 0x10000 || !IsCommitted(a) || !IsCommitted(a + 63)) continue;
          RT_ERROR("  [%08X] %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X", a,
                   Ld32(a), Ld32(a + 4), Ld32(a + 8), Ld32(a + 12), Ld32(a + 16), Ld32(a + 20),
                   Ld32(a + 24), Ld32(a + 28), Ld32(a + 32), Ld32(a + 36), Ld32(a + 40), Ld32(a + 44));
        }
      }
    }
}
#ifdef _WIN32
static LONG WINAPI GuestFaultReporter(EXCEPTION_POINTERS* ep) {
  auto* rec = ep->ExceptionRecord;
  if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
    ReportGuestFault(reinterpret_cast<uint8_t*>(rec->ExceptionInformation[1]), rec->ExceptionInformation[0] != 0);
  return EXCEPTION_CONTINUE_SEARCH;
}
#else
// Linux: back to a guarded call (rt::g_fault_guard) if one is running; else
// report, then let the default action end the process (core dump).
thread_local sigjmp_buf* rt::g_fault_guard = nullptr;
static void GuestFaultSignal(int sig, siginfo_t* info, void*) {
  if (rt::g_fault_guard) siglongjmp(*rt::g_fault_guard, 1);
  ReportGuestFault(static_cast<uint8_t*>(info->si_addr), false);
  bf_crash_diag::PrintStack(nullptr);
  signal(sig, SIG_DFL);
  raise(sig);
}
#endif

// "12:start,15.5:a,20:down,30:back*3,40:ly+*2" -> scripted virtual pad presses
// (held 150 ms, or the seconds after '*'). Sticks: lx+ lx- ly+ ly- rx+ rx- ry+
// ry- (full deflection; ly+ = forward), triggers: lt rt.
static void ParsePresses(const std::string& spec) {
  static const std::pair<const char*, uint16_t> kNames[] = {
      {"up", 0x0001}, {"down", 0x0002}, {"left", 0x0004}, {"right", 0x0008},
      {"start", 0x0010}, {"back", 0x0020}, {"lb", 0x0100}, {"rb", 0x0200},
      {"a", 0x1000}, {"b", 0x2000}, {"x", 0x4000}, {"y", 0x8000}};
  size_t pos = 0;
  while (pos < spec.size()) {
    size_t end = spec.find(',', pos);
    std::string item = spec.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    size_t colon = item.find(':');
    if (colon != std::string::npos) {
      double t = atof(item.substr(0, colon).c_str());
      std::string name = item.substr(colon + 1);
      double hold = 0.15;
      if (size_t star = name.find('*'); star != std::string::npos) {
        hold = atof(name.c_str() + star + 1);
        name.resize(star);
      }
      for (auto& [n, bits] : kNames)
        if (name == n) g_scripted_presses.push_back({t, bits, hold});
      static const char* kAxes[] = {"lx", "ly", "rx", "ry"};
      for (int a = 0; a < 4; ++a)
        if (name.size() == 3 && name.compare(0, 2, kAxes[a]) == 0 && (name[2] == '+' || name[2] == '-'))
          g_scripted_presses.push_back({t, 0, hold, a, name[2] == '+' ? 1 : -1});
      if (name == "lt" || name == "rt") g_scripted_presses.push_back({t, 0, hold, name == "lt" ? 4 : 5, 1});
    }
    if (end == std::string::npos) break;
    pos = end + 1;
  }
}

// --bots N: once the --host game's server exists, start N more copies of this
// program that join it as players: no window and no GPU emulation (--toy-gpu
// --headless), each with its own ports (--port-offset) and user folder. A
// client always connects to 127.0.0.1:1003, the host's server, since only
// bound ports are offset. The job object closes them with the host.
int rt::g_bot_count = 0;
bool rt::g_no_audio = false;  // --no-audio (the Linux server decodes XMA too; it has no sound device)
static std::vector<std::string> g_argv;
void rt::LaunchBots() {
  static bool launched = false;
  if (launched || !g_bot_count) return;
  launched = true;
  // The bots are players on this server (bot_players.cpp); BF_BOT_COPIES=1:
  // copies of the game joining it, as before.
  if (!getenv("BF_BOT_COPIES")) return;
#ifndef _WIN32
  // Linux: fork + exec of this program; each bot ends with the server
  // (PR_SET_PDEATHSIG), its output in <user>_botN/bot.log.
  std::string ai = StartBotDirector(g_bot_count);
  for (int i = 1; i <= g_bot_count; ++i) {
    std::filesystem::path user = g_user_root.string() + "_bot" + std::to_string(i);
    std::filesystem::create_directories(user);
    std::vector<std::string> args = {g_argv[0], "--game", g_game_root.string(), "--user", user.string(), "--host",
                                     "--toy-gpu", "--headless", "--port-offset", std::to_string(10 * i),
                                     "--host-map", g_host_map, "--gamertag", "Bot" + std::to_string(i)};
    if (!ai.empty()) {
      args.insert(args.end(), {"--bot-ai", ai, "--bot-slot", std::to_string(i - 1)});
    }
    std::vector<char*> argv_c;
    for (std::string& a : args) argv_c.push_back(a.data());
    argv_c.push_back(nullptr);
    std::string log = (user / "bot.log").string();
    pid_t pid = fork();
    if (pid == 0) {
      prctl(PR_SET_PDEATHSIG, SIGTERM);
      int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (fd >= 0) dup2(fd, 1), dup2(fd, 2);
      execv("/proc/self/exe", argv_c.data());
      _exit(127);
    }
    if (pid > 0) RT_INFO("bots: started bot %d (log %s)", i, log.c_str());
    else RT_WARN("bots: could not start bot %d", i);
  }
}
#else
  std::string ai = StartBotDirector(g_bot_count);
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
  for (int i = 1; i <= g_bot_count; ++i) {
    std::filesystem::path user = g_user_root.string() + "_bot" + std::to_string(i);
    std::filesystem::create_directories(user);
    std::string cmd = "\"" + g_argv[0] + "\" --game \"" + g_game_root.string() + "\" --user \"" + user.string() +
                      "\" --host --toy-gpu --headless --port-offset " + std::to_string(10 * i) + " --host-map " +
                      g_host_map + " --gamertag Bot" + std::to_string(i) +
                      (ai.empty() ? "" : " --bot-ai " + ai + " --bot-slot " + std::to_string(i - 1));
    std::filesystem::path log = user / "bot.log";
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE out = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr);
    STARTUPINFOA si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = out;
    PROCESS_INFORMATION pi{};
    if (CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                       nullptr, &si, &pi)) {
      AssignProcessToJobObject(job, pi.hProcess);
      ResumeThread(pi.hThread);
      ::CloseHandle(pi.hThread);
      ::CloseHandle(pi.hProcess);
      RT_INFO("bots: started bot %d (log %s)", i, log.string().c_str());
    } else {
      RT_WARN("bots: could not start bot %d (%lu)", i, GetLastError());
    }
    ::CloseHandle(out);
  }
}
#endif

int main(int argc, char** argv) {
  g_argv.assign(argv, argv + argc);
  std::filesystem::path exe_dir = std::filesystem::path(argv[0]).parent_path();
  g_game_root = "original/v106";
  g_user_root = "userdata";
  int aniso = 5;            // 16x anisotropic filtering (--aniso 0: the game's own)
  // Sun shadowmap size (game: 1024). Each slice is drawn in one pass, so it
  // must fit the 10 MB EDRAM as 32-bit depth: 1600x1600 (20 tiles of 80 x 1600
  // rows) is the largest square that does. At 2048 the game draws and resolves
  // only a 1040-wide strip of each 2048x2048 slice; the rest stays 0, and
  // shadows falling there vanish (which half a receiver lands in moves with
  // the shadow distance).
  int shadowmap = 1600;
  std::string bot_ai;  // --bot-ai <shared memory> --bot-slot N: this is a bot, driven by the host's AI
  int bot_slot = -1;
  uint32_t texture_mb = 0;
  std::string shader_dump;  // --dump-shaders <dir>
  int res_scale = 2, present_effect = 2, window_w = 0, window_h = 0;  // --res-scale, --sharpen, --window
  bool hdr = false;                                                   // --hdr, --hdr-white, --hdr-peak
  float hdr_white = 200.0f, hdr_peak = 1000.0f;
  // --preset stock|enhanced|toon|noir|ww2: the graphics options it stands for go
  // first, so the other options can still change them. stock: the game's own
  // rendering; enhanced: PBR materials, scene effects (sun shafts, haze,
  // screen-space and ray-traced shadows), the 1600 shadowmap at 300 m and 4x
  // vegetation detail; toon, noir and ww2: enhanced with that look.
  std::vector<std::string> preset_args;
  std::vector<char*> expanded_argv;
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--preset")) continue;
    std::string preset = argv[i + 1];
    if (preset == "stock") {
      preset_args = {"--shadowmap", "0", "--shadow-distance", "0", "--lod", "1"};
    } else {
      preset_args = {"--pbr", "--scene-fx"};
      if (preset == "toon" || preset == "noir" || preset == "ww2") {
        preset_args.push_back("--look");
        preset_args.push_back(preset);
      }
    }
    expanded_argv.push_back(argv[0]);
    for (auto& p : preset_args) expanded_argv.push_back(p.data());
    for (int k = 1; k < argc; ++k) {
      if (k == i || k == i + 1) continue;
      expanded_argv.push_back(argv[k]);
    }
    argc = int(expanded_argv.size());
    argv = expanded_argv.data();
    break;
  }
  std::string join_address;  // --join
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--game" && i + 1 < argc) g_game_root = argv[++i];
    else if (a == "--user" && i + 1 < argc) g_user_root = argv[++i];
    else if (a == "--trace") g_trace_imports = true;
    else if (a == "--toy-gpu") g_use_xenia_gpu = false;   // count PM4 only, no Vulkan
    else if (a == "--headless") g_present = false;
    else if (a == "--dedicated") {
      g_dedicated = true;
      g_present = false;
      g_use_xenia_gpu = false;
      g_lan_host = true;
    }
    else if (a == "--join" && i + 1 < argc) join_address = argv[++i];
    else if (a == "--name" && i + 1 < argc) g_gamertag = std::string(argv[++i]).substr(0, 15);
    else if (a == "--maps" && i + 1 < argc) {
      std::string list = argv[++i];
      for (size_t p = 0; p <= list.size();) {
        size_t c = list.find(',', p);
        if (c == std::string::npos) c = list.size();
        if (c > p) g_map_rotation.push_back(list.substr(p, c - p));
        p = c + 1;
      }
    }
    else if (a == "--press" && i + 1 < argc) ParsePresses(argv[++i]);
    else if (a == "--capture" && i + 1 < argc) g_capture_seconds = uint32_t(atoi(argv[++i]));
    else if (a == "--capture-burst" && i + 1 < argc) g_capture_burst = uint32_t(atoi(argv[++i]));
    else if (a == "--look" && i + 1 < argc) {
      std::string l = argv[++i];
      xgpu::SetLook(l == "toon" ? 1 : l == "noir" ? 2 : l == "ww2" ? 3 : 0);
    }
    else if (a == "--blood") g_blood = true;
    else if (a == "--vblank-hz" && i + 1 < argc) g_vblank_hz = std::max(1, atoi(argv[++i]));
    else if (a == "--max-fps" && i + 1 < argc) g_max_fps = uint32_t(std::max(0, atoi(argv[++i])));
    else if (a == "--vsync") g_vsync = true;
    else if (a == "--gamertag" && i + 1 < argc) g_gamertag = std::string(argv[++i]).substr(0, 15);
    else if (a == "--dump-threads" && i + 1 < argc) {
      int sec = std::max(1, atoi(argv[++i]));
      std::thread([sec] {
        for (;;) {
          std::this_thread::sleep_for(std::chrono::seconds(sec));
          DumpGuestThreads();
        }
      }).detach();
    }
    else if (a == "--args" && i + 1 < argc) g_guest_args = argv[++i];
    else if (a == "--online-server" && i + 1 < argc) g_online_server = argv[++i];
    else if (a == "--lan-ip" && i + 1 < argc) inet_pton(AF_INET, argv[++i], &g_lan_ip);
    else if (a == "--host") g_lan_host = true;
    else if (a == "--bots" && i + 1 < argc) g_bot_count = std::clamp(atoi(argv[++i]), 0, 31);
    else if (a == "--host-map" && i + 1 < argc) g_host_map = argv[++i];
    else if (a == "--log-ui") g_log_ui = true;
    else if (a == "--log-settings" && i + 1 < argc) g_log_settings = argv[++i];
    else if (a == "--ui-call" && i + 1 < argc) g_ui_call = argv[++i];
    else if (a == "--mouse-sens" && i + 1 < argc) g_mouse_sensitivity = float(atof(argv[++i]));
    else if (a == "--invert-mouse") g_mouse_invert = true;
    else if (a == "--test-mouse" && i + 1 < argc) g_test_mouse_at = atof(argv[++i]);
    else if (a == "--worldrender" && i + 1 < argc) g_worldrender = argv[++i];
    else if (a == "--overgrowth" && i + 1 < argc) g_overgrowth = argv[++i];
    else if (a == "--lod" && i + 1 < argc) g_lod = float(atof(argv[++i]));
    else if (a == "--bot-ai" && i + 1 < argc) bot_ai = argv[++i];
    else if (a == "--bot-slot" && i + 1 < argc) bot_slot = atoi(argv[++i]);
    else if (a == "--watch-write" && i + 1 < argc) {  // DEBUG: addr,size,seconds
      uint32_t addr = 0, size = 4;
      double at = 0;
      sscanf(argv[++i], "%x,%u,%lf", &addr, &size, &at);
      StartWriteWatch(addr, size, at);
    } else if (a == "--snap-mem" && i + 1 < argc) {  // DEBUG: "t1,t2,..." seconds
      std::string times = argv[++i];
      std::thread([times] {
        auto t0 = std::chrono::steady_clock::now();
        size_t p = 0;
        for (int n = 0; p < times.size(); ++n) {
          double t = atof(times.c_str() + p);
          p = times.find(',', p);
          p = p == std::string::npos ? times.size() : p + 1;
          std::this_thread::sleep_until(t0 + std::chrono::milliseconds(int64_t(t * 1000)));
          char name[64];
          snprintf(name, sizeof(name), "snap_%d.bin", n);
          SnapshotMemory(name);
          RT_INFO("snap-mem: %s at %.1f s", name, t);
        }
      }).detach();
    }
    else if (a == "--port-offset" && i + 1 < argc) g_port_offset = uint16_t(g_base_port_offset + atoi(argv[++i]));
    else if (a == "--audio-wav" && i + 1 < argc) g_audio_wav = argv[++i];
    else if (a == "--no-audio") g_no_audio = true;
    else if (a == "--textures" && i + 1 < argc) {
      size_t n = xe::gpu::texrep::SetDirectory(std::filesystem::absolute(argv[++i]));
      RT_INFO("textures: %zu replacements", n);
      if (n) texture_mb = 2048;  // 2x RGBA8 textures need far more than Xenia's 384 MB
    }
    else if (a == "--pbr") {  // [table], default: pbr_shaders.txt next to the exe
      std::filesystem::path table;
      if (i + 1 < argc && std::strncmp(argv[i + 1], "--", 2)) table = argv[++i];
      else {
#ifdef _WIN32
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        table = std::filesystem::path(exe).parent_path() / "pbr_shaders.txt";
#else
        table = std::filesystem::canonical("/proc/self/exe").parent_path() / "pbr_shaders.txt";
#endif
      }
      RT_INFO("pbr: %zu shaders", xe::gpu::pbr::Load(std::filesystem::absolute(table)));
    }
    else if (a == "--scene-fx") {  // [key=value,...]: volumetric sun light and haze
      const char* opts = i + 1 < argc && std::strncmp(argv[i + 1], "--", 2) ? argv[++i] : "";
      xe::gpu::scenefx::Configure(opts);
      RT_INFO("scene effects: on %s", opts);
    }
    else if (a == "--aniso" && i + 1 < argc) {  // 1, 2, 4, 8 or 16
      int x = atoi(argv[++i]), level = 1;  // Xenos AnisoFilter: 1 = 1:1 ... 5 = 16:1
      while (level < 5 && (1 << level) <= x) ++level;
      aniso = x <= 0 ? -1 : level;
    }
    else if (a == "--shadowmap" && i + 1 < argc) shadowmap = std::clamp(atoi(argv[++i]), 0, 1600);
    else if (a == "--shadow-distance" && i + 1 < argc) g_shadow_distance = float(atof(argv[++i]));
    else if (a == "--dump-shaders" && i + 1 < argc) shader_dump = argv[++i];
    else if (a == "--res-scale" && i + 1 < argc) res_scale = atoi(argv[++i]);
    else if (a == "--sharpen" && i + 1 < argc) {
      std::string e = argv[++i];
      present_effect = e == "fsr" ? 2 : e == "cas" ? 1 : 0;
    }
    else if (a == "--window" && i + 1 < argc) sscanf(argv[++i], "%dx%d", &window_w, &window_h);
    else if (a == "--trace-texture" && i + 1 < argc) xe::gpu::texrep::SetTrace(argv[++i]);
    else if (a == "--log-textures") xe::gpu::texrep::SetLogMisses(true);
    else if (a == "--hdr") hdr = true;
    else if (a == "--hdr-white" && i + 1 < argc) hdr_white = float(atof(argv[++i]));
    else if (a == "--hdr-peak" && i + 1 < argc) hdr_peak = float(atof(argv[++i]));
  }
  xgpu::SetHdr(hdr, hdr_white, hdr_peak);
  xgpu::SetOptions(aniso, texture_mb, shader_dump.c_str());
  if (shadowmap) shadowmap = std::max(80, shadowmap / 80 * 80);  // whole EDRAM tiles
  xgpu::SetDisplayOptions(res_scale, present_effect, window_w, window_h, shadowmap ? shadowmap : 1024);
  g_shadow_size = uint32_t(shadowmap);
  if (!bot_ai.empty() && bot_slot >= 0) SetBotSlot(bot_ai, bot_slot);
  g_auto_start = g_dedicated || bot_slot >= 0;
  if (!join_address.empty() && !rt::lan::PrepareJoin(join_address, g_host_map.empty() ? "Wake_island_s" : g_host_map))
    RT_WARN("--join: not an IPv4 address: %s", join_address.c_str());
  if (g_dedicated) {
    if (g_host_map.empty()) g_host_map = g_map_rotation.empty() ? "Wake_island_s" : g_map_rotation[0];
    if (g_gamertag.empty() || g_gamertag == "Player") g_gamertag = "Dedicated";
    rt::lan::StartBeacon();
    RT_INFO("dedicated server '%s': hosting %s on UDP %d (browsers: UDP 19430)%s", g_gamertag.c_str(),
            g_host_map.c_str(), 1003 + g_port_offset, g_bot_count ? ", with bots" : "");
  }
  if (g_lan_host && join_address.empty()) rt::lan::StartBeacon();  // --host: shown in server browsers
  if (shadowmap && g_guest_args.find("Shadowmap") == std::string::npos)
    g_guest_args = "-WorldRender.ShadowmapWidth " + std::to_string(shadowmap) + " -WorldRender.ShadowmapHeight " +
                   std::to_string(shadowmap) + (g_guest_args.empty() ? "" : " ") + g_guest_args;
  g_game_root = std::filesystem::absolute(g_game_root);
  g_user_root = std::filesystem::absolute(g_user_root);
  std::filesystem::create_directories(g_user_root);
  rt::pcset::Load();  // pc_settings.cpp: the PC settings page's saved choices
#ifdef _WIN32
  AddVectoredExceptionHandler(1, GuestFaultReporter);
#else
  struct sigaction sa{};
  sa.sa_sigaction = GuestFaultSignal;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, nullptr);
  sigaction(SIGBUS, &sa, nullptr);
  signal(SIGPIPE, SIG_IGN);  // a closed socket must not end the server
#endif

  if (!MemoryInit()) Fatal("could not reserve the 4 GB guest address space");
  RT_INFO("guest memory at %p, game root %s", g_base, g_game_root.string().c_str());
  RT_INFO("host timebase %.1f MHz", HostTicksPerSecond() / 1e6);
  if (!LoadExecutable(g_game_root / "default.xex")) Fatal("could not load default.xex");
  timeBeginPeriod(1);  // 1 ms sleeps and timers, as on the console
  InstallPcUi();
  StartKernelServices();
  RunMainThread();
}
