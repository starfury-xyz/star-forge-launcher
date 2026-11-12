// Gives the game the profile's own %LOCALAPPDATA% (<profile>\state\AppData\Local). Port of
// crates/sc-agent/src/folders.rs.
//
// The game asks SHGetFolderPathA(CSIDL_LOCAL_APPDATA) once at startup and keeps everything
// per-user under Targem\StarConflict there: the store cache, player notes, logs, and the local
// server's MongoDB and Hub folders (passed on to those processes). Answering with a profile
// folder keeps one server's data out of another's sessions. The launcher copies the shared
// settings files (graphics settings, key bindings) in and out.

#include "agent/folders.hpp"

#include <windows.h>

#include <cstring>
#include <filesystem>
#include <optional>
#include <system_error>

#include "agent/agent.hpp"
#include "hook/hook.hpp"
#include "common/text.hpp"

namespace sf::agent::folders {
namespace {

constexpr int kCsidlLocalAppData = 0x1c;  // CSIDL_LOCAL_APPDATA
constexpr int kCsidlMask = 0xff;          // CSIDL values carry flags (CSIDL_FLAG_CREATE, ...) above the low byte

using GetFolderA = HRESULT(WINAPI*)(HWND, int, HANDLE, DWORD, LPSTR);
using GetFolderW = HRESULT(WINAPI*)(HWND, int, HANDLE, DWORD, LPWSTR);

// The answer, in both encodings. Set before the hooks go live and never changed after.
std::string g_ansi;
std::wstring g_wide;
GetFolderA g_orig_a = nullptr;
GetFolderW g_orig_w = nullptr;

// Our SHGetFolderPathA/W. The caller's buffer is MAX_PATH chars; install() checked the path
// fits. Every other folder goes to the real function.
HRESULT WINAPI get_folder_a(HWND hwnd, int csidl, HANDLE token, DWORD flags, LPSTR path) {
    if ((csidl & kCsidlMask) == kCsidlLocalAppData && path) {
        std::memcpy(path, g_ansi.c_str(), g_ansi.size() + 1);
        return S_OK;
    }
    return g_orig_a(hwnd, csidl, token, flags, path);
}

HRESULT WINAPI get_folder_w(HWND hwnd, int csidl, HANDLE token, DWORD flags, LPWSTR path) {
    if ((csidl & kCsidlMask) == kCsidlLocalAppData && path) {
        std::memcpy(path, g_wide.c_str(), (g_wide.size() + 1) * sizeof(wchar_t));
        return S_OK;
    }
    return g_orig_w(hwnd, csidl, token, flags, path);
}

}  // namespace

void install(const std::wstring& dir) {
    const std::string shown = utf8(dir);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        log("game data folder %s: %s; using the shared one", shown.c_str(), ec.message().c_str());
        return;
    }
    // The game calls the A (ANSI) version, which can only hold characters of the system code page.
    std::optional<std::string> ansi = sf::ansi(dir);
    if (!ansi) {
        log("game data folder %s: not representable in the system code page; using the shared one", shown.c_str());
        return;
    }
    if (ansi->size() + 1 > MAX_PATH || dir.size() + 1 > MAX_PATH) {
        log("game data folder %s: path too long for the game; using the shared one", shown.c_str());
        return;
    }
    g_ansi = *ansi;
    g_wide = dir;

    HMODULE shell32 = LoadLibraryW(L"shell32.dll");
    if (!shell32) {
        log("shell32.dll not found; using the shared game data folder");
        return;
    }
    auto real_a = reinterpret_cast<GetFolderA>(GetProcAddress(shell32, "SHGetFolderPathA"));
    auto real_w = reinterpret_cast<GetFolderW>(GetProcAddress(shell32, "SHGetFolderPathW"));
    if (!real_a || !real_w) {
        log("SHGetFolderPath not found; using the shared game data folder");
        return;
    }
    if (std::string err = hook::install(real_a, get_folder_a, &g_orig_a); !err.empty()) {
        log("hook SHGetFolderPathA: %s; using the shared game data folder", err.c_str());
        return;
    }
    if (std::string err = hook::install(real_w, get_folder_w, &g_orig_w); !err.empty()) {
        log("hook SHGetFolderPathW: %s", err.c_str());
        return;
    }
    log("game data folder: %s", shown.c_str());
}

}  // namespace sf::agent::folders
