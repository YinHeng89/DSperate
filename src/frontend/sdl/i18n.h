// SPDX-License-Identifier: GPL-3.0-or-later
// UI language switching for the pause menu.
//
// The menu's UI text is written in English -- the language upstream writes it
// in, so a rebase stays readable -- and the Chinese lives in one table,
// tr_data.inc, keyed by that English. Which language is drawn is decided once
// per frame, from the `ui.language` config key, by Menu::draw -- and read
// everywhere else through tr_text(), which draw_text() and text_width()
// consult before they measure or paint a string. So a single setting flips the
// whole menu, including the settings pages, with no per-string bookkeeping at
// the call sites, and giving a new upstream string its Chinese is one line in
// the table and nothing at the call site.
#pragma once
#include "core/types.h"

namespace ds::sdl {

// The two UI languages. Stored as the config value "en" / "zh".
enum class UiLang : u8 { En = 0, Zh = 1 };

// Set every frame by Menu::draw from host_->get("ui.language"). Read by ds_tr
// below and by tr_text() (defined in menu.cpp, where the text layer lives).
extern UiLang g_ui_lang;

// Resolve `s` to the string for the current UI language. Declared here so a
// caller that only needs the resolution -- a test asserting on the text a row
// draws, say -- does not have to reach into menu.cpp.
const char* tr_text(const char* s);

// The leak check draw_text() and text_width() run on every string they are
// handed: with English set, a string carrying a Han character or a fullwidth
// form is a Chinese literal the sources should no longer hold, so the player
// is about to read a row in the wrong language. Off unless the tests turn it
// on -- a leak and a correctly drawn English row look identical on screen, so
// nothing in the product can tell them apart, and the tests are the only place
// that can. The other direction -- English drawn with Chinese set because the
// table has no entry for it -- is answered by tools/gen_tr.py, not here.
extern bool g_strict_i18n;
extern int g_i18n_leaks;

// Convenience for the rare call site that already has both forms in hand:
// returns `zh` in Chinese mode, `en` otherwise.
inline const char* ds_tr(const char* en, const char* zh) {
  return g_ui_lang == UiLang::Zh ? zh : en;
}

} // namespace ds::sdl
