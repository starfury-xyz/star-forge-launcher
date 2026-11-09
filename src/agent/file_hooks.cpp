// File API detours. A path inside the game root is replaced with the profile's copy when
// <profile>\files\<same relative path> exists.
//
// Port of crates/sc-agent/src/hooks.rs. Named file_hooks here so it isn't confused with
// hook.cpp, the MinHook wrapper it uses.
//
// Folder listings (FindFirstFileA and friends) also include files that only exist in the
// profile: the engine finds loose files by listing data\, so they must show up there to load.

#include "agent/file_hooks.hpp"

#include <windows.h>

#include <algorithm>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "agent/agent.hpp"
#include "agent/hook.hpp"
#include "common/text.hpp"
#include "profile/profile.hpp"

namespace sf::agent::file_hooks {
namespace {

using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using GetAttrsWFn = DWORD(WINAPI*)(LPCWSTR);
using GetAttrsAFn = DWORD(WINAPI*)(LPCSTR);
using GetAttrsExWFn = BOOL(WINAPI*)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
using GetAttrsExAFn = BOOL(WINAPI*)(LPCSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
using FindFirstAFn = HANDLE(WINAPI*)(LPCSTR, LPWIN32_FIND_DATAA);
using FindFirstExAFn = HANDLE(WINAPI*)(LPCSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
using FindNextAFn = BOOL(WINAPI*)(HANDLE, LPWIN32_FIND_DATAA);
using FindCloseFn = BOOL(WINAPI*)(HANDLE);

// Original (unhooked) entry points; MinHook fills them in before any hook goes live.
CreateFileWFn g_create_w = nullptr;
CreateFileAFn g_create_a = nullptr;
GetAttrsWFn g_attrs_w = nullptr;
GetAttrsAFn g_attrs_a = nullptr;
GetAttrsExWFn g_attrs_ex_w = nullptr;
GetAttrsExAFn g_attrs_ex_a = nullptr;
FindFirstAFn g_find_first_a = nullptr;
FindFirstExAFn g_find_first_ex_a = nullptr;
FindNextAFn g_find_next_a = nullptr;
FindCloseFn g_find_close = nullptr;

// Set by install() before the hooks go live, read-only after.
std::wstring g_game_root;
std::wstring g_profile_dir;
bool g_trace = false;

// Lower-cased relative path -> replacement path (nullopt = not in the profile). The game asks
// for the same files many times; each is looked up on disk once.
std::mutex g_cache_lock;
std::unordered_map<std::wstring, std::optional<std::wstring>> g_cache;

// Merged listings handed out under our own handles: the entries FindNextFileA hasn't returned
// yet. The handle is the address of `token`, a heap byte only we own, so it can't equal a handle
// the system gives out.
struct Listing {
    std::unique_ptr<char> token;
    std::deque<WIN32_FIND_DATAA> rest;
};
std::mutex g_listings_lock;
std::unordered_map<HANDLE, Listing> g_listings;

// Set while this thread is inside our own lookup code, so any file call that code makes isn't
// redirected again (a hook calling itself would recurse forever).
thread_local bool t_in_hook = false;

// Runs `f` with the reentrancy guard set; nullopt (pass through) if this thread is already inside.
template <class F>
auto guarded(F f) -> decltype(f()) {
    if (t_in_hook) return std::nullopt;
    t_in_hook = true;
    auto r = f();
    t_in_hook = false;
    return r;
}

std::optional<std::wstring> full_path(const wchar_t* p) {
    std::wstring buf(1024, L'\0');
    DWORD n = GetFullPathNameW(p, static_cast<DWORD>(buf.size()), buf.data(), nullptr);
    if (n == 0 || n >= buf.size()) return std::nullopt;
    buf.resize(n);
    return buf;
}

bool is_file(DWORD attrs) { return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY); }
bool is_dir(DWORD attrs) { return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY); }

std::wstring ascii_lower(std::wstring s) {
    for (auto& c : s)
        if (c >= L'A' && c <= L'Z') c = c - L'A' + L'a';
    return s;
}

// The profile replacement for `path`, if any.
std::optional<std::wstring> redirect(const wchar_t* path) {
    return guarded([&]() -> std::optional<std::wstring> {
        if (!path) return std::nullopt;
        auto full = full_path(path);
        if (!full) return std::nullopt;
        auto rel = profile::relative_to(g_game_root, *full);
        if (!rel) return std::nullopt;  // not a game file

        std::lock_guard<std::mutex> lock(g_cache_lock);
        const std::wstring key = ascii_lower(*rel);
        if (auto it = g_cache.find(key); it != g_cache.end()) return it->second;
        if (g_trace) log("trace %s", utf8(*rel).c_str());
        std::wstring candidate = profile::profile_file(g_profile_dir, *rel);
        std::optional<std::wstring> hit;
        if (is_file(g_attrs_w(candidate.c_str()))) {
            hit = candidate;
            log("redirect %s -> %s", utf8(*rel).c_str(), utf8(candidate).c_str());
        }
        g_cache.emplace(key, hit);
        return hit;
    });
}

// For a folder under the game root that only exists in the profile (say data\strings when the
// profile overrides a packed file inside it): the profile folder, so attribute queries see it
// and the engine walks into it.
std::optional<std::wstring> profile_only_dir(const wchar_t* path) {
    return guarded([&]() -> std::optional<std::wstring> {
        if (!path || g_attrs_w(path) != INVALID_FILE_ATTRIBUTES) return std::nullopt;  // exists in the game
        auto full = full_path(path);
        if (!full) return std::nullopt;
        auto rel = profile::relative_to(g_game_root, *full);
        if (!rel) return std::nullopt;
        std::wstring candidate = profile::profile_file(g_profile_dir, *rel);
        if (!is_dir(g_attrs_w(candidate.c_str()))) return std::nullopt;
        return candidate;
    });
}

// Where an attribute query should look: a redirected file, a profile-only folder, or nowhere else.
std::optional<std::wstring> attrs_target(const wchar_t* path) {
    if (auto r = redirect(path)) return r;
    return profile_only_dir(path);
}

// The detours. Each one asks where the path should really point and calls the original
// function with that; anything not ours goes through unchanged. The A (ANSI) versions convert
// the path and use the W original for redirected paths, since profile paths may not be
// representable in ANSI.

HANDLE WINAPI create_file_w(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp,
                            DWORD flags, HANDLE tmpl) {
    if (auto p = redirect(name)) return g_create_w(p->c_str(), access, share, sa, disp, flags, tmpl);
    return g_create_w(name, access, share, sa, disp, flags, tmpl);
}

HANDLE WINAPI create_file_a(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp,
                            DWORD flags, HANDLE tmpl) {
    if (auto w = from_ansi(name))
        if (auto p = redirect(w->c_str())) return g_create_w(p->c_str(), access, share, sa, disp, flags, tmpl);
    return g_create_a(name, access, share, sa, disp, flags, tmpl);
}

DWORD WINAPI get_attrs_w(LPCWSTR name) {
    if (auto p = attrs_target(name)) return g_attrs_w(p->c_str());
    return g_attrs_w(name);
}

DWORD WINAPI get_attrs_a(LPCSTR name) {
    if (auto w = from_ansi(name))
        if (auto p = attrs_target(w->c_str())) return g_attrs_w(p->c_str());
    return g_attrs_a(name);
}

BOOL WINAPI get_attrs_ex_w(LPCWSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
    if (auto p = attrs_target(name)) return g_attrs_ex_w(p->c_str(), level, info);
    return g_attrs_ex_w(name, level, info);
}

BOOL WINAPI get_attrs_ex_a(LPCSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
    if (auto w = from_ansi(name))
        if (auto p = attrs_target(w->c_str())) return g_attrs_ex_w(p->c_str(), level, info);
    return g_attrs_ex_a(name, level, info);
}

// --- folder listings ---------------------------------------------------------------------------

// Lower-cased entry name, for matching profile entries against the game's.
std::string entry_name(const WIN32_FIND_DATAA& d) {
    std::string n = d.cFileName;
    for (auto& c : n)
        if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    return n;
}

// Drops the entries matching `pred` (C++20 has std::erase_if; this is the C++17 spelling).
template <class Pred>
void remove_where(std::vector<WIN32_FIND_DATAA>& v, Pred pred) {
    v.erase(std::remove_if(v.begin(), v.end(), pred), v.end());
}

// Every entry of a search started with `first` / `data`, via the unhooked functions.
void collect(HANDLE first, const WIN32_FIND_DATAA& data, std::vector<WIN32_FIND_DATAA>& out) {
    if (first == INVALID_HANDLE_VALUE) return;
    WIN32_FIND_DATAA d = data;
    do {
        out.push_back(d);
    } while (g_find_next_a(first, &d));
    g_find_close(first);
}

// For a search pattern (like C:\Game\data\*) under the game root whose folder also exists in the
// profile: the game's entries plus the profile's, the profile winning on a name clash. `real`
// runs the caller's own search. nullopt means "not ours, pass through".
template <class RealSearch>
std::optional<std::vector<WIN32_FIND_DATAA>> merged(LPCSTR pattern, RealSearch real) {
    auto wide_pattern = from_ansi(pattern);
    if (!wide_pattern) return std::nullopt;
    auto full = full_path(wide_pattern->c_str());
    if (!full) return std::nullopt;
    size_t slash = full->rfind(L'\\');
    if (slash == std::wstring::npos) return std::nullopt;
    const std::wstring dir = full->substr(0, slash), name = full->substr(slash + 1);
    auto rel_dir = profile::relative_to(g_game_root, dir);
    if (!rel_dir) return std::nullopt;
    if (g_trace) log("trace list %s\\%s", utf8(*rel_dir).c_str(), utf8(name).c_str());
    const std::wstring prof_dir = profile::profile_file(g_profile_dir, *rel_dir);
    if (!is_dir(g_attrs_w(prof_dir.c_str()))) return std::nullopt;  // profile adds nothing here

    // The profile's entries for the same pattern, without "." and "..".
    auto prof_pattern = ansi(prof_dir + L"\\" + name);
    if (!prof_pattern) return std::nullopt;
    std::vector<WIN32_FIND_DATAA> extra;
    WIN32_FIND_DATAA d = {};
    HANDLE h = g_find_first_a(prof_pattern->c_str(), &d);
    collect(h, d, extra);
    remove_where(extra, [](const WIN32_FIND_DATAA& e) { return entry_name(e) == "." || entry_name(e) == ".."; });

    // The game's entries, minus those the profile replaces, then the profile's.
    std::vector<WIN32_FIND_DATAA> out;
    d = {};
    h = real(&d);
    collect(h, d, out);
    remove_where(out, [&](const WIN32_FIND_DATAA& e) {
        for (const auto& x : extra)
            if (entry_name(x) == entry_name(e)) return true;
        return false;
    });
    for (const auto& e : extra) log("list %s: adding %s", utf8(*rel_dir).c_str(), entry_name(e).c_str());
    out.insert(out.end(), extra.begin(), extra.end());
    return out;
}

// Hands out a merged listing: the first entry now, the rest via FindNextFileA.
HANDLE start_listing(std::vector<WIN32_FIND_DATAA> entries, LPWIN32_FIND_DATAA data) {
    if (entries.empty()) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    *data = entries.front();
    Listing l{std::make_unique<char>(), std::deque<WIN32_FIND_DATAA>(entries.begin() + 1, entries.end())};
    HANDLE handle = l.token.get();
    std::lock_guard<std::mutex> lock(g_listings_lock);
    g_listings.emplace(handle, std::move(l));
    return handle;
}

HANDLE WINAPI find_first_a(LPCSTR name, LPWIN32_FIND_DATAA data) {
    auto entries = guarded([&] { return merged(name, [&](LPWIN32_FIND_DATAA d) { return g_find_first_a(name, d); }); });
    if (entries) return start_listing(std::move(*entries), data);
    return g_find_first_a(name, data);
}

HANDLE WINAPI find_first_ex_a(LPCSTR name, FINDEX_INFO_LEVELS level, LPVOID data, FINDEX_SEARCH_OPS op, LPVOID filter,
                              DWORD flags) {
    auto real = [&](LPWIN32_FIND_DATAA d) { return g_find_first_ex_a(name, level, d, op, filter, flags); };
    auto entries = guarded([&] { return merged(name, real); });
    if (entries) return start_listing(std::move(*entries), static_cast<LPWIN32_FIND_DATAA>(data));
    return g_find_first_ex_a(name, level, data, op, filter, flags);
}

BOOL WINAPI find_next_a(HANDLE h, LPWIN32_FIND_DATAA data) {
    {
        std::lock_guard<std::mutex> lock(g_listings_lock);
        if (auto it = g_listings.find(h); it != g_listings.end()) {
            if (it->second.rest.empty()) {
                SetLastError(ERROR_NO_MORE_FILES);
                return FALSE;
            }
            *data = it->second.rest.front();
            it->second.rest.pop_front();
            return TRUE;
        }
    }
    return g_find_next_a(h, data);  // not one of ours
}

BOOL WINAPI find_close(HANDLE h) {
    {
        std::lock_guard<std::mutex> lock(g_listings_lock);
        if (g_listings.erase(h)) return TRUE;
    }
    return g_find_close(h);
}

// Finds an export in kernelbase (where the real code lives on Windows 8+ and Wine), falling
// back to kernel32. Hooking kernelbase catches callers of either DLL.
void* find_export(const char* name) {
    for (const wchar_t* dll : {L"kernelbase.dll", L"kernel32.dll"}) {
        if (HMODULE m = GetModuleHandleW(dll))
            if (FARPROC f = GetProcAddress(m, name)) return reinterpret_cast<void*>(f);
    }
    return nullptr;
}

}  // namespace

void install(const std::wstring& game_root, const std::wstring& profile_dir, bool trace) {
    g_game_root = game_root;
    g_profile_dir = profile_dir;
    g_trace = trace;

    struct Hook {
        const char* name;
        void* detour;
        void** original;
    };
    const Hook hooks[] = {
        {"CreateFileW", reinterpret_cast<void*>(create_file_w), reinterpret_cast<void**>(&g_create_w)},
        {"CreateFileA", reinterpret_cast<void*>(create_file_a), reinterpret_cast<void**>(&g_create_a)},
        {"GetFileAttributesW", reinterpret_cast<void*>(get_attrs_w), reinterpret_cast<void**>(&g_attrs_w)},
        {"GetFileAttributesA", reinterpret_cast<void*>(get_attrs_a), reinterpret_cast<void**>(&g_attrs_a)},
        {"GetFileAttributesExW", reinterpret_cast<void*>(get_attrs_ex_w), reinterpret_cast<void**>(&g_attrs_ex_w)},
        {"GetFileAttributesExA", reinterpret_cast<void*>(get_attrs_ex_a), reinterpret_cast<void**>(&g_attrs_ex_a)},
        {"FindFirstFileA", reinterpret_cast<void*>(find_first_a), reinterpret_cast<void**>(&g_find_first_a)},
        {"FindFirstFileExA", reinterpret_cast<void*>(find_first_ex_a), reinterpret_cast<void**>(&g_find_first_ex_a)},
        {"FindNextFileA", reinterpret_cast<void*>(find_next_a), reinterpret_cast<void**>(&g_find_next_a)},
        {"FindClose", reinterpret_cast<void*>(find_close), reinterpret_cast<void**>(&g_find_close)},
    };
    // Create every hook first, then switch them on together: all or nothing.
    for (const Hook& h : hooks) {
        void* target = find_export(h.name);
        if (!target) {
            log("hook %s: export not found, file redirects disabled", h.name);
            return;
        }
        if (std::string err = hook::create(target, h.detour, h.original); !err.empty()) {
            log("hook %s: %s, file redirects disabled", h.name, err.c_str());
            return;
        }
    }
    if (std::string err = hook::enable_all(); !err.empty()) {
        log("enabling hooks failed: %s", err.c_str());
        return;
    }
    log("file hooks installed");
}

}  // namespace sf::agent::file_hooks
