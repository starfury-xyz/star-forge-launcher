// Loads sc_agent.dll into this process the way sc-inject loads it into game.exe, then checks
// what the agent did. Built into <build>/fakegame/win64/ so the agent sees <build>/fakegame as
// the game root, like <install>\win64\game.exe. Runs under wine via ctest.
//
// Usage: load_test <scenario>. ctest runs each scenario in its own process: once the agent has
// hooked something it must never be unloaded (a hook pointing into an unloaded DLL crashes the
// next caller), and game.exe never unloads it either. Each scenario gets its own profile in
// <build>/profiles/<scenario>, outside the game folder like a real profile.

#include "common/text.hpp"
#include "test_util.hpp"

#include <shlobj.h>

#include <cstring>
#include <filesystem>

using sf::utf8;
using test::check;
using test::contains;

namespace {

std::wstring g_win64, g_game_root, g_dll, g_profile, g_log;

const char* kPack = R"({ "format": 1, "id": "org.example.test", "name": "Test Pack", "version": "1.0.0" })";

// Points SC_PROFILE at this scenario's profile (with forward slashes and a trailing slash, which
// the agent must normalize) and loads the agent. Returns agent.log.
std::string load_agent(bool loaded_expected = true) {
    std::wstring env = g_profile;
    for (auto& c : env)
        if (c == L'\\') c = L'/';
    SetEnvironmentVariableW(L"SC_PROFILE", (env + L"/").c_str());
    HMODULE m = LoadLibraryW(g_dll.c_str());
    check((m != nullptr) == loaded_expected, "agent loads");
    return test::read_file(g_log);
}

// What SHGetFolderPath answers right now (the game asks this for its data folder).
std::wstring folder_w(int csidl) {
    wchar_t buf[MAX_PATH] = {};
    return SHGetFolderPathW(nullptr, csidl, nullptr, 0, buf) == S_OK ? buf : L"(failed)";
}
std::string folder_a(int csidl) {
    char buf[MAX_PATH] = {};
    return SHGetFolderPathA(nullptr, csidl, nullptr, 0, buf) == S_OK ? buf : "(failed)";
}

// A file's contents read through CreateFileW / CreateFileA, as the game would; "(missing)" if
// it can't be opened.
std::string read_from(HANDLE h) {
    if (h == INVALID_HANDLE_VALUE) return "(missing)";
    char buf[256];
    DWORD n = 0;
    ReadFile(h, buf, sizeof buf, &n, nullptr);
    CloseHandle(h);
    return std::string(buf, n);
}
std::string open_w(const std::wstring& path) {
    return read_from(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
}
std::string open_a(const std::string& path) {
    return read_from(CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
}

// --- scenarios -------------------------------------------------------------------------------

// Not started by the launcher (no SC_PROFILE): the agent loads and does nothing.
void no_profile() {
    SetEnvironmentVariableW(L"SC_PROFILE", nullptr);
    check(LoadLibraryW(g_dll.c_str()) != nullptr, "loads without SC_PROFILE");
    check(GetFileAttributesW(g_log.c_str()) == INVALID_FILE_ATTRIBUTES, "no agent.log without SC_PROFILE");
}

// Started by the launcher, but the profile has no pack.json: the agent logs where it is,
// reports the profile error and still lets the game start, unmodded.
std::string broken_profile() {
    std::string text = load_agent();
    check(contains(text, "sc-agent "), "agent.log has the version line");
    check(contains(text, "game root: " + utf8(g_game_root) + "\n"), "agent.log has the game root");
    check(contains(text, "profile:   " + utf8(g_profile) + "\n"), "agent.log has the normalized profile");
    check(contains(text, "profile error, agent disabled: "), "agent.log reports the missing pack.json");
    check(!contains(text, "file hooks installed"), "broken profile: nothing hooked");
    return text;
}

// A valid profile that keeps the shared game data (local.json shared_game_data): the agent
// reads pack.json + local.json and leaves %LOCALAPPDATA% alone.
std::string shared_data() {
    test::write_file(g_profile + L"\\pack.json", kPack);
    test::write_file(g_profile + L"\\local.json", R"({ "name": "My Server", "trace_files": true, "shared_game_data": true })");
    const std::wstring real_local = folder_w(CSIDL_LOCAL_APPDATA);
    std::string text = load_agent();
    check(!contains(text, "profile error"), "no profile error");
    check(contains(text, "profile name: My Server\n"), "agent.log has the local.json name");
    check(contains(text, "tracing file access\n"), "agent.log notes trace_files");
    check(!contains(text, "game data folder"), "shared game data: no folder redirect");
    check(folder_w(CSIDL_LOCAL_APPDATA) == real_local, "shared game data: %LOCALAPPDATA% unchanged");
    check(GetFileAttributesW((g_profile + L"\\state").c_str()) == INVALID_FILE_ATTRIBUTES, "shared game data: no state folder");
    return text;
}

// The default, own game data: %LOCALAPPDATA% becomes <profile>\state\AppData\Local, for the A
// and W functions and with flags like CSIDL_FLAG_CREATE; other folders don't change.
std::string own_data() {
    test::write_file(g_profile + L"\\pack.json", kPack);
    const std::wstring state = g_profile + L"\\state\\AppData\\Local";
    const std::wstring real_roaming = folder_w(CSIDL_APPDATA);
    std::string text = load_agent();
    check(contains(text, "game data folder: " + utf8(state) + "\n"), "agent.log has the game data folder");
    DWORD attrs = GetFileAttributesW(state.c_str());
    check(attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY), "state folder created");
    check(folder_w(CSIDL_LOCAL_APPDATA) == state, "SHGetFolderPathW(LOCAL_APPDATA) -> profile");
    check(folder_w(CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE) == state, "... also with CSIDL_FLAG_CREATE");
    check(folder_a(CSIDL_LOCAL_APPDATA) == utf8(state), "SHGetFolderPathA(LOCAL_APPDATA) -> profile");
    check(folder_w(CSIDL_APPDATA) == real_roaming, "other folders (APPDATA) unchanged");
    return text;
}

// File redirects: a game file that also exists under <profile>\files\ is opened from there.
std::string redirect() {
    test::write_file(g_profile + L"\\pack.json", kPack);
    // The fake game's files, and the profile's overrides (files\ mirrors the game root).
    const std::wstring game_data = g_game_root + L"\\data\\redirect_test";
    const std::wstring prof_data = g_profile + L"\\files\\data\\redirect_test";
    std::filesystem::remove_all(game_data);
    std::filesystem::create_directories(game_data);
    std::filesystem::create_directories(prof_data);
    std::filesystem::create_directories(g_profile + L"\\files\\data\\redirect_test_new\\sub");
    test::write_file(game_data + L"\\both.txt", "from game");
    test::write_file(prof_data + L"\\both.txt", "from profile");
    test::write_file(game_data + L"\\game_only.txt", "game only");
    test::write_file(prof_data + L"\\profile_only.txt", "profile only");
    // Same relative path, but outside the game root: never redirected.
    const std::wstring elsewhere = test::dir_of(g_game_root) + L"\\elsewhere\\data\\redirect_test";
    std::filesystem::create_directories(elsewhere);
    test::write_file(elsewhere + L"\\both.txt", "elsewhere");

    std::string text = load_agent();
    check(contains(text, "file hooks installed\n"), "agent.log: file hooks installed");

    check(open_w(game_data + L"\\both.txt") == "from profile", "CreateFileW: profile copy wins");
    check(open_a(utf8(game_data) + "\\both.txt") == "from profile", "CreateFileA: profile copy wins");
    check(open_w(g_game_root + L"\\DATA\\Redirect_Test\\BOTH.TXT") == "from profile", "CreateFileW: any case");
    check(open_w(g_game_root + L"/data/redirect_test/both.txt") == "from profile", "CreateFileW: forward slashes");
    check(open_w(game_data + L"\\game_only.txt") == "game only", "file without a profile copy: game's");
    check(open_w(game_data + L"\\profile_only.txt") == "profile only", "file only in the profile: opens");
    check(open_w(elsewhere + L"\\both.txt") == "elsewhere", "same path outside the game root: untouched");

    const std::wstring only = game_data + L"\\profile_only.txt";
    check(GetFileAttributesW(only.c_str()) != INVALID_FILE_ATTRIBUTES, "GetFileAttributesW sees a profile-only file");
    check(GetFileAttributesA(utf8(only).c_str()) != INVALID_FILE_ATTRIBUTES, "GetFileAttributesA sees it too");
    WIN32_FILE_ATTRIBUTE_DATA info = {};
    check(GetFileAttributesExW(only.c_str(), GetFileExInfoStandard, &info) && info.nFileSizeLow == std::strlen("profile only"),
          "GetFileAttributesExW reports the profile file's size");
    info = {};
    check(GetFileAttributesExA(utf8(game_data + L"\\both.txt").c_str(), GetFileExInfoStandard, &info) &&
              info.nFileSizeLow == std::strlen("from profile"),
          "GetFileAttributesExA reports the profile copy's size");
    check(GetFileAttributesW((game_data + L"\\nowhere.txt").c_str()) == INVALID_FILE_ATTRIBUTES, "missing everywhere: missing");

    DWORD attrs = GetFileAttributesW((g_game_root + L"\\data\\redirect_test_new").c_str());
    check(attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY), "profile-only folder shows as a folder");
    attrs = GetFileAttributesW((g_game_root + L"\\data\\redirect_test_new\\sub").c_str());
    check(attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY), "... and nested ones");

    // The log names each redirect once, however often the file is opened (the cache).
    text = test::read_file(g_log);
    const std::string line = "redirect data\\redirect_test\\both.txt -> " + utf8(prof_data) + "\\both.txt\n";
    check(contains(text, line), "agent.log names the redirect");
    check(text.find(line) == text.rfind(line), "... once (cached)");
    return text;
}

}  // namespace

int main(int argc, char** argv) {
    test::exit_on_crash();
    const std::string scenario = argc > 1 ? argv[1] : "";
    g_win64 = test::exe_dir();
    g_game_root = test::dir_of(g_win64);
    g_dll = g_win64 + L"\\sc_agent.dll";
    std::filesystem::create_directories(test::dir_of(g_game_root) + L"\\profiles");
    g_profile = test::fresh_dir(test::dir_of(g_game_root) + L"\\profiles\\" + sf::wide(scenario));
    g_log = g_profile + L"\\agent.log";

    std::string text;
    if (scenario == "no_profile") no_profile();
    else if (scenario == "broken_profile") text = broken_profile();
    else if (scenario == "shared_data") text = shared_data();
    else if (scenario == "own_data") text = own_data();
    else if (scenario == "redirect") text = redirect();
    else {
        std::printf("unknown scenario '%s'\n", scenario.c_str());
        return 2;
    }
    if (test::failures) std::printf("--- agent.log ---\n%s", text.c_str());
    return test::failures ? 1 : 0;
}
