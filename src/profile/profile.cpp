#include "profile/profile.hpp"

#include <windows.h>

#include <nlohmann/json.hpp>

#include "common/text.hpp"

namespace sf::profile {
namespace {

using nlohmann::json;

// Whole file into `out`. Returns 0 or the Win32 error code. Uses the Win32 API rather than
// std::ifstream so a missing file can be told apart from other failures.
DWORD read_file(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    out.clear();
    char buf[64 * 1024];
    DWORD n = 0;
    DWORD err = 0;
    while (true) {
        if (!ReadFile(h, buf, sizeof buf, &n, nullptr)) {
            err = GetLastError();
            break;
        }
        if (n == 0) break;
        out.append(buf, n);
    }
    CloseHandle(h);
    return err;
}

// `j[key]` as T, or `fallback` when the key is missing or null. Throws json::type_error when
// the value has the wrong type (e.g. "trace_files": "yes").
template <class T>
T get_or(const json& j, const char* key, T fallback) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return fallback;
    return it->get<T>();
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

Patches parse_patches(const json& j) {
    Patches p;
    std::string method = get_or<std::string>(j, "method", "inject");
    if (method != "inject") throw Error("patches.method \"" + method + "\": expected \"inject\"");
    std::string on_error = get_or<std::string>(j, "on_error", "exit");
    if (on_error == "exit") {
        p.on_error = OnError::Exit;
    } else if (on_error == "continue") {
        p.on_error = OnError::Continue;
    } else {
        throw Error("patches.on_error \"" + on_error + "\": expected \"exit\" or \"continue\"");
    }
    p.plugins = get_or<std::vector<std::string>>(j, "plugins", {});
    p.core = get_or<std::vector<std::string>>(j, "core", {});
    return p;
}

// Parses `text` as a JSON object, prefixing any error with the file's name (like the Rust
// "pack.json: ..." messages).
json parse_object(const std::string& text, const char* file) {
    try {
        json j = json::parse(text);
        if (!j.is_object()) throw Error(std::string(file) + ": expected a JSON object");
        return j;
    } catch (const json::exception& e) {
        throw Error(std::string(file) + ": " + e.what());
    }
}

}  // namespace

Profile load(const std::wstring& dir) {
    const std::wstring root = normalize(dir);
    Profile profile;

    // pack.json: required.
    const std::wstring pack_path = root + L"\\" + kPackFile;
    std::string text;
    if (DWORD err = read_file(pack_path, text)) throw Error(utf8(pack_path) + ": " + win_error(err));
    const json pack = parse_object(text, "pack.json");
    // Errors from here on (ours and json type errors alike) get the file name in front once.
    try {
        unsigned format = get_or<unsigned>(pack, "format", 0);
        if (format == 0 || format > kPackFormat)
            throw Error("format " + std::to_string(format) + " is not supported (this agent reads format " +
                        std::to_string(kPackFormat) + "); update the launcher");
        profile.name = get_or<std::string>(pack, "name", "");
        if (trim(profile.name).empty()) throw Error("name is empty");
        if (auto it = pack.find("patches"); it != pack.end() && !it->is_null()) profile.patches = parse_patches(*it);
    } catch (const std::exception& e) {
        throw Error(std::string("pack.json: ") + e.what());
    }

    // local.json: optional; missing means "no changes".
    const std::wstring local_path = root + L"\\" + kLocalFile;
    DWORD err = read_file(local_path, text);
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) return profile;
    if (err) throw Error("local.json: " + win_error(err));
    const json local = parse_object(text, "local.json");
    try {
        std::string name = get_or<std::string>(local, "name", "");
        if (!trim(name).empty()) profile.name = name;
        profile.trace_files = get_or<bool>(local, "trace_files", false);
        profile.patch_choices = get_or<decltype(profile.patch_choices)>(local, "patches", {});
        profile.own_game_data = !get_or<bool>(local, "shared_game_data", false);
    } catch (const std::exception& e) {
        throw Error(std::string("local.json: ") + e.what());
    }
    return profile;
}

std::wstring normalize(const std::wstring& path) {
    size_t start = 0;
    if (path.rfind(L"\\\\?\\", 0) == 0 || path.rfind(L"\\??\\", 0) == 0) start = 4;
    std::wstring out;
    out.reserve(path.size() - start);
    for (size_t i = start; i < path.size(); ++i) {
        wchar_t c = path[i] == L'/' ? L'\\' : path[i];
        if (c == L'\\' && !out.empty() && out.back() == L'\\') continue;
        out.push_back(c);
    }
    while (out.size() > 3 && out.back() == L'\\') out.pop_back();
    return out;
}

std::wstring state_local_appdata(const std::wstring& profile_dir) {
    return normalize(profile_dir) + L"\\" + kStateDir + L"\\AppData\\Local";
}

}  // namespace sf::profile
