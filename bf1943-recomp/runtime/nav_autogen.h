// The bots' navigation files (nav/<map>.nav, Bad Company also .vnav and .hf)
// are made on the player's PC from their own game (tools/make_navs.py): where
// the game looks for them, and a hosting server that has none for its map
// starting the tool by itself, once, in the background.
#pragma once

#include <cstdio>
#include <filesystem>
#include <string>

namespace rt::navauto {

// The folder the game exe is in.
std::filesystem::path ExeDir();
// A navigation file (e.g. "Wake_island_s.nav"): <exe dir>/nav first, then nav/
// in the working folder. Empty if it is in neither.
std::filesystem::path Find(const std::string& file);
// The hosted map has no navigation files (title "bf1943" or "bfbc1"): starts
// tools/make_navs.py for it in the background (once per map and process; its
// output in <user>/make_navs_<map>.log), into <exe dir>/nav. Not in a run of
// that tool itself (BF_MAKE_NAVS_CHILD), with BF_BOT_DUMP_COLLISION or
// BF_NO_NAV_AUTOGEN set, or when this game isn't hosting. The bots play
// without routes until the files are there; their loaders look again.
void Request(const char* title, const std::string& map);
// Opens a path (wide on Windows, for folder names outside the ANSI code page).
FILE* Open(const std::filesystem::path& p, const char* mode);
// A path in UTF-8, for the log.
std::string Utf8(const std::filesystem::path& p);

}  // namespace rt::navauto
