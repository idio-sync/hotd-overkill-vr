#include "settings_file.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace hv::vr {

SettingsWrite write_settings_if_missing(const std::wstring& dir, const wchar_t* name, std::string_view text, std::string* error) {
    namespace fs = std::filesystem;
    const fs::path path = fs::path(dir) / name;
    std::error_code ec;
    if (fs::exists(path, ec)) return SettingsWrite::kept;
    fs::create_directories(fs::path(dir), ec);  // an existing folder is fine; a failure shows at the open
    // "x": exclusive create - fails with EEXIST rather than replace a file that exists by now.
    FILE* f = nullptr;
    const errno_t err = _wfopen_s(&f, path.c_str(), L"wbx");
    if (err || !f) {
        if (err == EEXIST) return SettingsWrite::kept;
        char why[128] = "unknown error";
        strerror_s(why, sizeof why, err);
        if (error) *error = std::string("cannot create the file: ") + why;
        return SettingsWrite::failed;
    }
    std::string crlf;
    crlf.reserve(text.size() + text.size() / 16);
    for (char c : text) {
        if (c == '\r') continue;
        if (c == '\n') crlf += '\r';
        crlf += c;
    }
    const bool ok = std::fwrite(crlf.data(), 1, crlf.size(), f) == crlf.size();
    const bool closed = std::fclose(f) == 0;
    if (!ok || !closed) {
        fs::remove(path, ec);  // never leave a half-written settings file
        if (error) *error = "writing the file failed";
        return SettingsWrite::failed;
    }
    return SettingsWrite::written;
}

static std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

static bool iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

bool ini_text_value(std::string_view text, std::string_view section, std::string_view key, std::string* value) {
    bool in_section = false;
    while (!text.empty()) {
        const size_t nl = text.find('\n');
        std::string_view line = trim(text.substr(0, nl));
        text = nl == std::string_view::npos ? std::string_view() : text.substr(nl + 1);
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        if (line.front() == '[') {
            const size_t close = line.find(']');
            in_section = close != std::string_view::npos && iequal(trim(line.substr(1, close - 1)), section);
            continue;
        }
        const size_t eq = line.find('=');
        if (!in_section || eq == std::string_view::npos || !iequal(trim(line.substr(0, eq)), key)) continue;
        if (value) *value = std::string(trim(line.substr(eq + 1)));
        return true;
    }
    return false;
}

}  // namespace hv::vr
