// File API detours. A path inside the game root is replaced with the profile's copy when
// <profile>\files\<same relative path> exists. Port of crates/sc-agent/src/hooks.rs.
//
// Port status: opening files and attribute queries are done; folder listings (FindFirstFile and
// friends, so profile-only files show up in the engine's scan of data\) are still to do.

#include "agent/hooks.hpp"

#include <windows.h>

#include <mutex>
#include <optional>
#include <unordered_map>

#include "agent/agent.hpp"
#include "agent/hook.hpp"
#include "common/text.hpp"
#include "profile/profile.hpp"

namespace sf::agent::hooks {
namespace {

using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using GetAttrsWFn = DWORD(WINAPI*)(LPCWSTR);
using GetAttrsAFn = DWORD(WINAPI*)(LPCSTR);
using GetAttrsExWFn = BOOL(WINAPI*)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
using GetAttrsExAFn = BOOL(WINAPI*)(LPCSTR, GET_FILEEX_INFO_LEVELS, LPVOID);

// Original (unhooked) entry points; MinHook fills them in before any hook goes live.
CreateFileWFn g_create_w = nullptr;
CreateFileAFn g_create_a = nullptr;
GetAttrsWFn g_attrs_w = nullptr;
GetAttrsAFn g_attrs_a = nullptr;
GetAttrsExWFn g_attrs_ex_w = nullptr;
GetAttrsExAFn g_attrs_ex_a = nullptr;

// Set by install() before the hooks go live, read-only after.
std::wstring g_game_root;
std::wstring g_profile_dir;
bool g_trace = false;

// Lower-cased relative path -> replacement path (nullopt = not in the profile). The game asks
// for the same files many times; each is looked up on disk once.
std::mutex g_cache_lock;
std::unordered_map<std::wstring, std::optional<std::wstring>> g_cache;

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

}  // namespace sf::agent::hooks
