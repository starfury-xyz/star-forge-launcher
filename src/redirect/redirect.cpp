// File API detours. A path inside the game root is served from the first folder (the writes
// folder, then each layer) that has the same relative path.
//
// Port of crates/sc-agent/src/hooks.rs, generalized from one profile folder to a list of them so
// the server proxy can stack its writes folder over the pack's overlay.
//
// Folder listings (FindFirstFileA and friends) also include files that only exist in a layer:
// the engine finds loose files by listing data\, so they must show up there to load.
//
// With a writes folder, writing a game file never changes it: the file is copied there first
// (copy on write) and the copy is what the process opens, reads back and lists from then on.
// Creating, deleting, moving and copying files under the game root go there too.

#include "redirect/redirect.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

#include "common/text.hpp"
#include "hook/hook.hpp"
#include "profile/profile.hpp"

namespace sf::redirect {
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
using CreateDirAFn = BOOL(WINAPI*)(LPCSTR, LPSECURITY_ATTRIBUTES);
using DeleteFileAFn = BOOL(WINAPI*)(LPCSTR);
using MoveFileAFn = BOOL(WINAPI*)(LPCSTR, LPCSTR);
using MoveFileExAFn = BOOL(WINAPI*)(LPCSTR, LPCSTR, DWORD);
using CopyFileAFn = BOOL(WINAPI*)(LPCSTR, LPCSTR, BOOL);

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
CreateDirAFn g_create_dir_a = nullptr;
DeleteFileAFn g_delete_a = nullptr;
MoveFileAFn g_move_a = nullptr;
MoveFileExAFn g_move_ex_a = nullptr;
CopyFileAFn g_copy_a = nullptr;

// Set by install() before the hooks go live, read-only after.
Config g_cfg;
std::vector<std::wstring> g_dirs;  // where to look, in order: the writes folder (if any), then the layers

void log(const char* fmt, ...) {
    if (!g_cfg.log) return;
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    g_cfg.log(line);
}

// Lower-cased relative path -> replacement path (nullopt = in no layer). The game asks for the
// same files many times; each is looked up on disk once. Writes update it.
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

// `path` relative to the game root, if it's a game path we redirect (not in a skipped folder).
std::optional<std::wstring> game_relative(const wchar_t* path) {
    if (!path) return std::nullopt;
    auto full = full_path(path);
    if (!full) return std::nullopt;
    auto rel = profile::relative_to(g_cfg.game_root, *full);
    if (!rel) return std::nullopt;
    const std::wstring lower = ascii_lower(*rel);
    for (const auto& s : g_cfg.skip) {
        const std::wstring k = ascii_lower(s);
        if (lower == k || (lower.size() > k.size() && lower.compare(0, k.size(), k) == 0 && lower[k.size()] == L'\\'))
            return std::nullopt;
    }
    return rel;
}

std::wstring in_dir(const std::wstring& dir, const std::wstring& rel) { return profile::normalize(dir) + L"\\" + rel; }
std::wstring game_path(const std::wstring& rel) { return in_dir(g_cfg.game_root, rel); }

// Where `rel` lives in the layers (writes folder first), looking at the disk. Call with the
// guard set.
std::optional<std::wstring> find_in_layers(const std::wstring& rel) {
    for (const auto& d : g_dirs) {
        std::wstring candidate = in_dir(d, rel);
        if (is_file(g_attrs_w(candidate.c_str()))) return candidate;
    }
    return std::nullopt;
}

// The replacement for `path`, if any.
std::optional<std::wstring> redirect(const wchar_t* path) {
    return guarded([&]() -> std::optional<std::wstring> {
        auto rel = game_relative(path);
        if (!rel) return std::nullopt;  // not a game file

        std::lock_guard<std::mutex> lock(g_cache_lock);
        const std::wstring key = ascii_lower(*rel);
        if (auto it = g_cache.find(key); it != g_cache.end()) return it->second;
        if (g_cfg.trace) log("trace %s", utf8(*rel).c_str());
        std::optional<std::wstring> hit = find_in_layers(*rel);
        if (hit) log("redirect %s -> %s", utf8(*rel).c_str(), utf8(*hit).c_str());
        g_cache.emplace(key, hit);
        return hit;
    });
}

// For a folder under the game root that only exists in a layer (say data\strings when the
// profile overrides a packed file inside it): that layer's folder, so attribute queries see it
// and the engine walks into it.
std::optional<std::wstring> layer_only_dir(const wchar_t* path) {
    return guarded([&]() -> std::optional<std::wstring> {
        if (!path || g_attrs_w(path) != INVALID_FILE_ATTRIBUTES) return std::nullopt;  // exists in the game
        auto rel = game_relative(path);
        if (!rel) return std::nullopt;
        for (const auto& d : g_dirs) {
            std::wstring candidate = in_dir(d, *rel);
            if (is_dir(g_attrs_w(candidate.c_str()))) return candidate;
        }
        return std::nullopt;
    });
}

// Where an attribute query should look: a redirected file, a layer-only folder, or nowhere else.
std::optional<std::wstring> attrs_target(const wchar_t* path) {
    if (auto r = redirect(path)) return r;
    return layer_only_dir(path);
}

// --- writes (only with a writes folder) ----------------------------------------------------------

bool writes_on() { return !g_cfg.writes.empty(); }

// Points the cache at the writes copy (or forgets the file) after we changed it.
void remember(const std::wstring& rel, std::optional<std::wstring> where) {
    std::lock_guard<std::mutex> lock(g_cache_lock);
    g_cache[ascii_lower(rel)] = std::move(where);
}

bool make_parent(const std::wstring& path) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    return !ec;
}

// The file as the process sees it now: the writes copy, a layer's, or the game's. Guard set.
std::optional<std::wstring> current(const std::wstring& rel) {
    if (auto hit = find_in_layers(rel)) return hit;
    std::wstring game = game_path(rel);
    if (is_file(g_attrs_w(game.c_str()))) return game;
    return std::nullopt;
}

// Whether a CreateFile call may change the file (or make one).
bool is_write(DWORD access, DWORD disp, bool exists) {
    if (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE)) return true;
    if (disp == CREATE_ALWAYS || disp == CREATE_NEW || disp == TRUNCATE_EXISTING) return true;
    return disp == OPEN_ALWAYS && !exists;
}

// For a CreateFile that writes a game file: the writes copy to open instead, made first
// (copied from what the process sees now, unless the call throws the contents away).
std::optional<std::wstring> write_target(const wchar_t* path, DWORD access, DWORD disp) {
    return guarded([&]() -> std::optional<std::wstring> {
        auto rel = game_relative(path);
        if (!rel) return std::nullopt;
        const std::wstring target = in_dir(g_cfg.writes, *rel);
        auto now = current(*rel);
        if (!is_write(access, disp, now.has_value())) return std::nullopt;  // a plain read
        if (now && *now == target) return target;                         // already ours
        if (is_dir(g_attrs_w(game_path(*rel).c_str()))) return std::nullopt;  // a folder: not ours to copy
        if (!make_parent(target)) {
            log("write %s: can't make %s", utf8(*rel).c_str(), utf8(target).c_str());
            return std::nullopt;
        }
        if (now && disp == CREATE_NEW) return *now;  // CREATE_NEW on an existing file fails, as it should
        const bool keep = disp == OPEN_EXISTING || disp == OPEN_ALWAYS || disp == TRUNCATE_EXISTING;
        if (now && keep && !CopyFileW(now->c_str(), target.c_str(), TRUE)) {
            log("write %s: copying %s failed: %s", utf8(*rel).c_str(), utf8(*now).c_str(), win_error(GetLastError()).c_str());
            return std::nullopt;
        }
        // TRUNCATE_EXISTING needs the file to be there: the copy above made it.
        log("write %s -> %s", utf8(*rel).c_str(), utf8(target).c_str());
        remember(*rel, target);
        return target;
    });
}

// The detours. Each one asks where the path should really point and calls the original
// function with that; anything not ours goes through unchanged. The A (ANSI) versions convert
// the path and use the W original for redirected paths, since layer paths may not be
// representable in ANSI.

// Where a CreateFile call should really open, or nullopt to open `name` itself.
std::optional<std::wstring> create_target(LPCWSTR name, DWORD access, DWORD disp) {
    if (writes_on())
        if (auto p = write_target(name, access, disp)) return p;
    return redirect(name);
}

HANDLE WINAPI create_file_w(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp,
                            DWORD flags, HANDLE tmpl) {
    if (auto p = create_target(name, access, disp)) return g_create_w(p->c_str(), access, share, sa, disp, flags, tmpl);
    return g_create_w(name, access, share, sa, disp, flags, tmpl);
}

HANDLE WINAPI create_file_a(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp,
                            DWORD flags, HANDLE tmpl) {
    if (auto w = from_ansi(name))
        if (auto p = create_target(w->c_str(), access, disp)) return g_create_w(p->c_str(), access, share, sa, disp, flags, tmpl);
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

// A folder under the game root that's missing everywhere is made in the writes folder.
BOOL WINAPI create_dir_a(LPCSTR name, LPSECURITY_ATTRIBUTES sa) {
    auto done = guarded([&]() -> std::optional<BOOL> {
        auto w = from_ansi(name);
        if (!w) return std::nullopt;
        auto rel = game_relative(w->c_str());
        if (!rel) return std::nullopt;
        if (g_attrs_w(game_path(*rel).c_str()) != INVALID_FILE_ATTRIBUTES) return std::nullopt;  // the real call fails as it should
        for (const auto& d : g_dirs)
            if (g_attrs_w(in_dir(d, *rel).c_str()) != INVALID_FILE_ATTRIBUTES) {
                SetLastError(ERROR_ALREADY_EXISTS);
                return FALSE;
            }
        const std::wstring target = in_dir(g_cfg.writes, *rel);
        if (!make_parent(target)) return std::nullopt;
        log("mkdir %s -> %s", utf8(*rel).c_str(), utf8(target).c_str());
        return CreateDirectoryW(target.c_str(), sa);
    });
    return done ? *done : g_create_dir_a(name, sa);
}

// Only the writes copy can be deleted; the game's and the overlay's files stay.
BOOL WINAPI delete_a(LPCSTR name) {
    auto done = guarded([&]() -> std::optional<BOOL> {
        auto w = from_ansi(name);
        if (!w) return std::nullopt;
        auto rel = game_relative(w->c_str());
        if (!rel) return std::nullopt;
        const std::wstring mine = in_dir(g_cfg.writes, *rel);
        if (is_file(g_attrs_w(mine.c_str()))) {
            BOOL ok = DeleteFileW(mine.c_str());
            if (ok) remember(*rel, find_in_layers(*rel));
            log("delete %s: %s", utf8(*rel).c_str(), ok ? "removed the writes copy" : "failed");
            return ok;
        }
        if (!current(*rel)) return std::nullopt;  // missing everywhere: the real call says so
        log("delete %s: refused, it's a game or overlay file", utf8(*rel).c_str());
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    });
    return done ? *done : g_delete_a(name);
}

// Moving or copying to a game path writes the writes copy. Moving a game or overlay file copies
// it instead: the original stays where it is.
std::optional<BOOL> move_or_copy(LPCSTR from, LPCSTR to, bool move, bool replace) {
    return guarded([&]() -> std::optional<BOOL> {
        auto wf = from_ansi(from), wt = from_ansi(to);
        if (!wf || !wt) return std::nullopt;
        auto rel_from = game_relative(wf->c_str()), rel_to = game_relative(wt->c_str());
        if (!rel_from && !rel_to) return std::nullopt;
        auto full_from = full_path(wf->c_str()), full_to = full_path(wt->c_str());
        if (!full_from || !full_to) return std::nullopt;
        std::wstring src = *full_from, dst = *full_to;
        if (rel_from) {
            auto now = current(*rel_from);
            if (!now) return std::nullopt;  // missing: the real call fails as it should
            src = *now;
        }
        if (rel_to) {
            dst = in_dir(g_cfg.writes, *rel_to);
            if (!make_parent(dst)) return std::nullopt;
        }
        const bool own = rel_from && src == in_dir(g_cfg.writes, *rel_from);
        BOOL ok = move && (own || !rel_from)
                      ? MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_COPY_ALLOWED | (replace ? MOVEFILE_REPLACE_EXISTING : 0))
                      : CopyFileW(src.c_str(), dst.c_str(), !replace);
        if (ok && rel_to) remember(*rel_to, dst);
        if (ok && move && own) remember(*rel_from, find_in_layers(*rel_from));
        log("%s %s -> %s%s", move ? "move" : "copy", utf8(src).c_str(), utf8(dst).c_str(),
            !ok ? ": failed" : move && rel_from && !own ? " (copied; the original stays)" : "");
        return ok;
    });
}

BOOL WINAPI move_a(LPCSTR from, LPCSTR to) {
    auto done = move_or_copy(from, to, true, false);
    return done ? *done : g_move_a(from, to);
}

BOOL WINAPI move_ex_a(LPCSTR from, LPCSTR to, DWORD flags) {
    auto done = to ? move_or_copy(from, to, true, (flags & MOVEFILE_REPLACE_EXISTING) != 0) : std::nullopt;
    return done ? *done : g_move_ex_a(from, to, flags);
}

BOOL WINAPI copy_a(LPCSTR from, LPCSTR to, BOOL fail_if_exists) {
    auto done = move_or_copy(from, to, false, !fail_if_exists);
    return done ? *done : g_copy_a(from, to, fail_if_exists);
}

// --- folder listings ---------------------------------------------------------------------------

// Lower-cased entry name, for matching layer entries against the game's.
std::string entry_name(const WIN32_FIND_DATAA& d) {
    std::string n = d.cFileName;
    for (auto& c : n)
        if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    return n;
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

// For a search pattern (like C:\Game\data\*) under the game root whose folder also exists in a
// layer: the game's entries plus the layers', the first layer winning on a name clash. `real`
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
    auto rel_dir = game_relative(dir.c_str());
    if (!rel_dir) return std::nullopt;
    if (g_cfg.trace) log("trace list %s\\%s", utf8(*rel_dir).c_str(), utf8(name).c_str());

    // Each layer's entries for the same pattern, without "." and ".."; earlier layers win.
    std::vector<WIN32_FIND_DATAA> extra;
    std::set<std::string> seen = {".", ".."};
    bool any = false;
    for (const auto& d : g_dirs) {
        const std::wstring layer_dir = in_dir(d, *rel_dir);
        if (!is_dir(g_attrs_w(layer_dir.c_str()))) continue;
        any = true;
        auto layer_pattern = ansi(layer_dir + L"\\" + name);
        if (!layer_pattern) return std::nullopt;
        std::vector<WIN32_FIND_DATAA> found;
        WIN32_FIND_DATAA fd = {};
        HANDLE h = g_find_first_a(layer_pattern->c_str(), &fd);
        collect(h, fd, found);
        for (const auto& e : found)
            if (seen.insert(entry_name(e)).second) extra.push_back(e);
    }
    if (!any) return std::nullopt;  // no layer adds anything here

    // The game's entries, minus those a layer replaces, then the layers'.
    std::vector<WIN32_FIND_DATAA> out;
    WIN32_FIND_DATAA fd = {};
    HANDLE h = real(&fd);
    collect(h, fd, out);
    out.erase(std::remove_if(out.begin(), out.end(),
                             [&](const WIN32_FIND_DATAA& e) {
                                 const std::string n = entry_name(e);
                                 return n != "." && n != ".." && seen.count(n);
                             }),
              out.end());
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

std::string install(Config config) {
    g_cfg = std::move(config);
    g_cfg.game_root = profile::normalize(g_cfg.game_root);
    g_dirs.clear();
    if (writes_on()) g_dirs.push_back(g_cfg.writes);
    g_dirs.insert(g_dirs.end(), g_cfg.layers.begin(), g_cfg.layers.end());

    struct Hook {
        const char* name;
        void* detour;
        void** original;
    };
    std::vector<Hook> hooks = {
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
    // The server exes only use the ANSI versions of these (checked against their imports).
    if (writes_on()) {
        hooks.push_back({"CreateDirectoryA", reinterpret_cast<void*>(create_dir_a), reinterpret_cast<void**>(&g_create_dir_a)});
        hooks.push_back({"DeleteFileA", reinterpret_cast<void*>(delete_a), reinterpret_cast<void**>(&g_delete_a)});
        hooks.push_back({"MoveFileA", reinterpret_cast<void*>(move_a), reinterpret_cast<void**>(&g_move_a)});
        hooks.push_back({"MoveFileExA", reinterpret_cast<void*>(move_ex_a), reinterpret_cast<void**>(&g_move_ex_a)});
        hooks.push_back({"CopyFileA", reinterpret_cast<void*>(copy_a), reinterpret_cast<void**>(&g_copy_a)});
    }
    // Create every hook first, then switch them on together: all or nothing.
    for (const Hook& h : hooks) {
        void* target = find_export(h.name);
        if (!target) return std::string("hook ") + h.name + ": export not found";
        if (std::string err = hook::create(target, h.detour, h.original); !err.empty())
            return std::string("hook ") + h.name + ": " + err;
    }
    if (std::string err = hook::enable_all(); !err.empty()) return "enabling hooks failed: " + err;
    log("file hooks installed");
    return {};
}

}  // namespace sf::redirect
