# Battlefield Recompilation Project: developer guide

Native Windows x64 ports of two Xbox 360 Battlefield games, built by statically
recompiling their PowerPC code with [XenonRecomp](https://github.com/hedge-dev/XenonRecomp)
and rendering through Vulkan with a modified [Xenia](https://github.com/xenia-project/xenia) GPU backend.

| Project | Game | State |
|---|---|---|
| [`bf1943-recomp/`](../bf1943-recomp/) | Battlefield 1943 (XBLA, v1.06) | Playable: all maps, LAN host/join, bots, full PC feature set |
| [`bfbc1-recomp/`](../bfbc1-recomp/) | Battlefield: Bad Company | In progress, shares the bf1943 runtime and renderer |

**No game files or recompiled game code are included.** Put your own dump in
`bf1943-recomp/original/v106/` (and `bfbc1-recomp/original/`), then generate and build
as described in [`bf1943-recomp/README.md`](../bf1943-recomp/README.md) (build steps, every command-line option and how each feature works).

## Before / after

Left: the game's own rendering (`--preset stock`, 1280×720). Right: this project
(`--preset enhanced`, HD/PBR texture pack, 2560×1440 with FSR; shown downscaled).

| Stock | Enhanced |
|---|---|
| ![Wake Island stock](images/wake_before.jpg) | ![Wake Island enhanced](images/wake_after.jpg) |
| ![Iwo Jima stock](images/iwo_before.jpg) | ![Iwo Jima enhanced](images/iwo_after.jpg) |

## Menus

The game's own intro and title screen are kept; from the main menu on, the PC menus
take over: a WW2 field-orders look (aged paper, olive drab, stencil and typewriter
type), animated, for mouse, keyboard and controller alike.

| | |
|---|---|
| ![Main menu](images/menu_main.jpg) | ![Settings](images/menu_settings.jpg) |
| ![Host game](images/menu_host.jpg) | ![Pause menu over the game](images/menu_pause.jpg) |

## Features (Battlefield 1943)

### Port
- Static recompilation of the whole game to x64 (clang-cl), no emulated CPU.
- Vulkan 1.3 renderer (Xenia's Xenos → SPIR-V translator, vendored and extended).
- Audio through Xenia's XMA decoder (FFmpeg) and XAudio2.
- Electron launcher: finds the game and texture pack, resolution, upscaling, shadows,
  frame rate, window size, HD textures, PBR, look presets, HDR.

### Graphics
- **Resolution scale** 1–3× of 720p, **AMD FSR / CAS** upscaling and sharpening.
- **Uncapped frame rate** (`--max-fps`, default 120) with the client simulation ticking at the same rate.
- **HD texture pack** (`tools/texpack`) with **PBR materials**: GGX metal/roughness lighting,
  roughness-blurred cube-map reflections, 16× anisotropic filtering.
- **Parallax occlusion mapping** from height maps in the pack's normal maps.
- **Ray-traced sun shadows** (Vulkan ray queries): every world mesh, multi-part vehicles,
  alpha-tested foliage and the full terrain heightmap; casters off screen still cast.
- **Screen-space sun shadows** on GPUs without ray tracing.
- **Ray-traced bounce lighting (GI)** with a world-space colour cache and contact darkening.
- **GTAO ambient occlusion**.
- **Volumetric light shafts and height fog**, fog matched to the horizon sky.
- **Volumetric clouds** with cloud shadows on the ground.
- **Water**: wave refraction, foam along hulls and beaches, clear turquoise shallows,
  sea and surface reflections.
- **Muzzle-flash light** replaced by a short-range shadowed light (the game's lit the whole scene).
- **Procedural surface detail**: grime, pebbles and cracks on bare ground.
- **Larger shadow map** (1600², 300 m) and **4× vegetation draw distance**.
- **HDR10 output** with adjustable paper white and peak.
- **Looks**: stock, enhanced, toon, noir and WW2 newsreel film.
- **Blood and gore** (optional): hit sprays, pools, bodies breaking apart on explosive kills.

### PC controls and menus
- Full **mouse and keyboard**: raw mouse look, rebindable keys, controller still works.
- **Vehicle keys**: W/S throttle and brake, A/D steering or rudder, arrow keys pitch and
  roll planes, separate **plane mouse sensitivity** and invert.
- **PRESS ANY KEY** title; any key or mouse button starts.
- **New main menu** (HOST GAME, JOIN GAME, SETTINGS, QUIT) replacing the game's: its
  buttons call the game's own code directly (`hostMultiplayerLocal`), the game's front end
  runs hidden underneath.
- **New pause menu** (Esc / Start in a level: RESUME, SETTINGS, SUICIDE, QUIT TO MENU) over
  the live game; suicide and quit work the game's own menu unseen behind a STAND BY screen.
- **Settings** (also F10 anywhere): every graphics option and effect, mouse, plane mouse,
  key bindings, multiplayer name; most apply live.
- Menus answer the mouse (hover, click, right click = back), keyboard and controller
  (D-pad / stick, A, B, LB/RB for pages); `BF_STOCK_MENUS=1` brings back the game's own.

### Multiplayer
- **HOST GAME**: pick the map (Wake Island, Iwo Jima, Guadalcanal, Coral Sea air
  superiority) and bot count; no sign-in needed.
- **JOIN GAME**: LAN server browser, or join by address.
- **Bots**: AI players for hosted games (control-point strategy, combat, landing craft from the carriers).
- Optional EA-server emulator (`server/arcadia360`, Arcadia, GPL-2.0).

## Layout
- `bf1943-recomp/runtime/`: the native host (kernel/XAM, memory, threads, input, network, PC menus, bots), shared with bfbc1.
  The menus: `pc_settings.cpp` (screens and input), `pc_menu.cpp` (the look, GDI+ on its own thread).
- `bf1943-recomp/thirdparty/xenia/`: the GPU backend with all render features (`gpu/vulkan/scene_fx*`, `gpu/shaders/`).
- `bf1943-recomp/tools/`: XenonRecomp helpers, texture pack builder, menu patcher, PBR table generator.
- `bf1943-recomp/launcher/`: the launcher.
- `bfbc1-recomp/`: Bad Company build files and game-specific code.

## Licences
Project code: as noted per directory. Third-party: XenonRecomp (MIT), Xenia (BSD-3),
VulkanMemoryAllocator (MIT), FFmpeg subset (LGPL-2.1), Arcadia (GPL-2.0).
Fonts in `bf1943-recomp/data/fonts`: Stardos Stencil (SIL OFL 1.1), Special Elite (Apache 2.0).
Battlefield is a trademark of Electronic Arts; this project is not affiliated with EA or DICE.

## Linux dedicated server

`bf1943-recomp/game/linux/CMakeLists.txt` builds `bf1943_server`: the recompiled game and the
runtime with clang on Linux, without GPU, sound output, window or PC menus
(`runtime/linux_server.cpp` has their stand-ins). The runtime's Windows calls have POSIX
versions: memory (`mmap`/`mprotect`, the physical views through `memfd`), threads,
files (`posix_files.h`, case-insensitive paths), sockets (`posix_net.h`, Winsock error
codes and option numbers mapped), time and SHA-1 (`posix_compat.h`, `sha1.h`).

```
cmake -S bf1943-recomp/game/linux -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release       -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build/linux
build/linux/bf1943_server --game <your dump> --dedicated --name "My Server" --maps Wake_island_s
```

It also builds on Windows, cross-compiled with LLVM for Windows and a Debian sysroot:

```
python bf1943-recomp/tools/fetch_linux_sysroot.py bf1943-recomp/build/linux-sysroot
cmake -S bf1943-recomp/game/linux -B bf1943-recomp/build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release       -DCMAKE_TOOLCHAIN_FILE=bf1943-recomp/game/linux/cross-from-windows.cmake
cmake --build bf1943-recomp/build/linux
```

The cross-compiled server runs on Ubuntu 24.04 (tested in WSL2): it passes the title,
hosts the map, and a Windows client joined it (`--join <server IP>`). Copy the game files
onto the Linux disk: loading them from a Windows drive in WSL is slow. Bots run as on Windows (players on
the server itself). Audio: Xenia's XMA decoder (FFmpeg xmaframes) is built for Linux too, with no sound
device (the game's audio code needs the decoding, without it the front end hangs);
`--no-audio` runs Windows the same way, and a dedicated server run like that hosted a
game a client joined and played. Still missing: a full Linux client (Vulkan presenter,
input, sound output).

## Working on the bots

The bot decisions are in `bf1943-recomp/runtime/bots/brain.cpp`, behind a small C interface
(`bots/bot_api.h`); `runtime/bots.cpp` reads the match from the server and passes each
bot's pad on. Testing switches (environment variables, on the hosting game):

- `BF_BOT_BRAIN=<path to build/game/bot_brain.dll>`: use the brain from that DLL, reloaded whenever it
  changes, so a running match picks up a change. Rebuild it (about 3 s) with
  `cmake --build build/game --target bot_brain`. Each bot's state is kept across reloads,
  unless the state struct changed size.
- `BF_BOT_SCENARIO="walk <flag>" | "board <kind>" | "drive <kind> <flag>"`: every bot does
  only that and the result is logged as `scenario: PASS ...` or `FAIL ...` (time limit
  `BF_BOT_SCENARIO_TIME`, default 180 s; `BF_BOT_RESULT=<file>` writes it to a file;
  `BF_BOT_SCENARIO_EXIT=1` ends the run once judged, exit code 0 or 3).
- `BF_BOT_LOG=<file.csv>`: every bot's position, vehicle and state twice a second, with
  the flags and vehicles; `python bf1943-recomp/tools/bot_paths.py <file.csv>` draws it.

An unattended test, headless (from `bf1943-recomp/`, in cmd):

```
set BF_BOT_SCENARIO=board LCVP
set BF_BOT_SCENARIO_EXIT=1
set BF_BOT_LOG=board.csv
build/game/bf1943.exe --dedicated --bots 1 --host-map Wake_island_s --user userdata_test
python tools/bot_paths.py board.csv
```

Vehicles (`BF_BOT_NO_VEHICLES` turns them off):
- Controls, as the game's default layout: LT throttle (it also lets a landing craft down
  from its carrier crane), LB brake and reverse, left stick X steers; in a plane the right
  stick Y pitches (`BF_PLANE_PITCH_SIGN=-1` flips it). `BF_BOT_INPUT_PROBE=1` makes a
  seated bot try every input for 4 s in turn, to read off the log what moves a vehicle.
- Seated shows in the soldier's byte at +34C (1 on foot, 0 seated). Jeeps, tanks, planes
  and guns keep no live position at *(+F0)+F0; where they were made, *(+EC)+310, is used.
- Landing craft: the bot stays in the first seat (the driver's) up to 45 s for the craft
  to come down (the game takes from 2 s to a minute), and rides it to the beach.
- Jeeps and tanks: taken near a captured flag when the next flag is a long walk away.
- Planes: one bot in four is a pilot: takes off from its carrier, holds about 100 m over
  the sea and circles the flag it's after (no bombing or dogfighting yet).
- On foot, a bot that's stuck backs off and walks round at an angle (one side for three
  tries, longer each time); at the water's edge along the shore, and to a way up another
  bot found.

Scenario `near`: the bot stands still, then the server and the bot's game move every
16-byte aligned vector equal to its position (its physics and the copies of it, a few
hundred on the server) to the new place. (Writing only the soldier's +180 or +3D0 is put
back every frame.)

### Navigation meshes (Bad Company's tools)

Bots walk along routes on a navigation mesh (`runtime/bots/nav.cpp`, Detour, the same
layer as Bad Company's bots): `nav/<map>.nav` next to the exe. Polygons a bot keeps
getting stuck on are turned off and kept in `<user>/nav/<map>.blocked`; the mesh's main
piece is the one the flags are on (on island maps the biggest is the sea floor).

A mesh is made on the player's PC from their own copy (private, never in the repo), with
Bad Company's collision tool, which reads every Havok rigid body of the running level
(`runtime/bot_collision.cpp`; BF1943's hkRigidBody vtable 820B07CC, hkWorld 820B0748,
found through their RTTI with `BF_FIND_VTABLE=hkRigidBody,hkWorld`):

```
set BF_BOT_DUMP_COLLISION=C:/path/live.obj
build/game/bf1943.exe --dedicated --host-map Wake_island_s --user userdata_dump
rem the log ends with "bot nav: play area x0 z0 x1 z1"
python tools/make_navmesh.py original/v106 wake_island_s Wake_island_s build/game/nav/Wake_island_s.nav ^
  --live C:/path/live.obj --bounds x0 z0 x1 z1
```

`make_navmesh.py` adds the terrain (`terrain.py`) and runs `tools/navgen` (build it from
`tools/navgen/CMakeLists.txt`); without `--live` it uses only the level files' collision
groups (no carriers or placed objects). `navgen --islands <nav> [x y z]...` shows the
mesh's pieces.

Bots as players inside the server (Bad Company's way, instead of a game copy per bot) are
being ported: `runtime/bot_players.cpp` has Bad Company's functions found in BF1943
(`tools/match_functions.py` matches functions between the games by their shape).
