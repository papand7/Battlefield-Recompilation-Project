// The PC menus' look (pc_menu.cpp): a WW2 field-orders style, aged paper,
// olive drab, stencil and typewriter type, drawn with GDI+ on its own thread
// into a 1920x1080 image shown over the game (xgpu::SetOverlay). The menus'
// logic (pc_settings.cpp) describes each screen as a View; the renderer
// animates between them (the selection slides, new pages slide in).
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace rt::pcmenu {

constexpr int kW = 1920, kH = 1080;
constexpr int kVisibleRows = 11;

enum class Screen { kButtons, kList, kBusy };

struct Row {
  std::wstring label, value, tag;
  bool choice = false;  // arrows either side of the value
};

struct View {
  Screen screen = Screen::kButtons;
  bool opaque = true;  // the game hidden; else seen, dimmed, right of the menu
  std::wstring title, subtitle;
  // kButtons: the big entries on the olive band.
  std::vector<std::wstring> buttons;
  int button = 0;
  // kList: tabs along the top, rows below.
  std::vector<std::wstring> tabs;
  int tab = 0;
  bool tab_focus = false;
  std::vector<Row> rows;  // the visible ones
  int row = -1;           // selected visible row (-1 none)
  bool more_above = false, more_below = false;
  std::wstring desc;  // the line under the rows (or the side note)
  bool pad = false;   // controller prompts instead of keys
  std::vector<std::pair<std::wstring, std::wstring>> prompts;  // key, label
  int page = 0;  // a different number: the page slides in
};

// Hit areas, in canvas pixels.
struct Rect {
  int l = 0, t = 0, r = 0, b = 0;
  bool Has(int x, int y) const { return x >= l && x < r && y >= t && y < b; }
};
Rect ButtonRect(int i);
Rect TabRect(int i);
Rect RowRect(int visible_index);
Rect LeftArrowRect(int visible_index);
Rect RightArrowRect(int visible_index);
Rect PromptRect(int i);

// Show (or update) the menu; Hide takes it away.
void Show(const View& view);
void Hide();

}  // namespace rt::pcmenu
