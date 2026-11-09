#include "profile/profile.hpp"

#include <windows.h>

#include <algorithm>

#include <nlohmann/json.hpp>

#include "common/file.hpp"
#include "common/text.hpp"

namespace sf::profile {
namespace {

using nlohmann::json;

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
        if (auto it = pack.find("server"); it != pack.end() && !it->is_null()) {
            if (auto a = get_or<std::string>(*it, "address", ""); !a.empty()) profile.server = a;
            profile.local_server = get_or<bool>(*it, "local", false);
        }
        if (auto it = pack.find("game"); it != pack.end() && !it->is_null())
            profile.args = get_or<std::vector<std::string>>(*it, "args", {});
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
        // "server": "" means "no server" (unlike a missing key, which keeps the pack's).
        if (auto it = local.find("server"); it != local.end() && !it->is_null()) {
            std::string s = it->get<std::string>();
            profile.server = s.empty() ? std::nullopt : std::optional<std::string>(s);
        }
        profile.local_server = get_or<bool>(local, "local_server", profile.local_server);
        for (auto& a : get_or<std::vector<std::string>>(local, "args", {})) profile.args.push_back(a);
    } catch (const std::exception& e) {
        throw Error(std::string("local.json: ") + e.what());
    }
    return profile;
}

std::vector<std::string> game_args(const Profile& profile) {
    std::vector<std::string> out;
    if (profile.local_server) out.push_back("-launchLocalServer");
    out.insert(out.end(), profile.args.begin(), profile.args.end());
    return out;
}

std::optional<std::string> address_cfg(const Profile& profile) {
    if (!profile.server) return std::nullopt;
    const std::string& s = *profile.server;
    bool ok = !s.empty() && s.size() <= 255 && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
               c == ':' || c == '_';
    });
    if (!ok) throw Error("server \"" + s + "\": expected host:port");
    return "set net_masterServerAddress \"" + s + "\"\r\n";
}

std::wstring windows_path_from_arg(const std::wstring& arg) {
    if (arg.empty() || arg[0] != L'/') return arg;
    std::wstring out = L"Z:" + arg;
    std::replace(out.begin(), out.end(), L'/', L'\\');
    return out;
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

std::optional<std::wstring> relative_to(const std::wstring& root_in, const std::wstring& full_in) {
    const std::wstring root = normalize(root_in);
    const std::wstring full = normalize(full_in);
    if (full.size() <= root.size() + 1) return std::nullopt;  // root itself, or shorter
    // Same root, ignoring ASCII case (like Rust's eq_ignore_ascii_case; Windows paths are
    // case-insensitive), followed by a separator: C:\SC2\x isn't inside C:\SC.
    auto lower = [](wchar_t c) { return c >= L'A' && c <= L'Z' ? wchar_t(c - L'A' + L'a') : c; };
    for (size_t i = 0; i < root.size(); ++i)
        if (lower(full[i]) != lower(root[i])) return std::nullopt;
    if (full[root.size()] != L'\\') return std::nullopt;

    std::wstring rel = full.substr(root.size() + 1);
    // Every part must be a plain name.
    size_t start = 0;
    while (true) {
        size_t end = rel.find(L'\\', start);
        std::wstring part = rel.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (part.empty() || part == L"." || part == L".." || part.find(L':') != std::wstring::npos) return std::nullopt;
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return rel;
}

std::vector<std::wstring> patchset_files(const std::wstring& profile_dir) {
    const std::wstring dir = normalize(profile_dir) + L"\\" + kPatchesDir;
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;  // no patches folder: no patchsets
    const std::wstring suffix = L".patchset.toml";
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = fd.cFileName, lower = name;
        for (auto& c : lower)
            if (c >= L'A' && c <= L'Z') c = wchar_t(c - L'A' + L'a');
        if (lower.size() >= suffix.size() && lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0)
            out.push_back(dir + L"\\" + name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

std::optional<std::string> patches_rel(const std::string& path) {
    const std::string prefix = "patches/";
    if (path.rfind(prefix, 0) != 0) return std::nullopt;
    const std::string rel = path.substr(prefix.size());
    if (rel.empty() || rel.find_first_of("\\:") != std::string::npos) return std::nullopt;
    size_t start = 0;
    while (true) {
        size_t end = rel.find('/', start);
        std::string part = rel.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part.empty() || part == "." || part == "..") return std::nullopt;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return rel;
}

std::wstring profile_file(const std::wstring& profile_dir, const std::wstring& rel) {
    return normalize(profile_dir) + L"\\" + kFilesDir + L"\\" + rel;
}

}  // namespace sf::profile
