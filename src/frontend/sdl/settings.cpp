// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
#include "frontend/sdl/settings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ds::sdl {
namespace {

using T = Setting::Type;

const Choice kOnOff[]   = {{"false", "OFF"}, {"true", "ON"}};
const Choice kSkipMode[] = {{"adaptive", "ADAPTIVE"}, {"fixed", "FIXED"}};
// The rates a panel comes in, plus the console's own and no limiter at all.
// "auto" is the default and the only one that plays a game at the speed it was
// written for; 60 is 0.29 % fast, which is what a player means by "60 fps"
// and what lines up with a 60 Hz panel. Both ends are named in the row's own
// language rather than left in English: the rest of the row is in the language
// that is set, and a value in another one is the odd thing out on it.
const Choice kLimiter[] = {{"auto", "CONSOLE (59.8)"}, {"30", "30"}, {"60", "60"}, {"120", "120"},
                           {"144", "144"}, {"240", "240"}, {"off", "UNLIMITED"}};
// Named for what the player sees, not for how the scale gets there: "under"
// leaves black bars around the picture and "over" cuts off its edges, and
// those are the two things a row has to tell them apart.
const Choice kIntScale[] = {{"off", "OFF"}, {"under", "UNDER"}, {"over", "OVER"}};
const Choice kSeam[]     = {{"dark", "DARK"}, {"blend", "BLEND"}, {"blend_linear", "BLEND LINEAR"}};
// The file's own words on the left. "mean" is the ordinary cell and reads as
// DEFAULT; the rest are named for what they do rather than how they do it.
const Choice kChunky[]   = {{"false", "OFF"}, {"mean", "DEFAULT"}, {"extreme", "ADAPTIVE"},
                            {"mode", "COMMON"}, {"tl", "FIRST"}, {"min", "DARKEST"}, {"max", "LIGHTEST"}};
const Choice kScreen[]   = {{"top", "TOP"}, {"bottom", "BOTTOM"}};
// GBATEK's order, which is what the firmware stores. Named the way the rest of
// the row is: these are the colours the firmware offers and a player picks one
// by its name, so it is the name that has to be in the language that is set.
const Choice kColour[]   = {{"0", "GREY"}, {"1", "BROWN"}, {"2", "RED"}, {"3", "PINK"},
                            {"4", "ORANGE"}, {"5", "YELLOW"}, {"6", "LIME"}, {"7", "GREEN"},
                            {"8", "DARK GREEN"}, {"9", "TURQUOISE"}, {"10", "BLUE"}, {"11", "DARK BLUE"},
                            {"12", "PURPLE"}, {"13", "VIOLET"}, {"14", "MAGENTA"}, {"15", "DARK PINK"}};
// The firmware stores the month as a number; the menu names it, because
// "BIRTHDAY MONTH 11" takes a moment to read and "十一月" does not.
const Choice kMonth[]    = {{"1", "JANUARY"}, {"2", "FEBRUARY"}, {"3", "MARCH"}, {"4", "APRIL"},
                            {"5", "MAY"}, {"6", "JUNE"}, {"7", "JULY"}, {"8", "AUGUST"},
                            {"9", "SEPTEMBER"}, {"10", "OCTOBER"}, {"11", "NOVEMBER"}, {"12", "DECEMBER"}};
// Local wireless, as one row. AUTO is --netplay (join a session heard on the
// LAN, else host one); HOST and GUEST are the same scan with the decision
// already made, so a console meant to be the second one cannot quietly become
// the session everyone joins. The address form stays a command-line flag:
// a pick row has nowhere to put one, and discovery finds hosts on its own.
const Choice kNetMode[]  = {{"off", "OFF"}, {"auto", "AUTO"}, {"host", "HOST"}, {"guest", "GUEST"},
                            {"internet", "INTERNET"}};
// Where the game's DNS queries go once it is on the internet. Two values
// here; the config takes a third form, a plain address, which a pick row has
// nowhere to put (the same reason the LAN join address stayed a flag).
//
// WIIMMFI is the default because it is the only thing a DS can still reach:
// Nintendo WFC was switched off in 2014, so HOST -- the host's own resolver,
// which is what a real DS would have used -- resolves the game's servers to
// nothing. HOST is there for a private server or a local test.
const Choice kWifiDns[]  = {{"wiimmfi", "WIIMMFI"}, {"host", "HOST"}};
const Choice kLanguage[] = {{"0", "JAPANESE"}, {"1", "ENGLISH"}, {"2", "FRENCH"},
                            {"3", "GERMAN"}, {"4", "ITALIAN"}, {"5", "SPANISH"}};
// A checkbox: the same two values as kOnOff, drawn as a box. Rows that are
// members of a set rather than switches read better that way.
const Choice kCheck[]    = {{"false", "[ ]"}, {"true", "[X]"}};
// Display::mode_name's words, in Display::Mode order.
const Choice kLayout[]   = {{"vertical", "VERTICAL"}, {"horizontal", "HORIZONTAL"}, {"single", "SINGLE"},
                            {"pip", "PIP"}, {"dominant_v", "DOMINANT V"}, {"dominant_h", "DOMINANT H"}};
const Choice kCorner[]   = {{"tl", "TOP LEFT"}, {"tr", "TOP RIGHT"}, {"bl", "BOTTOM LEFT"}, {"br", "BOTTOM RIGHT"}};

constexpr Setting boolean(const char* k, const char* l, const char* def, u8 f, Dep d, const char* n) {
  return Setting{k, l, T::Bool, kOnOff, 2, 0, 0, 0, nullptr, nullptr, nullptr, def, f, d, n};
}
constexpr Setting check(const char* k, const char* l, const char* def, u8 f, Dep d, const char* n) {
  return Setting{k, l, T::Bool, kCheck, 2, 0, 0, 0, nullptr, nullptr, nullptr, def, f, d, n};
}
constexpr Setting pick(const char* k, const char* l, const Choice* c, u8 nc, const char* def, u8 f, Dep d, const char* n) {
  return Setting{k, l, T::Pick, c, nc, 0, 0, 0, nullptr, nullptr, nullptr, def, f, d, n};
}
constexpr Setting number(const char* k, const char* l, int lo, int hi, int st, const char* def, u8 f, Dep d, const char* n,
                         const char* sv = nullptr, const char* sl = nullptr, const char* sx = nullptr) {
  return Setting{k, l, T::Int, nullptr, 0, lo, hi, st, sv, sl, sx, def, f, d, n};
}
constexpr Setting percent(const char* k, const char* l, int lo, int hi, int st, const char* def, u8 f, Dep d, const char* n,
                          const char* sv = nullptr, const char* sl = nullptr) {
  return Setting{k, l, T::Percent, nullptr, 0, lo, hi, st, sv, sl, nullptr, def, f, d, n};
}
constexpr Setting text(const char* k, const char* l, int maxlen, const char* def, u8 f, const char* n) {
  return Setting{k, l, T::Text, nullptr, 0, maxlen, maxlen, 0, nullptr, nullptr, nullptr, def, f, Dep::None, n};
}
constexpr Setting end() { return Setting{nullptr, nullptr, T::Bool, nullptr, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr, 0, Dep::None, nullptr}; }

} // namespace

const Setting kEmuSettings[] = {
  number("emu.frameskip", "FRAMESKIP", 0, 3, 1, "0", FlagLive, Dep::NetSession,
         "DRAW FEWER FRAMES. THE GAME STILL RUNS IN FULL"),
  pick("emu.frameskip_mode", "FRAMESKIP MODE", kSkipMode, 2, "adaptive", FlagLive, Dep::FrameskipMode,
       "ADAPTIVE SKIPS ONLY WHILE BEHIND REAL TIME"),
  // emu.cpu_tuning and emu.timing_oc are gone in v3.0.0: the CPU TUNING row
  // and its kCpuOc table were dropped there, and the menu no longer steps it.
  // emu.fast_load went the same way -- nothing reads the key any more, and a
  // switch that does nothing but be switchable is worse than no switch. The
  // three-way merge kept all three, because the v2.1.1 side had them and the
  // merge took its CLI whitelist as a union.
  pick("emu.limiter", "FRAME LIMITER", kLimiter, 7, "auto", FlagLive, Dep::NetSession,
       "THE RATE THE GAME IS HELD TO. CONSOLE IS THE ONE THE GAME WAS WRITTEN FOR"),
  // A whole percent in the file ("100"), as --speed and the frontend read it --
  // not a percent row, which keeps a 0..1 fraction: that showed the default as
  // 10000% and wrote 1 (one percent) for what it displayed as 100%.
  number("emu.speed", "GAME SPEED", 25, 400, 5, "100", FlagLive, Dep::NetSession,
         "HOW FAST THE GAME RUNS AGAINST THE LIMITER", nullptr, nullptr, "%"),
  // From 2: "1X" is real time, which is what not fast-forwarding already is.
  // A multiple of real time, not of the limiter: it is a floor, so holding
  // fast forward on a 240 Hz limiter never slows the game down to it.
  // UNLIMITED uncaps whatever the limiter says, which is why it is still
  // here now that the limiter has an UNLIMITED of its own -- a 60 Hz game
  // with a fast forward that goes as fast as the machine will.
  number("emu.ff_speed", "FAST FORWARD SPEED", 2, 16, 1, "0", FlagLive, Dep::NetSession,
         "AT LEAST THIS MANY TIMES REAL TIME WHILE FAST FORWARD IS HELD", "0", "UNLIMITED", "X"),
  number("emu.ff_skip", "FAST FORWARD SKIP", 0, 9, 1, "3", FlagLive, Dep::NetSession,
         "WHILE FAST FORWARDING, SHOW ONE FRAME IN THIS MANY PLUS ONE"),
  // The one audio row, and it lives here rather than on an audio page of its
  // own: a page for a single row is not worth the depth it adds. AUTO holds
  // the default and raises it only when the machine is keeping up and still
  // ran the queue dry -- a deeper buffer answers a hitch, never a machine
  // that cannot keep up at all. docs/audio-buffer-scoping.md.
  number("audio.buffer_size", "AUDIO BUFFER", 20, 200, 10, "auto", FlagLive, Dep::None,
         "SOUND HELD AHEAD. LOWER IS LESS DELAY, LESS SLACK BEFORE A LATE FRAME IS HEARD",
         "auto", "AUTO", " MS"),
  boolean("emu.autosave", "AUTOSAVE ON QUIT", "false", FlagLive, Dep::None,
          "SAVE A STATE WHEN THE EMULATOR EXITS, TO RESUME FROM"),
  boolean("emu.autoload", "AUTOLOAD ON START", "false", FlagRestart, Dep::None,
          "WHEN A GAME STARTS, RESUME FROM ITS AUTOSAVED STATE IF THERE IS ONE"),
  // Applied when the NAND is opened, so only a new DSi session sees it.
  boolean("emu.dsi_hide_installed", "HIDE NAND DSIWARE", "false", FlagRestart, Dep::None,
          "DSI MENU: HIDE THE NAND DUMP'S OWN TITLES. THE DUMP IS NOT CHANGED"),
  // Live: switching it writes or clears the .dspr.nds files at once.
  boolean("emu.dsi_nand_shortcuts", "DSI NAND LINKS", "false", FlagLive, Dep::ShortcutsPath,
          "GAME LIST ENTRIES THAT START THE NAND'S DSIWARE WITHOUT THE DSI MENU"),
  // Live, and it was not always: it used to need a restart because the MAC was
  // only randomized into the firmware image for a run that asked for a session,
  // and two instances sharing a dump's MAC is the fault that made PictoChat
  // drop its own messages. The MAC is settled every run now, so the radio can
  // go up and come down whenever -- a Pokemon player can trade and then carry
  // on playing. docs/wifi-scoping.md.
  pick("net.mode", "NETWORK FEATURES", kNetMode, 5, "off", FlagLive, Dep::Net,
       "LOCAL WIRELESS, OR INTERNET. AUTO JOINS A SESSION, ELSE HOSTS ONE"),
  pick("wifi.dns", "DNS", kWifiDns, 2, "wiimmfi", FlagRestart, Dep::NetInternet,
       "WHERE THE GAME LOOKS UP ITS SERVERS. NINTENDO'S ARE GONE; WIIMMFI REPLACES THEM"),
  end(),
};

const Setting kVideoSettings[] = {
  pick("video.integer_scale", "INTEGER SCALE", kIntScale, 3, "off", FlagDeferred, Dep::None,
       "WHOLE PANEL PIXELS PER DS PIXEL. UNDER LETTERBOXES, OVER CROPS"),
  boolean("video.linear", "BILINEAR", "false", FlagDeferred, Dep::PanelEffects,
          "SMOOTH SCALING. OVERRIDES THE GRID, SEAMS AND CHUNKY"),
  percent("video.lcd_grid", "LCD GRID", 0, 100, 10, "0", FlagDeferred, Dep::GridSeam,
          "A DARK SEAM AROUND EVERY DS PIXEL, LIKE THE ORIGINAL SCREEN"),
  pick("video.seam", "SEAM", kSeam, 3, "dark", FlagDeferred, Dep::GridSeam,
       "DARK DRAWS THE GRID. BLEND SOFTENS ONLY THE STRADDLING PIXEL"),
  pick("video.chunky", "CHUNKY", kChunky, 7, "false", FlagDeferred, Dep::Chunky,
       "DRAW BLOCKS OF DS PIXELS AS ONE FLAT CELL, FOR PANELS AT ODD SCALES"),
  number("video.chunky_cell", "CHUNKY CELL", 2, 8, 1, "auto", FlagDeferred, Dep::ChunkyCell,
         "PANEL PIXELS PER CELL", "auto", "AUTO"),
  number("video.screen_gap", "SCREEN GAP", 0, 128, 4, "0", FlagDeferred, Dep::OneWindow,
         "PANEL PIXELS BETWEEN THE TWO SCREENS WHEN THEY ARE STACKED OR SIDE BY SIDE. AUTO PUSHES THEM TO THE EDGES",
         "auto", "AUTO"),
  // v3.0.0 起它只是一个开关：文件里写的是 true / false。旧的 accurate /
  // enhanced / smooth 仍按“开”读入，但效果已经相同，所以不再列成一串，
  // 否则菜单会在表里找不到当前值，原样画出文件的 true / false。
  boolean("video.aa", "ANTI-ALIASING", "false", FlagLive, Dep::None,
          "3D EDGES: THE HARDWARE'S BLEND ON THE CPU RASTER, 4X MSAA ON THE GPU RASTER"),
  boolean("video.gpu3d", "GPU 3D", "false", FlagLive, Dep::None,
          "DRAW THE 3D LAYER ON THE GPU. NEEDS VULKAN; THE CPU DRAWS IT OTHERWISE"),
  boolean("video.fps", "FPS COUNTER", "false", FlagLive, Dep::None,
          "FRAMES PER SECOND IN THE CORNER OF THE SCREEN"),
  boolean("video.fullscreen", "FULLSCREEN", "false", FlagDeferred, Dep::Windowed, nullptr),
  end(),
};

const Setting kLayoutSettings[] = {
  pick("video.layout", "LAYOUT", kLayout, 6, "vertical", FlagDeferred, Dep::OneWindow,
       "HOW THE TWO SCREENS SHARE THE WINDOW"),
  pick("video.screen", "MAIN SCREEN", kScreen, 2, "top", FlagDeferred, Dep::None,
       "THE SCREEN SHOWN ALONE, LARGE OR FIRST"),
  pick("video.pip_corner", "PIP CORNER", kCorner, 4, "br", FlagDeferred, Dep::Pip,
       "WHERE THE SMALL SCREEN SITS"),
  percent("video.pip_scale", "PIP SIZE", 10, 90, 5, "0.33", FlagDeferred, Dep::Pip,
          "HOW BIG THE SMALL SCREEN IS AGAINST THE LARGE ONE"),
  percent("video.pip_alpha", "PIP OPACITY", 0, 100, 10, "1", FlagDeferred, Dep::Pip,
          "HOW SOLID THE SMALL SCREEN IS AT REST"),
  number("video.pip_touch_hold", "PIP TOUCH HOLD", 10, 300, 10, "60", FlagLive, Dep::PipTouchHold,
         "FRAMES THE SMALL SCREEN STAYS SOLID AFTER IT IS TOUCHED", "0", "NEVER FADE"),
  percent("video.dominant_ratio", "DOMINANT RATIO", 10, 90, 5, "auto", FlagDeferred, Dep::Dominant,
          "THE SMALLER SCREEN'S SIZE. AUTO FITS WHOLE PIXELS", "auto", "AUTO"),
  percent("video.dominant_threshold", "DOMINANT THRESHOLD", 10, 99, 5, "0.25", FlagDeferred, Dep::DominantThreshold,
          "THE SMALLEST SECONDARY AUTO WILL ACCEPT"),
  // What the layout hotkeys step through, one box per layout. Live: the
  // hotkeys read the list when pressed, and nothing on screen moves.
  check("video.layout_cycle.vertical", "CYCLE VERTICAL", "true", FlagLive, Dep::OneWindow,
        "THE LAYOUT HOTKEYS STEP THROUGH THE TICKED LAYOUTS"),
  check("video.layout_cycle.horizontal", "CYCLE HORIZONTAL", "true", FlagLive, Dep::OneWindow,
        "THE LAYOUT HOTKEYS STEP THROUGH THE TICKED LAYOUTS"),
  check("video.layout_cycle.single", "CYCLE SINGLE", "true", FlagLive, Dep::OneWindow,
        "THE LAYOUT HOTKEYS STEP THROUGH THE TICKED LAYOUTS"),
  check("video.layout_cycle.pip", "CYCLE PIP", "true", FlagLive, Dep::OneWindow,
        "THE LAYOUT HOTKEYS STEP THROUGH THE TICKED LAYOUTS"),
  check("video.layout_cycle.dominant_v", "CYCLE DOMINANT V", "true", FlagLive, Dep::OneWindow,
        "THE LAYOUT HOTKEYS STEP THROUGH THE TICKED LAYOUTS"),
  check("video.layout_cycle.dominant_h", "CYCLE DOMINANT H", "true", FlagLive, Dep::OneWindow,
        "THE LAYOUT HOTKEYS STEP THROUGH THE TICKED LAYOUTS. ONE ALWAYS STAYS"),
  end(),
};

// [user]: what a game sees as the console's owner. Restart-only: baked into
// generated firmware at boot; unused with a real dump.
const Setting kUserSettings[] = {
  text("user.nickname", "NICKNAME", 10, "DSperate", FlagRestart,
       "WHAT GAMES CALL YOU"),
  text("user.message", "MESSAGE", 26, "", FlagRestart,
       "THE GREETING THE DS MENU SHOWS"),
  pick("user.colour", "FAVOURITE COLOUR", kColour, 16, "0", FlagRestart, Dep::None,
       "SOME GAMES COLOUR THEMSELVES WITH IT"),
  pick("user.birthday_month", "BIRTHDAY MONTH", kMonth, 12, "1", FlagRestart, Dep::None,
       "GAMES THAT WISH YOU A HAPPY BIRTHDAY USE THIS"),
  number("user.birthday_day", "BIRTHDAY DAY", 1, 31, 1, "1", FlagRestart, Dep::None,
         "NOT CHECKED AGAINST THE MONTH, AS THE CONSOLE DOES NOT EITHER"),
  pick("user.language", "LANGUAGE", kLanguage, 6, "1", FlagRestart, Dep::None,
       "THE LANGUAGE MULTI-LANGUAGE GAMES START IN"),
  end(),
};

// [input]: turbo, one checkbox per DS button. A held button that is ticked
// here is handed to the game as a square wave -- half a period down, half up
// -- so holding it is the same as tapping it `input.turbo_rate` times a
// second. Nothing below the frontend knows: Input::frame() is the only place
// the mask is shaped, and the core sees an ordinary button that let go.
//
// The rate and the checkboxes hang off the switch (Dep::Turbo) because a rate
// with the switch off reads as a knob that does nothing. The button rows are
// checkboxes rather than switches for the reason video.layout_cycle is: they
// are members of a set, and a set of boxes reads as a set where a column of
// 关/开 does not.
// Turbo mode: hold = the button must stay down to fire; tap = one press
// latches it on and it keeps firing until the next press.
const Choice kTurboMode[] = {{"hold", "HOLD"}, {"toggle", "TAP"}};
const Setting kInputSettings[] = {
  boolean("input.turbo", "TURBO", "false", FlagLive, Dep::None,
          "WHEN ON, TICKED BUTTONS AUTOFIRE (SEE TURBO MODE)"),
  // hold: the button fires only while held (the original behaviour). toggle:
  // a single press latches it on and it keeps firing until pressed again, so
  // you do not have to keep the button down.
  pick("input.turbo_mode", "TURBO MODE", kTurboMode, 2, "hold", FlagLive, Dep::Turbo,
       "HOLD: FIRES ONLY WHILE HELD. TAP: PRESS ONCE TO KEEP FIRING, PRESS AGAIN TO STOP"),
  // HZ, not 次/秒: the suffix joins the number before tr_text sees the pair,
  // so a Chinese unit here would be drawn in Chinese with English set.
  number("input.turbo_rate", "TURBO RATE", 5, 30, 1, "12", FlagLive, Dep::Turbo,
         "TAPS AND RELEASES PER SECOND. TOO FAST AND A GAME MAY MISS ONE", nullptr, nullptr, " HZ"),
  check("input.turbo.a", "TURBO A", "false", FlagLive, Dep::Turbo, nullptr),
  check("input.turbo.b", "TURBO B", "false", FlagLive, Dep::Turbo, nullptr),
  check("input.turbo.select", "TURBO SELECT", "false", FlagLive, Dep::Turbo, nullptr),
  check("input.turbo.start", "TURBO START", "false", FlagLive, Dep::Turbo, nullptr),
  check("input.turbo.right", "TURBO RIGHT", "false", FlagLive, Dep::Turbo, nullptr),
  check("input.turbo.left", "TURBO LEFT", "false", FlagLive, Dep::Turbo, nullptr),
  check("input.turbo.up", "TURBO UP", "false", FlagLive, Dep::Turbo, nullptr),
  check("input.turbo.down", "TURBO DOWN", "false", FlagLive, Dep::Turbo, nullptr),
  check("input.turbo.r", "TURBO R", "false", FlagLive, Dep::Turbo, nullptr),
  check("input.turbo.l", "TURBO L", "false", FlagLive, Dep::Turbo, nullptr),
  check("input.turbo.x", "TURBO X", "false", FlagLive, Dep::Turbo, nullptr),
  check("input.turbo.y", "TURBO Y", "false", FlagLive, Dep::Turbo, nullptr),
  end(),
};

int settings_count(const Setting* table) {
  int n = 0;
  while (table[n].key) ++n;
  return n;
}

namespace {

bool truthy(const std::string& v) {
  return v == "1" || v == "true" || v == "yes" || v == "on";
}

// Percent rows keep a 0..1 double in the file; round to the nearest whole
// percent so display and re-stepped values agree.
int percent_of(const std::string& v) {
  return static_cast<int>(std::lround(std::atof(v.c_str()) * 100.0));
}
std::string percent_str(int p) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%.2f", p / 100.0);
  std::string s = buf;   // trim trailing zeros: "0.50" -> "0.5"
  if (s.find('.') != std::string::npos) {
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
  }
  return s;
}

int choice_index(const Setting& s, const std::string& v) {
  for (int i = 0; i < s.nchoices; ++i) {
    if (v == s.choices[i].value) return i;
    if (s.type == T::Bool && truthy(v) == truthy(s.choices[i].value)) return i;   // tolerate hand-edited variants
  }
  return -1;
}

} // namespace

std::string default_value(const Setting& s) {
  if (s.def) return s.def;
  switch (s.type) {
  case T::Bool:
  case T::Pick:    return s.choices[0].value;
  case T::Int:
  case T::Percent: return s.sentinel_value ? s.sentinel_value : std::to_string(s.lo);
  case T::Text:    return "";
  }
  return "";
}

std::string display_value(const Setting& s, const std::string& value) {
  // Unset text shows the firmware's default, not "--"; only an explicitly emptied field reads as empty.
  if (s.type == T::Text) {
    const std::string v = value.empty() ? default_value(s) : value;
    return v.empty() ? "--" : v;
  }
  const std::string v = value.empty() ? default_value(s) : value;
  switch (s.type) {
  case T::Text: break;   // handled above
  case T::Bool:
  case T::Pick: {
    const int i = choice_index(s, v);
    // A value the table does not know is shown as it stands rather than
    // silently redrawn as something else: the file said it, and the player
    // should see what the file said.
    // The label is the text as written. draw_text resolves it for the language
    // being drawn, so the row the player sees is in that language.
    return i >= 0 ? s.choices[i].label : v;
  }
  case T::Int:
    if (s.sentinel_value && v == s.sentinel_value) return s.sentinel_label;
    return s.suffix ? v + s.suffix : v;
  case T::Percent: {
    if (s.sentinel_value && v == s.sentinel_value) return s.sentinel_label;
    return std::to_string(percent_of(v)) + "%";
  }
  }
  return v;
}

std::string step_value(const Setting& s, const std::string& value, int dir, const SettingsHost& host) {
  if (s.type == T::Text) return value;   // stepped a character at a time, not as a whole
  const std::string v = value.empty() ? default_value(s) : value;
  if (s.type == T::Bool || s.type == T::Pick) {
    int i = choice_index(s, v);
    if (i < 0) i = 0;
    // Wraps (lists are short); skips choices the tier disallows; gives up if none are allowed.
    for (int n = 0; n < s.nchoices; ++n) {
      i = (i + (dir > 0 ? 1 : s.nchoices - 1)) % s.nchoices;
      if (host.value_allowed(s, s.choices[i].value)) return s.choices[i].value;
    }
    return v;
  }
  const bool at_sentinel = s.sentinel_value && v == s.sentinel_value;
  if (at_sentinel) {   // sentinel sits one step below lo: down is a no-op, up lands on lo
    if (dir <= 0) return v;
    return s.type == T::Percent ? percent_str(s.lo) : std::to_string(s.lo);
  }
  const int cur = s.type == T::Percent ? percent_of(v) : std::atoi(v.c_str());
  int next = cur + dir * s.step;
  if (next < s.lo) {
    if (s.sentinel_value) return s.sentinel_value;
    next = s.lo;
  }
  if (next > s.hi) next = s.hi;
  return s.type == T::Percent ? percent_str(next) : std::to_string(next);
}

} // namespace ds::sdl
