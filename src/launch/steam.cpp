#include "launch/steam.hpp"

#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

namespace sf::launch::steam {
namespace {

// The quoted tokens of a text VDF file, unescaped.
std::vector<std::string> vdf_tokens(const std::string& text) {
    std::vector<std::string> out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '"') continue;
        std::string tok;
        for (++i; i < text.size() && text[i] != '"'; ++i) {
            if (text[i] == '\\') {
                if (++i >= text.size()) break;
            }
            tok += text[i];
        }
        out.push_back(tok);
    }
    return out;
}

bool iequals(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               auto low = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
               return low(x) == low(y);
           });
}

bool is_dir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

#ifdef _WIN32
std::optional<fs::path> registry_steam_path() {
    wchar_t buf[1024];
    DWORD size = sizeof buf;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, buf, &size) !=
        ERROR_SUCCESS)
        return std::nullopt;
    return fs::path(buf);
}
#endif

}  // namespace

std::vector<std::string> vdf_values(const std::string& text, const std::string& key) {
    const auto toks = vdf_tokens(text);
    std::vector<std::string> out;
    for (size_t i = 0; i + 1 < toks.size(); ++i)
        if (iequals(toks[i], key)) out.push_back(toks[i + 1]);
    return out;
}

std::vector<fs::path> steam_roots() {
    std::vector<fs::path> candidates;
#ifdef _WIN32
    if (auto p = registry_steam_path()) candidates.push_back(*p);
    for (const char* var : {"ProgramFiles(x86)", "ProgramFiles"})
        if (auto pf = get_env(var)) candidates.push_back(upath(*pf) / "Steam");
#else
    if (auto home = get_env("HOME")) {
        const fs::path h = upath(*home);
        candidates.push_back(h / ".local/share/Steam");
        candidates.push_back(h / ".steam/steam");
        candidates.push_back(h / ".steam/root");
        candidates.push_back(h / ".var/app/com.valvesoftware.Steam/.local/share/Steam");
#ifdef __APPLE__
        candidates.push_back(h / "Library/Application Support/Steam");
#endif
    }
#endif
    std::vector<fs::path> out, seen;
    for (const auto& p : candidates) {
        if (!is_dir(p / "steamapps")) continue;
        std::error_code ec;
        fs::path key = fs::canonical(p, ec);
        if (ec) key = p;
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        seen.push_back(key);
        out.push_back(p);
    }
    return out;
}

std::vector<fs::path> libraries(const std::vector<fs::path>& roots) {
    std::vector<fs::path> out;
    auto add = [&](const fs::path& p) {
        if (is_dir(p / "steamapps") && std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    };
    for (const auto& root : roots) {
        add(root);
        if (auto text = read_text(root / "steamapps" / "libraryfolders.vdf"))
            for (const auto& p : vdf_values(*text, "path")) add(upath(p));
    }
    return out;
}

std::optional<App> find_app(const std::vector<fs::path>& libraries, const std::string& appid) {
    for (const auto& lib : libraries) {
        auto text = read_text(lib / "steamapps" / upath("appmanifest_" + appid + ".acf"));
        if (!text) continue;
        auto dirs = vdf_values(*text, "installdir");
        if (dirs.empty()) continue;
        fs::path dir = lib / "steamapps" / "common" / upath(dirs.front());
        if (is_dir(dir)) return App{lib, dir};
    }
    return std::nullopt;
}

bool is_game_dir(const fs::path& dir) {
    std::error_code ec;
    return fs::is_regular_file(dir / "win64" / "game.exe", ec) && is_dir(dir / "data");
}

}  // namespace sf::launch::steam
