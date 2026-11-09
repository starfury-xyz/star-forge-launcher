// Loads sc_agent.dll into this process the way sc-inject loads it into game.exe, then checks
// what the agent did. Built into <build>/fakegame/win64/ so the agent sees <build>/fakegame as
// the game root, like <install>\win64\game.exe. Runs under wine via ctest.

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static int g_failures = 0;

static void check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

static std::wstring dir_of(const std::wstring& p) { return p.substr(0, p.rfind(L'\\')); }

static std::string read_file(const std::wstring& path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

int main() {
    std::wstring exe(1024, L'\0');
    exe.resize(GetModuleFileNameW(nullptr, exe.data(), (DWORD)exe.size()));
    const std::wstring win64 = dir_of(exe);
    const std::wstring game_root = dir_of(win64);
    const std::wstring dll = win64 + L"\\sc_agent.dll";
    const std::wstring profile = game_root + L"\\profile";
    const std::wstring log = profile + L"\\agent.log";

    // 1. Not started by the launcher (no SC_PROFILE): the agent loads and does nothing.
    SetEnvironmentVariableW(L"SC_PROFILE", nullptr);
    DeleteFileW(log.c_str());
    HMODULE m = LoadLibraryW(dll.c_str());
    check(m != nullptr, "loads without SC_PROFILE");
    check(GetFileAttributesW(log.c_str()) == INVALID_FILE_ATTRIBUTES, "no agent.log without SC_PROFILE");
    if (m) FreeLibrary(m);

    // 2. Started by the launcher: the agent logs where it is. A trailing slash and forward
    //    slashes in SC_PROFILE must be normalized away.
    CreateDirectoryW(profile.c_str(), nullptr);
    std::wstring env = profile;
    for (auto& c : env)
        if (c == L'\\') c = L'/';
    env += L"/";
    SetEnvironmentVariableW(L"SC_PROFILE", env.c_str());
    m = LoadLibraryW(dll.c_str());
    check(m != nullptr, "loads with SC_PROFILE");
    const std::string text = read_file(log);
    check(text.find("sc-agent ") != std::string::npos, "agent.log has the version line");
    check(text.find("game root: " + utf8(game_root) + "\n") != std::string::npos, "agent.log has the game root");
    check(text.find("profile:   " + utf8(profile) + "\n") != std::string::npos, "agent.log has the normalized profile");
    if (m) FreeLibrary(m);

    if (g_failures) std::printf("--- agent.log ---\n%s", text.c_str());
    return g_failures ? 1 : 0;
}
