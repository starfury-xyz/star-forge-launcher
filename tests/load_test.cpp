// Loads sc_agent.dll into this process the way sc-inject loads it into game.exe, then checks
// what the agent did. Built into <build>/fakegame/win64/ so the agent sees <build>/fakegame as
// the game root, like <install>\win64\game.exe. Runs under wine via ctest.

#include "common/text.hpp"
#include "test_util.hpp"

#include <shlobj.h>

#include <filesystem>

using sf::utf8;
using test::check;
using test::contains;

// What SHGetFolderPath answers right now (the game asks this for its data folder).
static std::wstring local_appdata_w(int csidl) {
    wchar_t buf[MAX_PATH] = {};
    return SHGetFolderPathW(nullptr, csidl, nullptr, 0, buf) == S_OK ? buf : L"(failed)";
}
static std::string local_appdata_a(int csidl) {
    char buf[MAX_PATH] = {};
    return SHGetFolderPathA(nullptr, csidl, nullptr, 0, buf) == S_OK ? buf : "(failed)";
}

int main() {
    const std::wstring win64 = test::exe_dir();
    const std::wstring game_root = test::dir_of(win64);
    const std::wstring dll = win64 + L"\\sc_agent.dll";
    const std::wstring profile = test::fresh_dir(game_root + L"\\profile");
    const std::wstring log = profile + L"\\agent.log";
    std::string text;

    // 1. Not started by the launcher (no SC_PROFILE): the agent loads and does nothing.
    SetEnvironmentVariableW(L"SC_PROFILE", nullptr);
    HMODULE m = LoadLibraryW(dll.c_str());
    check(m != nullptr, "loads without SC_PROFILE");
    check(GetFileAttributesW(log.c_str()) == INVALID_FILE_ATTRIBUTES, "no agent.log without SC_PROFILE");
    if (m) FreeLibrary(m);

    // 2. Started by the launcher, but the profile has no pack.json: the agent logs where it is,
    //    reports the profile error and still lets the game start. A trailing slash and forward
    //    slashes in SC_PROFILE must be normalized away.
    std::wstring env = profile;
    for (auto& c : env)
        if (c == L'\\') c = L'/';
    env += L"/";
    SetEnvironmentVariableW(L"SC_PROFILE", env.c_str());
    m = LoadLibraryW(dll.c_str());
    check(m != nullptr, "loads with a broken profile");
    text = test::read_file(log);
    check(contains(text, "sc-agent "), "agent.log has the version line");
    check(contains(text, "game root: " + utf8(game_root) + "\n"), "agent.log has the game root");
    check(contains(text, "profile:   " + utf8(profile) + "\n"), "agent.log has the normalized profile");
    check(contains(text, "profile error, agent disabled: "), "agent.log reports the missing pack.json");
    if (m) FreeLibrary(m);
    if (test::failures) std::printf("--- agent.log ---\n%s", text.c_str());

    // What Windows answers before the agent changes anything.
    const std::wstring real_local = local_appdata_w(CSIDL_LOCAL_APPDATA);
    const std::wstring real_roaming = local_appdata_w(CSIDL_APPDATA);
    const std::wstring state = profile + L"\\state\\AppData\\Local";
    std::filesystem::remove_all(profile + L"\\state");

    // 3. A valid profile that keeps the shared game data (local.json shared_game_data): the
    //    agent reads pack.json + local.json and leaves %LOCALAPPDATA% alone.
    test::write_file(profile + L"\\pack.json", R"({ "format": 1, "id": "org.example.test", "name": "Test Pack", "version": "1.0.0" })");
    test::write_file(profile + L"\\local.json", R"({ "name": "My Server", "trace_files": true, "shared_game_data": true })");
    m = LoadLibraryW(dll.c_str());
    check(m != nullptr, "loads with a valid profile");
    text = test::read_file(log);
    check(!contains(text, "profile error"), "no profile error");
    check(contains(text, "profile name: My Server\n"), "agent.log has the local.json name");
    check(contains(text, "tracing file access\n"), "agent.log notes trace_files");
    check(!contains(text, "game data folder"), "shared game data: no folder redirect");
    check(local_appdata_w(CSIDL_LOCAL_APPDATA) == real_local, "shared game data: %LOCALAPPDATA% unchanged");
    check(GetFileAttributesW(state.c_str()) == INVALID_FILE_ATTRIBUTES, "shared game data: no state folder");
    if (m) FreeLibrary(m);
    if (test::failures) std::printf("--- agent.log ---\n%s", text.c_str());

    // 4. The default, own game data: %LOCALAPPDATA% becomes <profile>\state\AppData\Local, for
    //    the A and W functions and with flags like CSIDL_FLAG_CREATE; other folders don't change.
    //    The DLL stays loaded from here on: its hooks live in it (game.exe never unloads it either).
    test::write_file(profile + L"\\local.json", R"({ "name": "My Server" })");
    m = LoadLibraryW(dll.c_str());
    check(m != nullptr, "loads with own game data");
    text = test::read_file(log);
    check(contains(text, "game data folder: " + utf8(state) + "\n"), "agent.log has the game data folder");
    check(GetFileAttributesW(state.c_str()) & FILE_ATTRIBUTE_DIRECTORY && GetFileAttributesW(state.c_str()) != INVALID_FILE_ATTRIBUTES,
          "state folder created");
    check(local_appdata_w(CSIDL_LOCAL_APPDATA) == state, "SHGetFolderPathW(LOCAL_APPDATA) -> profile");
    check(local_appdata_w(CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE) == state, "... also with CSIDL_FLAG_CREATE");
    check(local_appdata_a(CSIDL_LOCAL_APPDATA) == utf8(state), "SHGetFolderPathA(LOCAL_APPDATA) -> profile");
    check(local_appdata_w(CSIDL_APPDATA) == real_roaming, "other folders (APPDATA) unchanged");

    if (test::failures) std::printf("--- agent.log ---\n%s", text.c_str());
    return test::failures ? 1 : 0;
}
