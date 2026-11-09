// Loads sc_agent.dll into this process the way sc-inject loads it into game.exe, then checks
// what the agent did. Built into <build>/fakegame/win64/ so the agent sees <build>/fakegame as
// the game root, like <install>\win64\game.exe. Runs under wine via ctest.
//
// Usage: load_test <scenario>. ctest runs each scenario in its own process: once the agent has
// hooked something it must never be unloaded (a hook pointing into an unloaded DLL crashes the
// next caller), and game.exe never unloads it either. Each scenario gets its own profile in
// <build>/profiles/<scenario>, outside the game folder like a real profile.

#include "common/file.hpp"
#include "common/text.hpp"
#include "patch/patchset.hpp"
#include "patch/sha1.hpp"
#include "test_util.hpp"

#include <shlobj.h>

#include <algorithm>
#include <cstring>
#include <vector>
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
    check((m != nullptr) == loaded_expected, loaded_expected ? "agent loads" : "agent refuses to load (the game must not start)");
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

// Every entry a listing returns, as "name:size" sorted by name, via FindFirstFileA (or ...ExA)
// + FindNextFileA + FindClose, the way the engine scans data\. "(none)" if the search fails.
std::string list(const std::wstring& pattern, bool ex = false) {
    WIN32_FIND_DATAA d = {};
    const std::string p = utf8(pattern);
    HANDLE h = ex ? FindFirstFileExA(p.c_str(), FindExInfoBasic, &d, FindExSearchNameMatch, nullptr, 0)
                  : FindFirstFileA(p.c_str(), &d);
    if (h == INVALID_HANDLE_VALUE) return "(none)";
    std::vector<std::string> names;
    do {
        const std::string n = d.cFileName;
        // "." and ".." are listed without a size: theirs is whatever the system says.
        names.push_back(n == "." || n == ".." ? n : n + ":" + std::to_string(d.nFileSizeLow));
    } while (FindNextFileA(h, &d));
    const bool done_ok = GetLastError() == ERROR_NO_MORE_FILES;
    // On Windows, FindClose fails for a handle it didn't make, so this catches the agent passing
    // its own listing handles to the real FindClose. Wine's FindClose returns TRUE for any
    // handle, so under wine that bug goes unnoticed: run the tests on Windows too.
    const bool closed = FindClose(h);
    std::sort(names.begin(), names.end());
    std::string out;
    for (const auto& n : names) out += (out.empty() ? "" : " ") + n;
    if (!done_ok) out += " (FindNextFileA ended without ERROR_NO_MORE_FILES)";
    if (!closed) out += " (FindClose failed)";
    return out;
}

// Folder listings: a folder that also exists in the profile lists the game's entries plus the
// profile's, the profile's copy replacing a same-named game file.
std::string listing() {
    test::write_file(g_profile + L"\\pack.json", kPack);
    const std::wstring game_dir = g_game_root + L"\\data\\listing_test";
    const std::wstring prof_dir = g_profile + L"\\files\\data\\listing_test";
    const std::wstring plain_dir = g_game_root + L"\\data\\listing_plain";  // no profile folder
    test::fresh_dir(game_dir);
    test::fresh_dir(plain_dir);
    std::filesystem::create_directories(prof_dir);
    std::filesystem::create_directories(g_profile + L"\\files\\data\\listing_new");
    test::write_file(game_dir + L"\\a.txt", "a");
    test::write_file(game_dir + L"\\Shared.txt", "game");      // 4 bytes; the profile's copy is 7
    test::write_file(game_dir + L"\\readme.md", "md");
    test::write_file(prof_dir + L"\\shared.txt", "profile");    // same name, other case
    test::write_file(prof_dir + L"\\extra.txt", "extra");
    test::write_file(plain_dir + L"\\p.txt", "p");
    test::write_file(g_profile + L"\\files\\data\\listing_new\\n.txt", "n");
    const std::wstring elsewhere = test::fresh_dir(test::dir_of(g_game_root) + L"\\elsewhere\\data\\listing_test");
    test::write_file(elsewhere + L"\\e.txt", "e");

    std::string text = load_agent();
    check(contains(text, "file hooks installed\n"), "agent.log: file hooks installed");

    const std::string merged = ". .. a.txt:1 extra.txt:5 readme.md:2 shared.txt:7";
    const std::string got = list(game_dir + L"\\*");
    std::printf("  data\\listing_test\\*: %s\n", got.c_str());
    check(got == merged, "FindFirstFileA: game + profile entries, profile's shared.txt replaces the game's");
    check(list(game_dir + L"\\*", true) == merged, "FindFirstFileExA: the same");
    check(list(game_dir + L"\\*.txt") == "a.txt:1 extra.txt:5 shared.txt:7", "a *.txt pattern applies to both");
    check(list(game_dir + L"\\*.none") == "(none)", "a pattern matching nothing: no listing");
    // The game has no such folder, so no "." / "..": only the profile's file.
    check(list(g_game_root + L"\\data\\listing_new\\*") == "n.txt:1", "a profile-only folder lists its files");
    check(list(plain_dir + L"\\*") == ". .. p.txt:1", "a folder with no profile copy: game's entries only");
    check(list(elsewhere + L"\\*") == ". .. e.txt:1", "same path outside the game root: untouched");

    text = test::read_file(g_log);
    check(contains(text, "list data\\listing_test: adding extra.txt\n"), "agent.log names the added entries");
    check(!contains(text, "list data\\listing_plain"), "agent.log: nothing added to a folder without a profile copy");
    return text;
}


// --- patches -----------------------------------------------------------------------------------
// load_test.exe plays game.exe, so it patches itself: the patchsets below change these markers.
// volatile: the compiler must really read them, not assume they still hold their first value.

volatile unsigned char g_marker[8] = {'E', 'X', 'E', '-', 'O', 'R', 'I', 'G'};
volatile unsigned char g_marker2[8] = {'S', 'E', 'C', 'O', 'N', 'D', '-', '1'};

std::string text_of(const volatile unsigned char* m) {
    std::string s;
    for (int i = 0; i < 8; ++i) s += static_cast<char>(m[i]);
    return s;
}
std::string hex_of(const std::string& s) { return sf::patch::to_hex(sf::patch::Bytes(s.begin(), s.end())); }

std::string file_sha1(const std::wstring& path) {
    std::string data;
    sf::read_file(path, data);
    return sf::patch::sha1_hex(data.data(), data.size());
}

// Patchset address of something in load_test.exe, for a set with image_base 0x140000000.
constexpr uint64_t kExeBase = 0x140000000;
std::string exe_va(const volatile void* p) {
    auto rva = reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return sf::patch::hex_va(kExeBase + rva);
}

// One [[patch]] with a single write turning `from` into `to` at `va`; `extra` adds fields.
std::string patch_toml(const std::string& id, const std::string& va, const std::string& from, const std::string& to,
                       const std::string& extra = "") {
    return "\n[[patch]]\nid = \"" + id + "\"\n" + extra + "[[patch.write]]\nva = " + va + "\norig = \"" + hex_of(from) +
           "\"\nbytes = \"" + hex_of(to) + "\"\n";
}

std::string set_toml(const std::string& exe, const std::string& sha1, uint64_t image_base, const std::string& patches) {
    return "format = 1\nexe = \"" + exe + "\"\nsha1 = \"" + sha1 + "\"\nimage_base = " + sf::patch::hex_va(image_base) +
           "\n" + patches;
}

void write_patchset(const std::string& file, const std::string& text) {
    std::filesystem::create_directories(g_profile + L"\\patches");
    test::write_file(g_profile + L"\\patches\\" + sf::wide(file), text);
}

// pack.json with the given "patches" object (JSON), e.g. { "on_error": "continue" }.
void write_pack(const std::string& patches_json) {
    test::write_file(g_profile + L"\\pack.json",
                     R"({ "format": 1, "id": "org.example.test", "name": "Test Pack", "version": "1.0.0", "patches": )" +
                         patches_json + " }");
}

// Patches applied to game.exe (here load_test.exe) before it runs. Also: an optional patch that
// doesn't match is skipped, and a patch the player turned off in local.json isn't applied.
std::string patch_exe() {
    write_pack("{}");
    const std::string sha = file_sha1(g_win64 + L"\\load_test.exe");
    write_patchset("load_test.exe.patchset.toml",
                   set_toml("load_test.exe", sha, kExeBase,
                            patch_toml("marker", exe_va(g_marker), "EXE-ORIG", "EXE-DONE", "title = \"Test marker\"\n") +
                                patch_toml("wrong", exe_va(g_marker2), "XXXXXXXX", "YYYYYYYY", "optional = true\n") +
                                patch_toml("player_off", exe_va(g_marker2), "SECOND-1", "SECOND-2")));
    // Module names in local.json match ignoring case.
    test::write_file(g_profile + L"\\local.json", R"({ "patches": { "LOAD_TEST.EXE": { "player_off": false } } })");
    std::string text = load_agent();
    check(text_of(g_marker) == "EXE-DONE", "the patch changed the exe's memory");
    check(text_of(g_marker2) == "SECOND-1", "optional mismatch and player-disabled patch: not applied");
    check(contains(text, "load_test.exe: applied marker (Test marker), 1 write(s)\n"), "agent.log: applied marker");
    check(contains(text, "load_test.exe: skipping optional patch wrong: bytes at "), "agent.log: optional patch skipped");
    check(contains(text, "load_test.exe: 1 patch(es) applied\n"), "agent.log: 1 patch applied");
    check(contains(text, "file hooks installed"), "the rest of the agent still runs");
    return text;
}

// A required patch whose bytes don't match: on_error "exit" (the default) stops the game, and
// nothing is written.
std::string patch_fail_exit() {
    write_pack("{}");
    write_patchset("load_test.exe.patchset.toml",
                   set_toml("load_test.exe", file_sha1(g_win64 + L"\\load_test.exe"), kExeBase,
                            patch_toml("ok", exe_va(g_marker), "EXE-ORIG", "EXE-DONE") +
                                patch_toml("bad", exe_va(g_marker2), "XXXXXXXX", "YYYYYYYY")));
    std::string text = load_agent(false);
    check(text_of(g_marker) == "EXE-ORIG", "nothing written: a set never half-applies");
    check(contains(text, "ERROR: load_test.exe: bad: bytes at "), "agent.log names the failing patch");
    check(contains(text, "not starting the game (patches.on_error is \"exit\""), "agent.log says the game won't start");
    check(!contains(text, "file hooks installed"), "agent stopped before hooking");
    return text;
}

// The same with on_error "continue": the error is logged and the game runs, unpatched.
std::string patch_fail_continue() {
    write_pack(R"({ "on_error": "continue" })");
    write_patchset("load_test.exe.patchset.toml",
                   set_toml("load_test.exe", file_sha1(g_win64 + L"\\load_test.exe"), kExeBase,
                            patch_toml("bad", exe_va(g_marker2), "XXXXXXXX", "YYYYYYYY")));
    std::string text = load_agent();
    check(contains(text, "(continuing: patches.on_error is \"continue\")"), "agent.log: error, continuing");
    check(contains(text, "file hooks installed"), "the rest of the agent still runs");
    return text;
}

// A patchset for another build of the exe is refused.
std::string patch_wrong_build() {
    write_pack("{}");
    write_patchset("load_test.exe.patchset.toml",
                   set_toml("load_test.exe", std::string(40, '0'), kExeBase,
                            patch_toml("marker", exe_va(g_marker), "EXE-ORIG", "EXE-DONE")));
    std::string text = load_agent(false);
    check(text_of(g_marker) == "EXE-ORIG", "wrong build: nothing written");
    check(contains(text, "load_test.exe is a different build (sha1 "), "agent.log: different build");
    return text;
}

// Address of patch_target.dll's marker in its own address space: the .sfmark section's RVA
// (the marker is alone in that section), read from the DLL file's section table.
constexpr uint64_t kDllBase = 0x180000000;
std::string dll_marker_va(const std::wstring& dll) {
    std::string d;
    sf::read_file(dll, d);
    auto u16 = [&](size_t o) { return uint32_t(uint8_t(d[o])) | uint32_t(uint8_t(d[o + 1])) << 8; };
    auto u32 = [&](size_t o) { return u16(o) | u16(o + 2) << 16; };
    const size_t nt = u32(0x3c), table = nt + 24 + u16(nt + 20);
    for (size_t i = 0; i < u16(nt + 6); ++i)
        if (d.compare(table + i * 40, 7, ".sfmark") == 0) return sf::patch::hex_va(kDllBase + u32(table + i * 40 + 12));
    return "0";
}

void write_dll_patchset(const std::string& from) {
    const std::wstring dll = g_win64 + L"\\patch_target.dll";
    write_patchset("patch_target.dll.patchset.toml",
                   set_toml("patch_target.dll", file_sha1(dll), kDllBase, patch_toml("marker", dll_marker_va(dll), from, "DLL-DONE")));
}

// A patchset for a DLL that isn't loaded yet is applied when it loads, before its DllMain.
std::string patch_dll() {
    write_pack("{}");
    write_dll_patchset("DLL-ORIG");
    std::string text = load_agent();
    check(contains(text, "patch_target.dll: not loaded yet; patching when it loads\n"), "agent.log: DLL patch deferred");
    HMODULE dll = LoadLibraryW((g_win64 + L"\\patch_target.dll").c_str());
    check(dll != nullptr, "patch_target.dll loads");
    if (!dll) return text;
    auto marker = reinterpret_cast<volatile unsigned char*>(GetProcAddress(dll, "marker"));
    auto seen = reinterpret_cast<unsigned char*>(GetProcAddress(dll, "seen_in_dllmain"));
    check(marker && text_of(marker) == "DLL-DONE", "the DLL was patched");
    check(seen && text_of(seen) == "DLL-DONE", "... before its DllMain ran");
    text = test::read_file(g_log);
    check(contains(text, "patch_target.dll: applied marker, 1 write(s)\n"), "agent.log: DLL patch applied");
    return text;
}

// A DLL patch that fails with on_error "exit": by then the game is running, so the agent ends the
// process (exit code 1). That has to happen in a child process: this one starts
// `load_test patch_dll_fail_exit_child` and checks how it ended.
std::string patch_dll_fail_exit_child() {
    write_pack("{}");
    write_dll_patchset("XXXXXXXX");
    load_agent();
    LoadLibraryW((g_win64 + L"\\patch_target.dll").c_str());
    std::printf("still running after the failed DLL patch\n");
    return test::read_file(g_log);
}
std::string patch_dll_fail_exit() {
    std::wstring cmd = L"\"" + g_win64 + L"\\load_test.exe\" patch_dll_fail_exit_child";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    check(CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi), "child starts");
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    check(code == 1, "the agent ended the game with exit code 1");
    std::string text = test::read_file(test::dir_of(g_profile) + L"\\patch_dll_fail_exit_child\\agent.log");
    check(contains(text, "ERROR: patch_target.dll: marker: bytes at "), "agent.log (child): the failing DLL patch");
    return text;
}

// Core patches are found by id; one for game.exe waits for game.exe (this test isn't game.exe).
std::string core_patch() {
    write_pack(R"({ "core": ["loose-before-packed"] })");
    std::string text = load_agent();
    check(contains(text, "game.exe: not loaded yet; patching when it loads\n"), "core patch for game.exe found and deferred");
    return text;
}

std::string core_unknown() {
    write_pack(R"({ "core": ["no-such-patch"] })");
    std::string text = load_agent(false);
    check(contains(text, "ERROR: unknown core patch \"no-such-patch\" (this launcher has: loose-before-packed)"),
          "agent.log: unknown core patch");
    return text;
}

// Plugins: loaded from the profile's patches\ folder; starforge_plugin_init gets the profile
// path and can log; a non-zero result is a failure for on_error.
void install_plugin() {
    std::filesystem::create_directories(g_profile + L"\\patches");
    std::filesystem::copy_file(g_win64 + L"\\test_plugin.dll", g_profile + L"\\patches\\test_plugin.dll",
                               std::filesystem::copy_options::overwrite_existing);
}

std::string plugin() {
    install_plugin();
    write_pack(R"({ "plugins": ["patches/test_plugin.dll"] })");
    std::string text = load_agent();
    check(contains(text, "plugin: hello from test plugin, profile has " + std::to_string(g_profile.size()) + " chars\n"),
          "the plugin ran, got the profile path and logged");
    check(contains(text, "plugin patches/test_plugin.dll: loaded\n"), "agent.log: plugin loaded");
    return text;
}

std::string plugin_fail() {
    install_plugin();
    test::write_file(g_profile + L"\\plugin_fail", "");
    write_pack(R"({ "on_error": "continue", "plugins": ["patches/test_plugin.dll"] })");
    std::string text = load_agent();
    check(contains(text, "ERROR: plugin patches/test_plugin.dll: init returned 7 (continuing"), "agent.log: plugin init failed");
    check(!contains(text, "plugin patches/test_plugin.dll: loaded"), "not reported as loaded");
    return text;
}

std::string plugin_missing() {
    write_pack(R"({ "plugins": ["patches/nope.dll"] })");
    std::string text = load_agent(false);
    check(contains(text, "ERROR: plugin patches/nope.dll: could not be loaded"), "agent.log: missing plugin");
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
    else if (scenario == "listing") text = listing();
    else if (scenario == "patch_exe") text = patch_exe();
    else if (scenario == "patch_fail_exit") text = patch_fail_exit();
    else if (scenario == "patch_fail_continue") text = patch_fail_continue();
    else if (scenario == "patch_wrong_build") text = patch_wrong_build();
    else if (scenario == "patch_dll") text = patch_dll();
    else if (scenario == "patch_dll_fail_exit") text = patch_dll_fail_exit();
    else if (scenario == "patch_dll_fail_exit_child") text = patch_dll_fail_exit_child();
    else if (scenario == "core_patch") text = core_patch();
    else if (scenario == "core_unknown") text = core_unknown();
    else if (scenario == "plugin") text = plugin();
    else if (scenario == "plugin_fail") text = plugin_fail();
    else if (scenario == "plugin_missing") text = plugin_missing();
    else {
        std::printf("unknown scenario '%s'\n", scenario.c_str());
        return 2;
    }
    if (test::failures) std::printf("--- agent.log ---\n%s", text.c_str());
    return test::failures ? 1 : 0;
}
