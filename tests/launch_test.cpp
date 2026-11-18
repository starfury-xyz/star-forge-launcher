// Unit test for sf::launch (the launcher library). Runs natively on Linux and under wine for
// Windows. Groups marked "Rust:" port sc-launch's tests case for case.

#include <algorithm>
#include <thread>

#include "launch/diagnostics.hpp"
#include "launch/env.hpp"
#include "launch/gamescope.hpp"
#include "launch/paths.hpp"
#include "launch/steam.hpp"

#include "check.hpp"

using namespace sf::launch;
using test::check;

namespace {

bool has(const std::vector<fs::path>& v, const fs::path& p) { return std::find(v.begin(), v.end(), p) != v.end(); }

// A fake Steam install with Star Conflict in a second library folder:
//   <root>/steam/steamapps/libraryfolders.vdf -> <root>/lib2
//   <root>/lib2/steamapps/appmanifest_212070.acf -> common/star conflict (win64/game.exe, data/)
void fake_steam(const fs::path& root) {
    const fs::path steam = root / "steam", lib2 = root / "lib2 (games)";
    fs::create_directories(steam / "steamapps");
    // VDF escapes backslashes, which Windows paths are full of.
    std::string lib2_vdf;
    for (char c : ustr(lib2)) lib2_vdf += c == '\\' ? std::string("\\\\") : std::string(1, c);
    test::write_file(steam / "steamapps" / "libraryfolders.vdf",
                     "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" + lib2_vdf + "\"\n\t}\n"
                     "\t\"1\"\n\t{\n\t\t\"path\"\t\t\"" + ustr(root / "missing") + "\"\n\t}\n}\n");
    test::write_file(lib2 / "steamapps" / "appmanifest_212070.acf",
                     "\"AppState\"\n{\n\t\"appid\"\t\t\"212070\"\n\t\"installdir\"\t\t\"star conflict\"\n}\n");
    test::write_file(lib2 / "steamapps" / "common" / "star conflict" / "win64" / "game.exe", "MZ");
    fs::create_directories(lib2 / "steamapps" / "common" / "star conflict" / "data");
}

}  // namespace

int main() {
    test::exit_on_crash();
    const fs::path tmp = test::fresh_dir(fs::temp_directory_path() / "sf_launch_test");

    // Rust (lib.rs): plain_parts
    for (const char* ok : {"data", "a.lua", "x.y.z", ".gitkeep", "console.lua", "com.lua", "comx", "lpt10"})
        check(plain_part(ok), std::string("plain part: ") + ok);
    for (std::string bad : {"", ".", "..", "a.", "a ", "C:", "a*b", "con", "CON.lua", "nul .txt", "com1", "LPT9.cfg", "aux"})
        check(!plain_part(bad), "not a plain part: \"" + bad + "\"");
    check(!plain_part(std::string("a\0", 2)), "not a plain part: a\\0");
    check(!plain_part("a\xc2\x85" "b"), "not a plain part: C1 control U+0085");
    check(plain_part("\xc3\xa9t\xc3\xa9"), "plain part: non-ASCII letters");

    // File errors: read_text, write_or_throw, rename_replacing and the messages they give.
    {
        const fs::path dir = tmp / "files";
        fs::create_directories(dir);
        std::error_code ec;
        check(!read_text(dir / "missing.json", &ec) && not_found(ec), "read_text: a missing file is not_found");
        check(!read_text(dir / "no" / "such" / "dir.json", &ec) && not_found(ec), "read_text: a missing folder is not_found");
        write_or_throw(dir / "a.json", "{}\n");
        check(read_text(dir / "a.json", &ec) == std::optional<std::string>("{}\n") && !ec, "write_or_throw then read_text");
        write_or_throw(dir / ".a.json.tmp", "new\n");
        rename_replacing(dir / ".a.json.tmp", dir / "a.json", ec);
        check(!ec && read_or_throw(dir / "a.json") == "new\n", "rename_replacing replaces the file");
        std::string what;
        try {
            read_or_throw(dir / "missing.json");
        } catch (const std::exception& e) {
            what = e.what();
        }
        check(what.find("missing.json: ") != std::string::npos && what.find("(os error 2)") != std::string::npos,
              "read_or_throw names the file and the error: " + what);
#ifdef _WIN32
        // Another program holding the file without letting others read: a sharing violation,
        // reported as such (not errno 13), with what to do about it.
        HANDLE held = CreateFileW((dir / "a.json").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(held != INVALID_HANDLE_VALUE, "test holds a.json open, unshared");
        check(!read_text(dir / "a.json", &ec) && ec.value() == ERROR_SHARING_VIOLATION && ec.category() == std::system_category(),
              "read_text: a file held open is a sharing violation (os error 32)");
        const std::string msg = io_msg(ec);
        check(msg.find("(os error 32)") != std::string::npos && msg.find("another program has the file open") != std::string::npos,
              "io_msg: a sharing violation says another program has it open: " + msg);
        CloseHandle(held);
        // Let go of within the retry window: read_text waits for it.
        held = CreateFileW((dir / "a.json").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        std::thread release([held] {
            Sleep(300);
            CloseHandle(held);
        });
        check(read_text(dir / "a.json", &ec) == std::optional<std::string>("new\n"), "read_text waits for a file let go of within a second");
        release.join();
        const std::string denied = io_msg(std::error_code(ERROR_ACCESS_DENIED, std::system_category()));
        check(denied.find("(os error 5)") != std::string::npos && denied.find("permissions") != std::string::npos,
              "io_msg: access denied points at permissions: " + denied);
#endif
    }

    // Diagnostics: every profile's pack.json read, parsed and saved, with the exact error.
    {
        const fs::path profiles = tmp / "diag-profiles";
        fs::create_directories(profiles / "good");
        fs::create_directories(profiles / "broken");
        test::write_file(profiles / "good" / "pack.json",
                         "{\"format\": 1, \"id\": \"local.good\", \"name\": \"Good\", \"version\": \"1.0.0\"}\n");
        test::write_file(profiles / "good" / "agent.log", "agent\n");
        test::write_file(profiles / "broken" / "pack.json", "{\"format\": 1,");
#ifdef _WIN32
        HANDLE held = CreateFileW((profiles / "good" / "pack.json").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
#endif
        const diagnostics::Report r = diagnostics::collect({"test", std::nullopt, profiles, std::nullopt, "no game here"});
#ifdef _WIN32
        CloseHandle(held);
        check(r.text.find("pack.json: CANNOT READ: Sharing violation (os error 32)") != std::string::npos,
              "diagnostics: a held pack.json is reported as a sharing violation");
#else
        check(r.text.find("pack.json: read ") != std::string::npos && r.text.find("parsed: local.good 1.0.0 \"Good\"") != std::string::npos,
              "diagnostics: a good pack.json is read and parsed");
#endif
        check(r.text.find("[profile broken]") != std::string::npos && r.text.find("CANNOT PARSE") != std::string::npos,
              "diagnostics: a broken pack.json can't be parsed");
        check(r.text.find("save test: OK") != std::string::npos, "diagnostics: the profiles folder can be saved to");
        check(r.text.find("not found: no game here") != std::string::npos, "diagnostics: says why the game wasn't found");
        bool has_log = false;
        for (const auto& [name, path] : r.files) has_log = has_log || name == "profiles/good/agent.log";
        check(has_log, "diagnostics: a profile's agent.log goes in the zip");
        const std::string z = diagnostics::zip(r);
        check(z.size() > 100 && z.compare(0, 2, "PK") == 0, "diagnostics: zip() makes a zip");
        std::error_code ec;
        check(!fs::exists(profiles / "good" / ".sf-diagnostics.tmp", ec) && !fs::exists(profiles / "good" / ".sf-diagnostics-probe", ec),
              "diagnostics: the save test cleans up");
        if (auto h = get_env(
#ifdef _WIN32
                "USERPROFILE"
#else
                "HOME"
#endif
                )) {
            check(diagnostics::redact("at " + *h + "/x") == "at ~/x", "diagnostics: redact writes the home folder as ~");
        }
    }

    // Rust (env.rs): parses_dotenv_lines
    {
        auto got = env::parse_dotenv(
            "# comment\n\nSTAR_FORGE_GAME_DIR=\"/games/star conflict\"\nexport STAR_FORGE_PROTON='/p'\nEMPTY=\nnot a line\n");
        decltype(got) want{{"STAR_FORGE_GAME_DIR", "/games/star conflict"}, {"STAR_FORGE_PROTON", "/p"}, {"EMPTY", ""}};
        check(got == want, "parse_dotenv");
        check(env::parse_dotenv("A=\"\r\nB='x\"\r\n") == decltype(got){{"A", "\""}, {"B", "'x\""}},
              "parse_dotenv: CRLF lines, unmatched quotes kept");
    }

    // Rust (env.rs): strips_appimage_paths
    {
        std::vector<std::pair<std::string, std::string>> vars{
            {"LD_LIBRARY_PATH", "/tmp/.mount_x/usr/lib/:/tmp/.mount_x/lib/:/opt/host/lib"},
            {"PYTHONHOME", "/tmp/.mount_x/usr/"},
            {"XDG_DATA_DIRS", "/tmp/.mount_x/usr/share:/usr/share:/usr/local/share"},
            {"GTK_THEME", "Adwaita:dark"},
            {"HOME", "/home/user"},
        };
        env::Changes want{{"LD_LIBRARY_PATH", "/opt/host/lib"},
                          {"PYTHONHOME", std::nullopt},
                          {"XDG_DATA_DIRS", "/usr/share:/usr/local/share"},
                          {"GTK_THEME", std::nullopt}};
        check(env::host_env_changes(vars, "/tmp/.mount_x/") == want, "host_env_changes");
        check(env::host_env_changes(vars, "/").empty(), "host_env_changes: appdir \"/\" (empty after trimming) changes nothing");
    }

    // env::path: unset and empty both mean "not set".
    set_env("SF_TEST_VAR", std::nullopt);
    check(!env::path("SF_TEST_VAR"), "env::path: unset");
    set_env("SF_TEST_VAR", "");
    check(!env::path("SF_TEST_VAR"), "env::path: empty");
    set_env("SF_TEST_VAR", "/x/\xc3\xa9");
    check(env::path("SF_TEST_VAR") == upath("/x/\xc3\xa9"), "env::path: set (UTF-8 survives)");
    // Not under an AppImage (the test isn't one): no changes.
    check(env::host_env().empty() || get_env("APPIMAGE"), "host_env: nothing outside an AppImage");

    // Rust (steam.rs): reads_library_paths_with_escapes, reads_installdir_and_tool_appid
    {
        const std::string text = R"("libraryfolders" { "0" { "path" "/home/u/.local/share/Steam" "apps" { "212070" "1" } }
            "1" { "path" "D:\\Games\\Steam" } })";
        check(steam::vdf_values(text, "path") == std::vector<std::string>{"/home/u/.local/share/Steam", "D:\\Games\\Steam"},
              "vdf: library paths, \\\\ unescaped");
        const std::string acf = "\"AppState\"\n{\n\t\"appid\"\t\t\"212070\"\n\t\"installdir\"\t\t\"star conflict\"\n}";
        check(steam::vdf_values(acf, "INSTALLDIR") == std::vector<std::string>{"star conflict"}, "vdf: installdir (any case)");
        const std::string manifest = "\"manifest\"\n{\n  \"require_tool_appid\" \"4183110\"\n}";
        check(steam::vdf_values(manifest, "require_tool_appid") == std::vector<std::string>{"4183110"}, "vdf: tool appid");
        check(steam::vdf_values("\"say\" \"\\\"hi\\\"\"", "say") == std::vector<std::string>{"\"hi\""}, "vdf: \\\" unescaped");
    }

    // A fake Steam: roots, libraries, the game. steam_roots() looks where this OS keeps Steam,
    // so point that at the fake (HOME on Linux, Program Files on Windows).
    {
        const fs::path root = tmp / "fake";
        fake_steam(root);
        const fs::path steam = root / "steam", lib2 = root / "lib2 (games)";
#ifdef _WIN32
        // <ProgramFiles(x86)>\Steam
        fs::rename(steam, root / "Steam");
        const fs::path found_root = root / "Steam";
        set_env("ProgramFiles(x86)", ustr(root));
#else
        // ~/.local/share/Steam, and ~/.steam/steam pointing at it like a real install.
        const fs::path home = tmp / "home";
        fs::create_directories(home / ".local/share");
        fs::create_directories(home / ".steam");
        fs::rename(steam, home / ".local/share/Steam");
        fs::create_directory_symlink(home / ".local/share/Steam", home / ".steam/steam");
        const fs::path found_root = home / ".local/share/Steam";
        set_env("HOME", ustr(home));
#endif
        auto roots = steam::steam_roots();
        check(has(roots, found_root), "steam_roots: finds the fake Steam");
        // (On a real Windows machine the registry may add the real Steam too: only count ours.)
        check(std::count(roots.begin(), roots.end(), found_root) == 1, "steam_roots: the fake Steam once");
#ifndef _WIN32
        check(roots.size() == 1, "steam_roots: ~/.steam/steam is the same folder, so not listed again");
#endif

        auto libs = steam::libraries({found_root});
        check(libs.size() == 2 && libs[0] == found_root && libs[1] == lib2, "libraries: the root, then libraryfolders.vdf's");
        check(!has(libs, root / "missing"), "libraries: a listed folder without steamapps is left out");
        check(steam::libraries({found_root, found_root}).size() == 2, "libraries: each once");

        auto app = steam::find_app(libs, steam::kGameAppid);
        check(app && app->library == lib2 && app->dir == lib2 / "steamapps" / "common" / "star conflict",
              "find_app: Star Conflict in the second library");
        check(app && steam::is_game_dir(app->dir), "is_game_dir: win64/game.exe + data/");
        check(!steam::find_app(libs, "1"), "find_app: an app that isn't installed");
        check(!steam::is_game_dir(lib2), "is_game_dir: some other folder");
        fs::create_directories(tmp / "data_only" / "data");
        check(!steam::is_game_dir(tmp / "data_only"), "is_game_dir: data/ alone isn't enough");
        fs::create_directories(tmp / "exe_only" / "win64");
        test::write_file(tmp / "exe_only" / "win64" / "game.exe", "MZ");
        check(!steam::is_game_dir(tmp / "exe_only"), "is_game_dir: win64/game.exe alone isn't enough");
    }

    // Gamescope (Steam Deck Game Mode): which app id the game's windows get.
    {
        using sf::launch::gamescope::app_id;
        using sf::launch::gamescope::is_wine_class;
        check(app_id("gamescope", nullptr, "212070") == 212070u, "gamescope: a Steam game's id as is");
        check(app_id(nullptr, "gamescope", "212070") == 212070u, "gamescope: XDG_SESSION_DESKTOP counts too");
        // A non-Steam shortcut's 64-bit game id: the app id is its top 32 bits.
        check(app_id("gamescope", nullptr, "16429544735187271680") == 3825301475u, "gamescope: a shortcut's app id");
        check(!app_id("KDE", "KDE", "212070"), "gamescope: not in a gamescope session");
        check(!app_id("gamescope", nullptr, nullptr) && !app_id("gamescope", nullptr, "") &&
                  !app_id("gamescope", nullptr, "12x") && !app_id("gamescope", nullptr, "0"),
              "gamescope: no usable SteamGameId");
        check(is_wine_class("steam_app_212070") && is_wine_class("game.exe") && is_wine_class("SC-INJECT.EXE"),
              "gamescope: Wine window classes");
        check(!is_wine_class("Star-forge") && !is_wine_class(".exe") && !is_wine_class("steamwebhelper"),
              "gamescope: other window classes");
    }
    return test::failures ? 1 : 0;
}
