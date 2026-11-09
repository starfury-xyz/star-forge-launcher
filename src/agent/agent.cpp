// On load the agent reads the active profile (from SC_PROFILE), applies the pack's patchsets and
// plugins, and hooks the Win32 file API so any game file that also exists under
// <profile>\files\ is opened from the profile instead.
//
// Port status: only the bootstrap (env check, log, game root) is here so far. Profile loading,
// patches, folders and file hooks are still to be ported from crates/sc-agent.

#include "agent/agent.hpp"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <string>

namespace sf::agent {
namespace {

// Must match sc_profile::PROFILE_ENV / AGENT_LOG.
constexpr const wchar_t* kProfileEnv = L"SC_PROFILE";
constexpr const wchar_t* kAgentLog = L"agent.log";

FILE* g_log = nullptr;

std::wstring env(const wchar_t* name) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) return {};
    std::wstring v(n, L'\0');
    v.resize(GetEnvironmentVariableW(name, v.data(), n));
    return v;
}

std::wstring exe_path() {
    std::wstring buf(1024, L'\0');
    buf.resize(GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size())));
    return buf;
}

// Same as sc_profile::normalize: drops a \\?\ or \??\ prefix, turns / into \, collapses
// repeated separators and strips trailing ones (keeping a drive root like C:\).
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

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

}  // namespace

void log(const char* fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

bool init() {
    std::wstring profile_dir = env(kProfileEnv);
    if (profile_dir.empty()) return true;  // Not launched by sc-launch: do nothing.
    profile_dir = normalize(profile_dir);
    g_log = _wfopen((profile_dir + L"\\" + kAgentLog).c_str(), L"wb");

    // game.exe lives in <root>\win64\.
    std::wstring exe = normalize(exe_path());
    std::wstring game_root;
    if (auto a = exe.rfind(L'\\'); a != std::wstring::npos && a > 0)
        if (auto b = exe.rfind(L'\\', a - 1); b != std::wstring::npos) game_root = exe.substr(0, b);

    log("sc-agent %s (C++) loaded into %s", SF_VERSION, utf8(exe).c_str());
    log("game root: %s", utf8(game_root).c_str());
    log("profile:   %s", utf8(profile_dir).c_str());
    // TODO(port): Profile::load, patches::apply_all, folders::install, hooks::install.
    return true;
}

}  // namespace sf::agent
