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

const Choice kOnOff[]   = {{"false", "关"}, {"true", "开"}};
const Choice kSkipMode[] = {{"adaptive", "自适应"}, {"fixed", "固定"}};
// The rates a panel comes in, plus the console's own and no limiter at all.
// "auto" is the default and the only one that plays a game at the speed it was
// written for; 60 is 0.29 % fast, which is what a player means by "60 fps"
// and what lines up with a 60 Hz panel. Both ends are named in the row's own
// language rather than left in English: the rest of the row is in the language
// that is set, and a value in another one is the odd thing out on it.
const Choice kLimiter[] = {{"auto", "实机 (59.8)"}, {"30", "30"}, {"60", "60"}, {"120", "120"},
                           {"144", "144"}, {"240", "240"}, {"off", "无限制"}};
// Named for what the player sees, not for how the scale gets there: "under"
// leaves black bars around the picture and "over" cuts off its edges, and
// those are the two things a row has to tell them apart.
const Choice kIntScale[] = {{"off", "关"}, {"under", "留黑边"}, {"over", "裁切"}};
const Choice kSeam[]     = {{"dark", "暗色"}, {"blend", "混合"}, {"blend_linear", "线性混合"}};
// The file's own words on the left. "mean" is the ordinary cell and reads as
// DEFAULT; the rest are named for what they do rather than how they do it.
const Choice kChunky[]   = {{"false", "关"}, {"mean", "默认"}, {"extreme", "自适应"},
                            {"mode", "众数"}, {"tl", "首个"}, {"min", "最暗"}, {"max", "最亮"}};
const Choice kScreen[]   = {{"top", "上屏"}, {"bottom", "下屏"}};
// GBATEK's order, which is what the firmware stores. Named the way the rest of
// the row is: these are the colours the firmware offers and a player picks one
// by its name, so it is the name that has to be in the language that is set.
const Choice kColour[]   = {{"0", "灰"}, {"1", "棕"}, {"2", "红"}, {"3", "粉"},
                            {"4", "橙"}, {"5", "黄"}, {"6", "青柠"}, {"7", "绿"},
                            {"8", "深绿"}, {"9", "青绿"}, {"10", "蓝"}, {"11", "深蓝"},
                            {"12", "紫"}, {"13", "紫罗兰"}, {"14", "洋红"}, {"15", "暗粉"}};
// The firmware stores the month as a number; the menu names it, because
// "BIRTHDAY MONTH 11" takes a moment to read and "十一月" does not.
const Choice kMonth[]    = {{"1", "一月"}, {"2", "二月"}, {"3", "三月"}, {"4", "四月"},
                            {"5", "五月"}, {"6", "六月"}, {"7", "七月"}, {"8", "八月"},
                            {"9", "九月"}, {"10", "十月"}, {"11", "十一月"}, {"12", "十二月"}};
// Local wireless, as one row. AUTO is --netplay (join a session heard on the
// LAN, else host one); HOST and GUEST are the same scan with the decision
// already made, so a console meant to be the second one cannot quietly become
// the session everyone joins. The address form stays a command-line flag:
// a pick row has nowhere to put one, and discovery finds hosts on its own.
const Choice kNetMode[]  = {{"off", "关"}, {"auto", "自动"}, {"host", "主机"}, {"guest", "客机"},
                            {"internet", "互联网"}};
// Where the game's DNS queries go once it is on the internet. Two values
// here; the config takes a third form, a plain address, which a pick row has
// nowhere to put (the same reason the LAN join address stayed a flag).
//
// WIIMMFI is the default because it is the only thing a DS can still reach:
// Nintendo WFC was switched off in 2014, so HOST -- the host's own resolver,
// which is what a real DS would have used -- resolves the game's servers to
// nothing. HOST is there for a private server or a local test.
const Choice kWifiDns[]  = {{"wiimmfi", "WIIMMFI"}, {"host", "主机"}};
const Choice kLanguage[] = {{"0", "日语"}, {"1", "英语"}, {"2", "法语"},
                            {"3", "德语"}, {"4", "意大利语"}, {"5", "西班牙语"}};
// A checkbox: the same two values as kOnOff, drawn as a box. Rows that are
// members of a set rather than switches read better that way.
const Choice kCheck[]    = {{"false", "[ ]"}, {"true", "[X]"}};
// Display::mode_name's words, in Display::Mode order.
const Choice kLayout[]   = {{"vertical", "纵向"}, {"horizontal", "横向"}, {"single", "单屏"},
                            {"pip", "画中画"}, {"dominant_v", "主次纵向"}, {"dominant_h", "主次横向"}};
const Choice kCorner[]   = {{"tl", "左上角"}, {"tr", "右上角"}, {"bl", "左下角"}, {"br", "右下角"}};

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
  number("emu.frameskip", "跳帧", 0, 3, 1, "0", FlagLive, Dep::NetSession,
         "减少绘制的帧数，游戏仍全速运行"),
  pick("emu.frameskip_mode", "跳帧模式", kSkipMode, 2, "adaptive", FlagLive, Dep::FrameskipMode,
       "自适应只在落后实机速度时才跳帧"),
  // emu.cpu_tuning and emu.timing_oc are gone in v3.0.0: the CPU TUNING row
  // and its kCpuOc table were dropped there, and the menu no longer steps it.
  // emu.fast_load went the same way -- nothing reads the key any more, and a
  // switch that does nothing but be switchable is worse than no switch. The
  // three-way merge kept all three, because the v2.1.1 side had them and the
  // merge took its CLI whitelist as a union.
  pick("emu.limiter", "帧率上限", kLimiter, 7, "auto", FlagLive, Dep::NetSession,
       "游戏被限制到的帧率，实机即原目标帧率"),
  // A whole percent in the file ("100"), as --speed and the frontend read it --
  // not a percent row, which keeps a 0..1 fraction: that showed the default as
  // 10000% and wrote 1 (one percent) for what it displayed as 100%.
  number("emu.speed", "游戏速度", 25, 400, 5, "100", FlagLive, Dep::NetSession,
         "相对帧率上限的运行速度", nullptr, nullptr, "%"),
  // From 2: "1X" is real time, which is what not fast-forwarding already is.
  // A multiple of real time, not of the limiter: it is a floor, so holding
  // fast forward on a 240 Hz limiter never slows the game down to it.
  // UNLIMITED uncaps whatever the limiter says, which is why it is still
  // here now that the limiter has an UNLIMITED of its own -- a 60 Hz game
  // with a fast forward that goes as fast as the machine will.
  number("emu.ff_speed", "快进速度", 2, 16, 1, "0", FlagLive, Dep::NetSession,
         "按住快进时至少达到此倍实时速度", "0", "无限制", "X"),
  number("emu.ff_skip", "快进跳帧", 0, 9, 1, "3", FlagLive, Dep::NetSession,
         "快进时每 N+1 帧只显示一帧"),
  // The one audio row, and it lives here rather than on an audio page of its
  // own: a page for a single row is not worth the depth it adds. AUTO holds
  // the default and raises it only when the machine is keeping up and still
  // ran the queue dry -- a deeper buffer answers a hitch, never a machine
  // that cannot keep up at all. docs/audio-buffer-scoping.md.
  number("audio.buffer_size", "音频缓冲", 20, 200, 10, "auto", FlagLive, Dep::None,
         "提前缓冲的声音，越低延迟越小，也更容易听到卡顿",
         "auto", "自动", " MS"),
  boolean("emu.autosave", "退出时存档", "false", FlagLive, Dep::None,
          "退出模拟器时自动存档以便续玩"),
  boolean("emu.autoload", "启动时读档", "false", FlagRestart, Dep::None,
          "启动游戏时若有存档则自动继续"),
  // Applied when the NAND is opened, so only a new DSi session sees it.
  boolean("emu.dsi_hide_installed", "隐藏 NAND 的 DSiWare", "false", FlagRestart, Dep::None,
          "在 DSi 菜单隐藏 NAND 自带软件，镜像本身不变"),
  // Live: switching it writes or clears the .dspr.nds files at once.
  boolean("emu.dsi_nand_shortcuts", "DSi NAND 链接", "false", FlagLive, Dep::ShortcutsPath,
          "在游戏列表中直接启动 NAND 内的 DSiWare"),
  // Live, and it was not always: it used to need a restart because the MAC was
  // only randomized into the firmware image for a run that asked for a session,
  // and two instances sharing a dump's MAC is the fault that made PictoChat
  // drop its own messages. The MAC is settled every run now, so the radio can
  // go up and come down whenever -- a Pokemon player can trade and then carry
  // on playing. docs/wifi-scoping.md.
  pick("net.mode", "网络功能", kNetMode, 5, "off", FlagLive, Dep::Net,
       "本地无线通信或联网。自动会先尝试加入会话"),
  pick("wifi.dns", "DNS", kWifiDns, 2, "wiimmfi", FlagRestart, Dep::NetInternet,
       "游戏查询服务器的方式。任天堂的已关停，改用 Wiimmfi"),
  end(),
};

const Setting kVideoSettings[] = {
  pick("video.integer_scale", "整数缩放", kIntScale, 3, "off", FlagDeferred, Dep::None,
       "每个 DS 像素占用的整数面板像素，不足留黑边，超出会裁切"),
  boolean("video.linear", "线性过滤", "false", FlagDeferred, Dep::PanelEffects,
          "平滑缩放，会覆盖网格、接缝与像素块"),
  percent("video.lcd_grid", "液晶网格", 0, 100, 10, "0", FlagDeferred, Dep::GridSeam,
          "在每个 DS 像素周围画暗缝，模拟原生屏幕"),
  pick("video.seam", "接缝", kSeam, 3, "dark", FlagDeferred, Dep::GridSeam,
       "暗色会绘制网格，混合只柔化跨界的那一像素"),
  pick("video.chunky", "像素块", kChunky, 7, "false", FlagDeferred, Dep::Chunky,
       "把相邻 DS 像素合并为一个色块，适合非整数缩放"),
  number("video.chunky_cell", "色块尺寸", 2, 8, 1, "auto", FlagDeferred, Dep::ChunkyCell,
         "每个色块占用的面板像素数", "auto", "自动"),
  number("video.screen_gap", "屏幕间距", 0, 128, 4, "0", FlagDeferred, Dep::OneWindow,
         "上下叠放或左右并排时两块屏幕之间的面板像素间距。自动会把间距推到屏幕边缘",
         "auto", "自动"),
  // v3.0.0 起它只是一个开关：文件里写的是 true / false。旧的 accurate /
  // enhanced / smooth 仍按“开”读入，但效果已经相同，所以不再列成一串，
  // 否则菜单会在表里找不到当前值，原样画出文件的 true / false。
  boolean("video.aa", "抗锯齿", "false", FlagLive, Dep::None,
          "3D 边缘，CPU 渲染时按硬件那样混合，GPU 渲染时按 4 倍多重采样混合"),
  boolean("video.gpu3d", "图形加速", "false", FlagLive, Dep::None,
          "在 GPU 上绘制 3D 图层，需要 Vulkan。否则仍由 CPU 绘制"),
  boolean("video.fps", "帧数显示", "false", FlagLive, Dep::None,
          "在屏幕角落显示每秒帧数"),
  boolean("video.fullscreen", "全屏", "false", FlagDeferred, Dep::Windowed, nullptr),
  end(),
};

const Setting kLayoutSettings[] = {
  pick("video.layout", "布局", kLayout, 6, "vertical", FlagDeferred, Dep::OneWindow,
       "两个屏幕在窗口中的排列方式"),
  pick("video.screen", "主屏幕", kScreen, 2, "top", FlagDeferred, Dep::None,
       "单独显示、放大显示或排在前面的屏幕"),
  pick("video.pip_corner", "画中画位置", kCorner, 4, "br", FlagDeferred, Dep::Pip,
       "小屏幕所处的位置"),
  percent("video.pip_scale", "画中画大小", 10, 90, 5, "0.33", FlagDeferred, Dep::Pip,
          "小屏相对大屏的尺寸比例"),
  percent("video.pip_alpha", "画中画不透明度", 0, 100, 10, "1", FlagDeferred, Dep::Pip,
          "静止时小屏幕的实心程度"),
  number("video.pip_touch_hold", "画中画触摸保持", 10, 300, 10, "60", FlagLive, Dep::PipTouchHold,
         "被触摸后小屏幕保持实心的帧数", "0", "永不淡出"),
  percent("video.dominant_ratio", "主次比例", 10, 90, 5, "auto", FlagDeferred, Dep::Dominant,
          "副屏的尺寸占比，自动按整像素适配", "auto", "自动"),
  percent("video.dominant_threshold", "主次阈值", 10, 99, 5, "0.25", FlagDeferred, Dep::DominantThreshold,
          "自动模式下可接受的最小副屏尺寸"),
  // What the layout hotkeys step through, one box per layout. Live: the
  // hotkeys read the list when pressed, and nothing on screen moves.
  check("video.layout_cycle.vertical", "循环 纵向", "true", FlagLive, Dep::OneWindow,
        "布局快捷键会在勾选的布局之间循环"),
  check("video.layout_cycle.horizontal", "循环 横向", "true", FlagLive, Dep::OneWindow,
        "布局快捷键会在勾选的布局之间循环"),
  check("video.layout_cycle.single", "循环 单屏", "true", FlagLive, Dep::OneWindow,
        "布局快捷键会在勾选的布局之间循环"),
  check("video.layout_cycle.pip", "循环 画中画", "true", FlagLive, Dep::OneWindow,
        "布局快捷键会在勾选的布局之间循环"),
  check("video.layout_cycle.dominant_v", "循环 主次纵向", "true", FlagLive, Dep::OneWindow,
        "布局快捷键会在勾选的布局之间循环"),
  check("video.layout_cycle.dominant_h", "循环 主次横向", "true", FlagLive, Dep::OneWindow,
        "布局快捷键在勾选的布局间循环，始终保留一个"),
  end(),
};

// [user]: what a game sees as the console's owner. Restart-only: baked into
// generated firmware at boot; unused with a real dump.
const Setting kUserSettings[] = {
  text("user.nickname", "昵称", 10, "DSperate", FlagRestart,
       "游戏中显示的名字"),
  text("user.message", "留言", 26, "", FlagRestart,
       "DS 菜单上显示的问候语"),
  pick("user.colour", "喜欢的颜色", kColour, 16, "0", FlagRestart, Dep::None,
       "部分游戏会用它做自己的配色"),
  pick("user.birthday_month", "生日月份", kMonth, 12, "1", FlagRestart, Dep::None,
       "部分游戏会在这个月祝你生日快乐"),
  number("user.birthday_day", "生日日期", 1, 31, 1, "1", FlagRestart, Dep::None,
         "不与月份校验，与实机行为一致"),
  pick("user.language", "主机语言", kLanguage, 6, "1", FlagRestart, Dep::None,
       "多语言游戏的初始语言"),
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
const Choice kTurboMode[] = {{"hold", "长按"}, {"toggle", "单击"}};
const Setting kInputSettings[] = {
  boolean("input.turbo", "连发", "false", FlagLive, Dep::None,
          "开启后，勾选的按键可自动连发（方式见连发模式）"),
  // hold: the button fires only while held (the original behaviour). toggle:
  // a single press latches it on and it keeps firing until pressed again, so
  // you do not have to keep the button down.
  pick("input.turbo_mode", "连发模式", kTurboMode, 2, "hold", FlagLive, Dep::Turbo,
       "长按：按住才连发。单击：按一下开始持续连发，再按一下停止"),
  // HZ, not 次/秒: the suffix joins the number before tr_text sees the pair,
  // so a Chinese unit here would be drawn in Chinese with English set.
  number("input.turbo_rate", "连发速率", 5, 30, 1, "12", FlagLive, Dep::Turbo,
         "每秒按下并松开的次数。太快游戏可能来不及认出单次按下", nullptr, nullptr, " HZ"),
  check("input.turbo.a", "连发 A", "false", FlagLive, Dep::Turbo, nullptr),
  check("input.turbo.b", "连发 B", "false", FlagLive, Dep::Turbo, nullptr),
  check("input.turbo.select", "连发 SELECT", "false", FlagLive, Dep::Turbo, nullptr),
  check("input.turbo.start", "连发 START", "false", FlagLive, Dep::Turbo, nullptr),
  check("input.turbo.right", "连发 右", "false", FlagLive, Dep::Turbo, nullptr),
  check("input.turbo.left", "连发 左", "false", FlagLive, Dep::Turbo, nullptr),
  check("input.turbo.up", "连发 上", "false", FlagLive, Dep::Turbo, nullptr),
  check("input.turbo.down", "连发 下", "false", FlagLive, Dep::Turbo, nullptr),
  check("input.turbo.r", "连发 R", "false", FlagLive, Dep::Turbo, nullptr),
  check("input.turbo.l", "连发 L", "false", FlagLive, Dep::Turbo, nullptr),
  check("input.turbo.x", "连发 X", "false", FlagLive, Dep::Turbo, nullptr),
  check("input.turbo.y", "连发 Y", "false", FlagLive, Dep::Turbo, nullptr),
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
