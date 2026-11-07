// Kernel-side declarations shared between runtime files: loaded module info,
// guest threads, dispatcher objects and the handle table.
#pragma once
#include "guest.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <vector>

namespace rt {

// ---- loaded executable -------------------------------------------------------
struct ModuleInfo {
  uint32_t entry = 0;
  uint32_t stack_size = 0x40000;
  uint32_t tls_slot_count = 64;
  uint32_t tls_raw_address = 0;   // template for static (__declspec(thread)) data
  uint32_t tls_data_size = 0;
  uint32_t tls_raw_size = 0;
  uint32_t xex_header = 0;        // guest copy of the XEX header
  uint32_t ldr_entry = 0;         // fake LDR_DATA_TABLE_ENTRY (module handle)
};
extern ModuleInfo g_module;
extern std::filesystem::path g_game_root;   // game:\ and d:\ map here
extern std::filesystem::path g_user_root;   // cache:\, writable devices
// XamContentCreateEx: "<root>:\..." maps onto host until unmounted.
void MountContentRoot(const std::string& root, const std::filesystem::path& host);
bool UnmountContentRoot(const std::string& root);

bool LoadExecutable(const std::filesystem::path& xex);
// Called by the loader for every data import; returns the guest address the
// import slot should point at.
uint32_t ResolveVariableImport(const char* module, const char* name, uint32_t ordinal);

// ---- dispatcher objects (live in guest memory) ---------------------------------
// X_DISPATCHER_HEADER: u8 type, u8 absolute, u8 size, u8 inserted,
// be32 signal_state, be32 wait_list[2]. Waits read and update signal_state.
enum : uint8_t {
  kNotificationEvent = 0, kSynchronizationEvent = 1, kMutant = 2, kProcess = 3,
  kQueue = 4, kSemaphore = 5, kThread = 6, kNotificationTimer = 8,
  kSynchronizationTimer = 9,
};
void SignalObject(uint32_t object);   // wake waiters after changing signal_state
uint32_t SetEvent(uint32_t event);    // returns previous state
uint32_t ResetEvent(uint32_t event);
uint32_t PulseEvent(uint32_t event);
// timeout_ptr: guest LARGE_INTEGER* (100 ns units, negative = relative), 0 = infinite.
uint32_t WaitMultiple(const uint32_t* objects, uint32_t count, bool wait_all, uint32_t timeout_ptr,
                      bool alertable);
uint32_t ReleaseSemaphore(uint32_t sem, int32_t increment, int32_t adjustment);
uint32_t ReleaseMutant(uint32_t mutant, bool abandon, bool wait);
void InitDispatcherHeader(uint32_t object, uint8_t type, int32_t signal_state);

// ---- handle table ---------------------------------------------------------------
struct HostObject {
  virtual ~HostObject() = default;
};
// A handle refers to a guest object (dispatcher header or other block) and
// optionally carries a host-side object (files, threads).
uint32_t CreateHandle(uint32_t guest_object, std::shared_ptr<HostObject> host = nullptr);
bool CloseHandle(uint32_t handle);
uint32_t DuplicateHandle(uint32_t handle);
uint32_t HandleToObject(uint32_t handle);  // 0 if invalid
std::shared_ptr<HostObject> HandleToHost(uint32_t handle);
constexpr uint32_t kCurrentProcess = 0xFFFFFFFF;
constexpr uint32_t kCurrentThread = 0xFFFFFFFE;

// ---- guest threads ----------------------------------------------------------------
struct GuestThread;
GuestThread* CurrentThread();
PPCContext* CurrentContext();  // null on non-guest threads
uint32_t CurrentKThread();      // guest address of the current X_KTHREAD
uint32_t CurrentThreadId();
// Creates a guest thread. start/param as in ExCreateThread (xapi_startup may be 0).
// Returns the thread handle; *thread_id gets the id.
uint32_t CreateGuestThread(uint32_t stack_size, uint32_t xapi_startup, uint32_t start,
                           uint32_t param, bool suspended, uint32_t* thread_id,
                           const char* name = nullptr);
// Runs the module entry on the calling host thread (never returns normally).
[[noreturn]] void RunMainThread();
// Gives the calling host thread a guest identity (PCR, stack, KTHREAD) so it can
// call guest code, e.g. the vsync interrupt thread.
PPCContext& AdoptHostThread(const char* name, uint32_t stack_size);

void StartKernelServices();  // timestamp bundle, timers

// GPU: Xenia's Vulkan command processor (default) or the counting-only one.
extern bool g_use_xenia_gpu;
extern bool g_present;  // open a window and show frames
extern uint32_t g_capture_seconds;
extern uint32_t g_capture_burst;  // seconds: then 8 frames back to back (burst_N.png)
extern bool g_blood;              // --blood: blood on soldiers hit, gibs on big kills
void DumpGuestThreads();  // --dump-threads <seconds>
extern uint32_t g_max_fps;   // --max-fps: replaces Game.MaxFps (0 = no limit)
extern bool g_vsync;         // --vsync: keep the game's vblank-paced presents
extern uint32_t g_vblank_hz;  // --vblank-hz: guest vsync rate (the game presents every 2nd vblank)
extern std::string g_gamertag;       // --gamertag: profile name (default "Player")
extern float g_mouse_sensitivity;  // --mouse-sens (1 = default)
extern bool g_mouse_invert;        // --invert-mouse
extern float g_plane_mouse_sensitivity;  // PC settings: mouse in planes (1 = default)
extern bool g_plane_mouse_invert;        // PC settings: mouse forward pulls up in planes
// The local player's vehicle (bots.cpp; none in games without the probe).
enum LocalVehicleKind { kOnFoot = 0, kDriving = 1, kFlying = 2, kGunner = 3 };
int LocalVehicle();
bool SimulatedKeyDown(int vk);  // xam.cpp: a key held by BF_TEST_KEY (tests)
// The PC main and pause menus replace the game's (game_patches.cpp; false in
// games without them), and the game's dialogs up now (-1 before it said).
bool CustomMenus();
// --dedicated: a console server (no window, GPU or menus) that hosts
// g_host_map, then each map of --maps in turn when a round ends.
extern bool g_dedicated;
extern std::vector<std::string> g_map_rotation;
int GameDialogs();
extern double g_test_mouse_at;     // --test-mouse <s>
// Mouse look for this poll, in right-stick units (1 = full deflection, may exceed 1).
float MouseLookAxis(int axis);
void InstallPcUi();                // pc_ui.cpp: PC key icons in the UI atlases
void PatchPcText(uint8_t* data, size_t size);  // pc_ui.cpp: PC wording in the string tables
extern int g_bot_count;              // --bots N: extra clients that join the --host game
void LaunchBots();                    // main.cpp
std::string StartBotDirector(int count);  // bots.cpp: host side AI; returns the shared memory name
void SetBotSlot(const std::string& shared_name, int slot);  // bots.cpp: bot process side
extern bool g_lan_host;            // --host: local game over real UDP (LAN server)
extern std::string g_host_map;     // --host-map: level hosted from the main menu
extern uint32_t g_join_address;    // lan.cpp: the joined host (network order, 0 none)
void RequestHost();                // game_patches.cpp: host g_host_map from the main loop
extern bool g_log_ui;              // --log-ui: log menu callbacks (name and argument)
extern std::string g_log_settings; // --log-settings A.B,C.D: log these engine settings
extern std::string g_ui_call;      // --ui-call name:args: menu callback fired after the title (testing)
extern uint32_t g_shadow_size;           // --shadowmap (0 = the game's 1024)
extern float g_shadow_distance;          // --shadow-distance (default 300; 0 = the game's 100 m)
extern float g_lod;                      // --lod N: N times the vegetation detail (default 4, 1 = the game's)
extern std::string g_overgrowth;         // --overgrowth "Field=value,..." (all OvergrowthSettings copies)
extern std::string g_worldrender;         // --worldrender "Field=value,..." (all WorldRender copies)
void ApplyShadowSettings();               // game_patches.cpp: to every WorldRender copy
extern std::atomic<bool> g_in_menus;     // files.cpp: front-end menus (not in a level)
extern std::atomic<bool> g_level_menu;   // game_patches.cpp: a menu over a level (Esc, spawn screen)
extern std::atomic<bool> g_online_ready;  // net.cpp: the Theater login has answered
extern std::string g_guest_args;     // --args: the game's own command line (ExLoadedCommandLine)
extern std::string g_online_server;  // --online-server: EA host names (*.ea.com) resolve here
extern uint32_t g_lan_ip;            // --lan-ip: address in our XNADDR (network order)
void RegisterHostedGame(uint16_t port, const std::string& map);  // net.cpp: list on Arcadia
extern uint16_t g_port_offset;       // --port-offset: added to bound ports
extern std::string g_audio_wav;  // --audio-wav: also write the mixed output (front L/R) here

// Virtual pad button presses at fixed times since start (--press), for tests.
struct ScriptedPress {
  double at_seconds;
  uint16_t buttons;  // XINPUT_GAMEPAD_* bits
  double hold_seconds = 0.15;
  int axis = -1;     // or a stick/trigger: 0 lx, 1 ly, 2 rx, 3 ry, 4 lt, 5 rt
  int direction = 0; // +1 / -1 (full deflection)
};
extern std::vector<ScriptedPress> g_scripted_presses;  // save frames/frame_NNN.png this often (0 = off)
extern std::atomic<bool> g_title_passed;      // xam.cpp: Start taken at the title screen (profile read)
extern std::atomic<int64_t> g_pad_t0_ns;  // xam.cpp: time of the first pad read (ns, steady clock)
extern std::atomic<bool> g_mainmenu_open;    // files.cpp: the main menu file has been opened
extern std::atomic<uint16_t> g_auto_buttons;  // xam.cpp: buttons held by automation, added to pad 0

}  // namespace rt
