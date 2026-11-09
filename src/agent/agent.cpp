// On load the agent reads the active profile (from SC_PROFILE), applies the pack's patchsets and
// plugins, and hooks the Win32 file API so any game file that also exists under
// <profile>\files\ is opened from the profile instead.
//
// Port status: bootstrap and profile loading are done. Patches, folders and file hooks are
// still to be ported from crates/sc-agent.

#include "agent/agent.hpp"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <string>

#include "common/text.hpp"
#include "profile/profile.hpp"

namespace sf::agent {
namespace {

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
    std::wstring profile_dir = env(profile::kProfileEnv);
    if (profile_dir.empty()) return true;  // Not launched by sc-launch: do nothing.
    profile_dir = profile::normalize(profile_dir);
    g_log = _wfopen((profile_dir + L"\\" + profile::kAgentLog).c_str(), L"wb");

    // game.exe lives in <root>\win64\.
    std::wstring exe = profile::normalize(exe_path());
    std::wstring game_root;
    if (auto a = exe.rfind(L'\\'); a != std::wstring::npos && a > 0)
        if (auto b = exe.rfind(L'\\', a - 1); b != std::wstring::npos) game_root = exe.substr(0, b);

    log("sc-agent %s (C++) loaded into %s", SF_VERSION, utf8(exe).c_str());
    log("game root: %s", utf8(game_root).c_str());
    log("profile:   %s", utf8(profile_dir).c_str());

    profile::Profile prof;
    try {
        prof = profile::load(profile_dir);
    } catch (const profile::Error& e) {
        // A broken profile shouldn't stop the game; it just runs unmodded.
        log("profile error, agent disabled: %s", e.what());
        return true;
    }
    if (prof.trace_files) log("tracing file access");
    if (!prof.name.empty()) log("profile name: %s", prof.name.c_str());
    // TODO(port): patches::apply_all, folders::install, hooks::install.
    return true;
}

}  // namespace sf::agent
