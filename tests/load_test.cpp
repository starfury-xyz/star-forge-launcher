// Loads sc_agent.dll into this process the way sc-inject loads it into game.exe, then checks
// what the agent did. Built into <build>/fakegame/win64/ so the agent sees <build>/fakegame as
// the game root, like <install>\win64\game.exe. Runs under wine via ctest.

#include "common/text.hpp"
#include "test_util.hpp"

using sf::utf8;
using test::check;
using test::contains;

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

    // 3. A valid profile: the agent reads pack.json + local.json.
    test::write_file(profile + L"\\pack.json", R"({ "format": 1, "id": "org.example.test", "name": "Test Pack", "version": "1.0.0" })");
    test::write_file(profile + L"\\local.json", R"({ "name": "My Server", "trace_files": true })");
    m = LoadLibraryW(dll.c_str());
    check(m != nullptr, "loads with a valid profile");
    text = test::read_file(log);
    check(!contains(text, "profile error"), "no profile error");
    check(contains(text, "profile name: My Server\n"), "agent.log has the local.json name");
    check(contains(text, "tracing file access\n"), "agent.log notes trace_files");
    if (m) FreeLibrary(m);

    if (test::failures) std::printf("--- agent.log ---\n%s", text.c_str());
    return test::failures ? 1 : 0;
}
