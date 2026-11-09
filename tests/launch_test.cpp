// Unit test for sf::launch (the launcher library). Runs natively on Linux and under wine for
// Windows. Groups marked "Rust:" port sc-launch's tests case for case.

#include <algorithm>

#include "launch/env.hpp"
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
    }

    return test::failures ? 1 : 0;
}
