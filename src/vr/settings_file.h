#pragma once
// The mod's first-run settings file (release installs). Plain file
// logic without the game, unit tested (tests/test_stereo.cpp); src/mod/config.cpp applies it.
//
// A release install is the two DLLs copied into the game folder: nothing writes hovr\hovr.ini
// there, so at the first start the mod writes the default settings itself - the text of
// tools/hovr.default.ini, built into the DLL (generated header hovr_default_ini.h, kDefaultIni).
// An existing hovr.ini (a player's own settings) is never replaced: the file is opened with
// exclusive create, so even a file that appears between the check and the write is kept.

#include <string>
#include <string_view>

namespace hv::vr {

enum class SettingsWrite {
    kept,     // dir\hovr.ini (or anything of that name) already exists: left untouched
    written,  // it was missing: dir was created if needed and the text written (CRLF line ends)
    failed,   // it was missing and could not be written (*error says why); nothing left behind
};

// Writes `text` to dir\name only when no file of that name exists (dir is created first if missing).
SettingsWrite write_settings_if_missing(const std::wstring& dir, const wchar_t* name, std::string_view text, std::string* error);

// The value of `key` in [section] of an ini text (case-insensitive names, spaces around '=' and
// the value trimmed, ';' and '#' lines are comments). For running on the built-in defaults when
// hovr.ini could not be written.
bool ini_text_value(std::string_view text, std::string_view section, std::string_view key, std::string* value);

}  // namespace hv::vr
