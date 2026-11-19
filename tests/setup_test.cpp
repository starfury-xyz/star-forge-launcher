// Tests sf::launch's setup (launch.hpp): Proton detection, resolve(), and the command that starts
// sc-inject, on a fake Steam tree. Groups marked "Rust:" port sc-launch's tests case for case.

#include <algorithm>

#include "launch/launch.hpp"
#include "launch/profiles.hpp"

#include "check.hpp"

using namespace sf::launch;
using test::check;
using test::contains;

namespace {

void touch(const fs::path& p, const std::string& text = "") { test::write_file(p, text); }

// The message of the std::exception `f` throws ("" if it doesn't).
template <class F>
std::string error_of(F f) {
    try {
        f();
    } catch (const std::exception& e) {
        return e.what();
    }
    return "";
}

// The message of the SetupError `f` throws ("" if it doesn't), and its fix.
template <class F>
std::string setup_error(F f, std::optional<SteamFix>* fix = nullptr) {
    try {
        f();
    } catch (const SetupError& e) {
        if (fix) *fix = e.fix;
        return e.what();
    }
    return "";
}

// Steam's own Proton setup unless another runner is asked for.
Options opts(std::optional<fs::path> game = {}, std::optional<fs::path> tools = {}, std::optional<fs::path> proton = {},
             std::optional<std::string> runner = std::string(kSteamProtonRunner)) {
    Options o;
    o.game_dir = game;
    o.tools_dir = tools;
    o.proton = proton;
    o.runner = runner;
    return o;
}

const char* kRuntimeAcf = "\"AppState\"\n{\n\t\"appid\"\t\t\"4183110\"\n\t\"installdir\"\t\t\"SteamLinuxRuntime_4\"\n}";
const char* kManifest = "\"manifest\"\n{\n  \"require_tool_appid\" \"4183110\"\n}\n";

}  // namespace

int main() {
    test::exit_on_crash();
    const fs::path tmp = test::fresh_dir(fs::temp_directory_path() / "sf_setup_test");

    // Rust (launch.rs): library_from_game_dir
    check(library_of(upath("/s/lib/steamapps/common/star conflict")) == upath("/s/lib"), "library_of");
    check(library_of(upath("/s/lib/SteamApps/Common/star conflict/")) == upath("/s/lib"), "library_of: any case, trailing /");
    check(!library_of(upath("/games/star conflict")), "library_of: not in a library");
    check(!library_of(upath("/s/lib/other/common/star conflict")), "library_of: common/ must be inside steamapps/");

    // SteamFix
    check(SteamFix{SteamFix::Kind::InstallGame}.url() == "steam://install/212070", "fix url: install game");
    check(SteamFix{SteamFix::Kind::StartOnce}.url() == "steam://rungameid/212070", "fix url: start once");
    check(SteamFix{SteamFix::Kind::InstallRuntime, 4183110}.url() == "steam://install/4183110", "fix url: runtime");
    check(SteamFix{SteamFix::Kind::StartOnce}.label() == "Start once from Steam", "fix label");

    // Rust (compat.rs): config_info_gives_the_proton_folder
    check(proton_from_config_info("CachyOS-11.0-100\n/usr/share/steam/compatibilitytools.d/proton-cachyos-slr/files/share/"
                                  "fonts/\n/x/files/lib/\n") == upath("/usr/share/steam/compatibilitytools.d/proton-cachyos-slr"),
          "config_info: the Proton folder");
    check(!proton_from_config_info("only-a-version\n"), "config_info: no path line");
    check(!proton_from_config_info("/a/files/x\n"), "config_info: the first line is the version, never a path");

    // Rust (compat.rs): proton_must_name_an_installed_runtime
    const fs::path library = tmp / "library", proton = tmp / "proton-x";
    fs::create_directories(library / "steamapps/common/SteamLinuxRuntime_4");
    fs::create_directories(proton);
    {
        std::optional<SteamFix> fix;
        check(contains(setup_error([&] { runtime_for(proton, {library}); }, &fix), "proton-x doesn't run inside the Steam Linux Runtime") && !fix,
              "no toolmanifest: refused, nothing to install");
        touch(proton / "toolmanifest.vdf", kManifest);
        setup_error([&] { runtime_for(proton, {library}); }, &fix);
        check(fix == SteamFix{SteamFix::Kind::InstallRuntime, 4183110}, "runtime not installed: fix installs app 4183110");
        touch(library / "steamapps/appmanifest_4183110.acf", kRuntimeAcf);
        touch(library / "steamapps/common/SteamLinuxRuntime_4/run");
        check(runtime_for(proton, {library}) == library / "steamapps/common/SteamLinuxRuntime_4/run", "runtime installed: its run script");
    }

    // detect_proton: each way the prefix or Proton can be missing.
    {
        std::optional<SteamFix> fix;
        const fs::path compat = library / "steamapps/compatdata/212070";
        auto detect = [&](std::optional<fs::path> o = std::nullopt) { return detect_proton(tmp / "steamroot", library, {library}, o); };
        check(contains(setup_error([&] { detect(); }, &fix), "hasn't been started from Steam") &&
                  fix == SteamFix{SteamFix::Kind::StartOnce},
              "no prefix: start once from Steam");
        fs::create_directories(compat / "pfx");
        check(contains(setup_error([&] { detect(); }), "hasn't recorded which Proton"), "no config_info");
        touch(compat / "config_info", "v1\n" + ustr(tmp / "gone") + "/files/share/fonts/\n");
        check(contains(setup_error([&] { detect(); }, &fix), "(" + ustr(tmp / "gone") + ") is gone") &&
                  fix == SteamFix{SteamFix::Kind::StartOnce},
              "recorded Proton deleted: start once");
        fix.reset();
        check(contains(setup_error([&] { detect(tmp / "gone"); }, &fix), "has no proton script; choose another") && !fix,
              "override without a proton script: no Steam fix");
        touch(proton / "proton");
        touch(compat / "config_info", "v1\n" + ustr(proton) + "/files/share/fonts/\n");
        Proton p = detect();
        check(p.dir == proton && p.compat_data == compat && p.steam_root == tmp / "steamroot" &&
                  p.runtime == library / "steamapps/common/SteamLinuxRuntime_4/run",
              "detect: the recorded Proton, its runtime, the prefix");
        const fs::path other = tmp / "proton-y";
        touch(other / "proton");
        touch(other / "toolmanifest.vdf", kManifest);
        check(detect(other).dir == other, "detect: an override wins over config_info");
    }

    // proton_candidates: Proton builds in libraries and compatibilitytools.d, sorted, each once.
    {
        touch(library / "steamapps/common/Proton 9.0/proton");
        touch(tmp / "root/compatibilitytools.d/GE-Proton/proton");
        fs::create_directories(tmp / "root/compatibilitytools.d/not-proton");
        auto c = proton_candidates({tmp / "root"}, {library, library});
        bool ours_only = true;
        for (const auto& p : c)
            if (ustr(p).rfind("/usr/share/", 0) != 0 && p != library / "steamapps/common/Proton 9.0" &&
                p != tmp / "root/compatibilitytools.d/GE-Proton")
                ours_only = false;
        check(ours_only && std::count(c.begin(), c.end(), library / "steamapps/common/Proton 9.0") == 1 &&
                  std::count(c.begin(), c.end(), tmp / "root/compatibilitytools.d/GE-Proton") == 1 &&
                  std::is_sorted(c.begin(), c.end()),
              "proton_candidates");
    }

    // inject_error: the last "error: " line of inject.log.
    touch(tmp / "prof/inject.log", "profile x\r\nerror: first\r\nstarting\nerror: the real one\n");
    check(inject_error(tmp / "prof") == std::string("the real one"), "inject_error: the last error line");
    check(!inject_error(tmp / "nowhere"), "inject_error: no log");

#ifndef _WIN32
    // Rust (launch.rs): wine_paths
    check(tool_path(upath("/home/user/star conflict")) == "Z:\\home\\user\\star conflict", "tool_path: Z:");
    for (const char* bad : {"relative", "/a/../b", "/a:b", "/a\\b"}) {
        bool threw = false;
        try {
            tool_path(upath(bad));
        } catch (const std::runtime_error&) {
            threw = true;
        }
        check(threw, std::string("tool_path refuses ") + bad);
    }
    // runtime_hides: what pressure-vessel mounts its own files over.
    for (const char* hidden : {"/usr/lib/STAR FORGE Launcher/tools", "/usr", "/etc/x", "/lib64/x", "/usr/../usr/lib"})
        check(runtime_hides(hidden), std::string("runtime_hides: ") + hidden);
    for (const char* shown : {"/opt/star-forge/tools", "/home/u/usr", "/", "/tmp/.mount_x/usr/lib/tools", "relative/usr"})
        check(!runtime_hides(shown), std::string("runtime_hides: not ") + shown);
#endif

    // find_game / resolve on a fake install: <library>/steamapps/common/star conflict.
#ifndef _WIN32
    // resolve() finds the runtime through Steam's own libraries: a fake Steam in a fake HOME,
    // listing the library, so this machine's Steam (or none, on CI) doesn't count.
    touch(tmp / "home/.local/share/Steam/steamapps/libraryfolders.vdf",
          "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" + ustr(library) + "\"\n\t}\n}\n");
    set_env("HOME", ustr(tmp / "home"));
#endif
    const fs::path game = library / "steamapps/common/star conflict";
    const fs::path tools = tmp / "tools";
    fs::create_directories(game / "data");
    touch(game / "win64/game.exe", "MZ");
    fs::create_directories(game / "win64");
    {
        check(contains(setup_error([&] { find_game(opts(tmp)); }), "does not look like a Star Conflict install"), "find_game: not a game folder");
        check(find_game(opts(game)) == game, "find_game: explicit folder");
        set_env("STAR_FORGE_GAME_DIR", ustr(game));
        check(find_game(opts()) == game, "find_game: STAR_FORGE_GAME_DIR");
        set_env("STAR_FORGE_GAME_DIR", std::nullopt);

        touch(tools / "sc-inject.exe");
        check(contains(setup_error([&] { resolve(opts(game, tools)); }), "sc_agent.dll is missing from " + ustr(tools)),
              "resolve: a missing tool is named");
        touch(tools / "sc_agent.dll");
        Setup s = resolve(opts(game, tools));
        check(s.game_dir == game && s.tools_dir == tools && s.game_exe() == game / "win64" / "game.exe", "resolve: folders");
#ifdef _WIN32
        check(!s.proton, "resolve: no Proton on Windows");
#else
        check(s.proton && s.proton->dir == proton, "resolve: Proton from the game's prefix");
        set_env("STAR_FORGE_PROTON", ustr(tmp / "proton-y"));
        check(resolve(opts(game, tools)).proton->dir == tmp / "proton-y", "resolve: STAR_FORGE_PROTON");
        check(resolve(opts(game, tools, proton)).proton->dir == proton, "resolve: an explicit option beats the variable");
        set_env("STAR_FORGE_PROTON", std::nullopt);
        check(contains(setup_error([&] { resolve(opts(tmp / "elsewhere", tools)); }), "does not look like"), "resolve: bad game folder");
        fs::create_directories(tmp / "loose/data");
        touch(tmp / "loose/win64/game.exe");
        check(contains(setup_error([&] { resolve(opts(tmp / "loose", tools)); }), "not inside a Steam library"),
              "resolve: a game outside a Steam library has no known prefix");
#endif
    }

    // command(): what starts sc-inject for a profile.
    {
        const fs::path profile = tmp / "profiles/starfury";
        fs::create_directories(profile);
        Setup s = resolve(opts(game, tools));
#ifdef _WIN32
        Command c = command(s, profile, {"-x"});
        check(c.program == tools / "sc-inject.exe", "command: sc-inject.exe itself");
        check(c.args == std::vector<std::string>{"--game", ustr(game / "win64" / "game.exe"), "--profile", ustr(fs::canonical(profile)),
                                                 "--dll", ustr(tools / "sc_agent.dll"), "--", "-x"},
              "command: arguments");
#else
        // The Steam overlay, as Steam sets it up.
        touch(tmp / "steamroot/ubuntu12_64/gameoverlayrenderer.so");
        s.proton->steam_root = tmp / "steamroot";
        set_env("PRESSURE_VESSEL_FILESYSTEMS_RW", "/already");
        const std::string overlay = ustr(tmp / "steamroot/ubuntu12_64/gameoverlayrenderer.so");
        set_env("LD_PRELOAD", "/x.so " + overlay);  // already there: not added twice
        Command c = command(s, profile, {"-x"});
        const fs::path canon = fs::canonical(profile);
        check(c.program == s.proton->runtime, "command: the runtime's run script");
        std::vector<std::string> want{"--", ustr(proton / "proton"), "waitforexitandrun", ustr(tools / "sc-inject.exe"),
                                      "--game", "Z:" + ustr(game / "win64/game.exe"), "--profile", "Z:" + ustr(canon),
                                      "--dll", "Z:" + ustr(tools / "sc_agent.dll"), "--", "-x"};
        for (auto& a : want)
            if (a.rfind("Z:", 0) == 0) std::replace(a.begin(), a.end(), '/', '\\');
        check(c.args == want, "command: runtime -- proton waitforexitandrun sc-inject.exe <args> -- <game args>");
        check(c.env_value("PRESSURE_VESSEL_FILESYSTEMS_RW") ==
                  "/already:" + ustr(s.proton->compat_data) + ":" + ustr(game) + ":" + ustr(canon) + ":" + ustr(tools),
              "command: the prefix, game, profile and tools are shared read-write with the container");
        check(c.env_value("STEAM_COMPAT_DATA_PATH") == ustr(s.proton->compat_data) &&
                  c.env_value("STEAM_COMPAT_CLIENT_INSTALL_PATH") == ustr(tmp / "steamroot") &&
                  c.env_value("SteamAppId") == std::string("212070") && c.env_value("SteamGameId") == std::string("212070") &&
                  c.env_value("STEAM_COMPAT_APP_ID") == std::string("212070"),
              "command: Steam's variables");
        check(c.env_value("LD_PRELOAD") == "/x.so " + overlay && c.env_value("ENABLE_VK_LAYER_VALVE_steam_overlay_1") == std::string("1"),
              "command: overlay preloaded once, Vulkan layer on");
        set_env("LD_PRELOAD", std::nullopt);
        check(command(s, profile, {}).env_value("LD_PRELOAD") == overlay, "command: overlay added to an empty LD_PRELOAD");
        // Started by Steam as a non-Steam game: the shortcut's id stays for the overlay and gamescope.
        set_env("SteamGameId", "3141592653");
        {
            const Command shortcut = command(s, profile, {});
            check(shortcut.env_value("SteamGameId") == std::string("3141592653") &&
                      shortcut.env_value("SteamOverlayGameId") == std::string("3141592653") &&
                      shortcut.env_value("SteamAppId") == std::string("212070") &&
                      shortcut.env_value("STEAM_COMPAT_APP_ID") == std::string("212070"),
                  "command: a Steam shortcut's SteamGameId is kept, the game's appid still given");
        }
        set_env("SteamGameId", std::nullopt);
        check(command(s, profile, {}).args.back() == "Z:" + [&] { auto d = ustr(tools / "sc_agent.dll"); std::replace(d.begin(), d.end(), '/', '\\'); return d; }(),
              "command: no game args, no --");
        set_env("PRESSURE_VESSEL_FILESYSTEMS_RW", std::nullopt);
        // A ':' would split PRESSURE_VESSEL_FILESYSTEMS_RW.
        Setup bad = s;
        bad.tools_dir = tmp / "a:b";
        bool threw = false;
        try {
            command(bad, profile, {});
        } catch (const std::runtime_error& e) {
            threw = contains(e.what(), ":");
        }
        check(threw, "command: a shared path with ':' is refused");
#endif
        check(c.cwd == game / "win64", "command: runs in the game's win64 folder");
        // The profile path is made absolute and canonical first: "..", a relative path or a
        // symlink would otherwise reach the injector (and wine) as given.
        fs::create_directories(tmp / "profiles/other");
        bool canonical = false;
        try {
            Command c2 = command(s, tmp / "profiles/other/../starfury", {});
            canonical = std::find(c2.args.begin(), c2.args.end(), tool_path(fs::canonical(profile))) != c2.args.end();
        } catch (const std::exception& e) {
            std::printf("  %s\n", e.what());
        }
        check(canonical, "command: the profile path is canonical");
        bool missing = false;
        try {
            command(s, tmp / "no-such-profile", {});
        } catch (const std::runtime_error&) {
            missing = true;
        }
        check(missing, "command: a missing profile folder is an error");
    }

    // server_command(): sc-server.exe for a server install, with `proton run` on Linux.
    {
        const Setup s = resolve(opts(game, tools));
        const fs::path cloud = tmp / "server/cloud";
        fs::create_directories(cloud);
        check(contains(error_of([&] { server_command(s, cloud, "run"); }), "sc-server.exe is missing"),
              "server_command: needs sc-server.exe");
        touch(tools / "sc-server.exe");
        const Command c = server_command(s, cloud, "stop");
        check(c.cwd == cloud, "server_command: runs in the cloud folder");
#ifdef _WIN32
        check(c.program == tools / "sc-server.exe" && c.args == std::vector<std::string>{"stop", tool_path(fs::absolute(cloud))},
              "server_command: sc-server.exe stop <cloud>");
#else
        std::string z = "Z:" + ustr(cloud);
        std::replace(z.begin(), z.end(), '/', '\\');
        check(c.program == s.proton->runtime &&
                  c.args == std::vector<std::string>{"--", ustr(proton / "proton"), "run", ustr(tools / "sc-server.exe"), "stop", z},
              "server_command: runtime -- proton run sc-server.exe stop Z:<cloud>");
        check(c.env_value("PRESSURE_VESSEL_FILESYSTEMS_RW") ==
                  ustr(s.proton->compat_data) + ":" + ustr(game) + ":" + ustr(cloud) + ":" + ustr(tools),
              "server_command: the prefix, game, cloud folder and tools are shared");
        check(c.env_value("STEAM_COMPAT_DATA_PATH") == ustr(s.proton->compat_data) && !c.env_value("ENABLE_VK_LAYER_VALVE_steam_overlay_1"),
              "server_command: the game's prefix, no overlay");
#endif
    }

#ifndef _WIN32
    // Wine-style runners: `<runner> exe args` in a prefix of their own, nothing from Steam.
    {
        const fs::path bin = tmp / "bin";
        touch(bin / "wine");
        touch(bin / "umu-run");
        const auto old_path = get_env("PATH");
        set_env("PATH", ustr(bin) + ":/usr/bin");
        set_env("STAR_FORGE_DATA_DIR", ustr(tmp / "data"));

        check(find_on_path("wine") == bin / "wine", "find_on_path: on PATH");
        check(!find_on_path("no-such-wine"), "find_on_path: missing");
        check(find_on_path(ustr(bin / "umu-run")) == bin / "umu-run" && !find_on_path(ustr(tmp / "nope/wine")),
              "find_on_path: a path is checked as given");
        check(contains(setup_error([&] { wine_setup("no-such-wine", std::nullopt); }), "was not found on PATH"),
              "wine_setup: a missing runner is named");

        Options o = opts(tmp / "loose", tools);
        o.runner = "wine";
        Setup s = resolve(o);
        check(!s.proton && s.wine && s.wine->program == bin / "wine" && !s.wine->umu &&
                  s.wine->prefix == tmp / "data/prefixes/wine" && fs::is_directory(tmp / "data/prefixes/wine"),
              "resolve: wine, the launcher's own prefix, a game outside any Steam library");
        set_env("STAR_FORGE_RUNNER", "umu-run");
        set_env("STAR_FORGE_WINEPREFIX", ustr(tmp / "pfx"));
        Setup u = resolve(opts(game, tools, {}, std::nullopt));
        check(u.wine && u.wine->umu && !u.wine->proton && u.wine->prefix == tmp / "pfx",
              "resolve: STAR_FORGE_RUNNER and STAR_FORGE_WINEPREFIX; umu-run by name is a Wine-style command");
        set_env("STAR_FORGE_RUNNER", "steam-proton");
        check(resolve(opts(game, tools, {}, std::nullopt)).proton.has_value(), "resolve: STAR_FORGE_RUNNER=steam-proton");
        set_env("STAR_FORGE_RUNNER", std::nullopt);
        set_env("STAR_FORGE_WINEPREFIX", std::nullopt);

        const fs::path profile = fs::canonical(tmp / "profiles/starfury");
        Command c = command(s, profile, {"-x"});
        std::vector<std::string> want{ustr(tools / "sc-inject.exe"), "--game", tool_path(tmp / "loose/win64/game.exe"),
                                      "--profile", tool_path(profile), "--dll", tool_path(tools / "sc_agent.dll"), "--", "-x"};
        check(c.program == bin / "wine" && c.args == want, "command: wine sc-inject.exe <args> -- <game args>");
        check(c.env_value("WINEPREFIX") == ustr(tmp / "data/prefixes/wine") && !c.env_value("STEAM_COMPAT_DATA_PATH") &&
                  !c.env_value("ENABLE_VK_LAYER_VALVE_steam_overlay_1") && !c.env_value("GAMEID"),
              "command: WINEPREFIX, nothing of Steam's");
        check(command(u, profile, {}).env_value("GAMEID") == std::string("umu-212070"), "command: umu-run gets a GAMEID");
        set_env("GAMEID", "umu-custom");
        check(!command(u, profile, {}).env_value("GAMEID").has_value() ||
                  command(u, profile, {}).env_value("GAMEID") == std::string("umu-custom"),
              "command: the player's GAMEID is kept");
        set_env("GAMEID", std::nullopt);

        const Command sc = server_command(s, tmp / "server/cloud", "run");
        check(sc.program == bin / "wine" && sc.args.front() == ustr(tools / "sc-server.exe") &&
                  sc.env_value("WINEPREFIX") == ustr(tmp / "data/prefixes/wine"),
              "server_command: wine sc-server.exe in the same prefix");

        // umu, the default: tools/umu-run (else PATH), umu's own Proton unless one is chosen, its own prefix.
        touch(bin / "python3");
        touch(tools / "umu-run");
        Setup m = resolve(opts(game, tools, {}, std::nullopt));
        check(m.wine && m.wine->umu && m.wine->program == tools / "umu-run" && !m.wine->proton &&
                  m.wine->prefix == tmp / "data/prefixes/umu",
              "resolve: umu by default, the bundled umu-run, umu's own Proton (not Steam's)");
        check(steam_compat_data() == library / "steamapps/compatdata/212070", "steam_compat_data");
        Command mc = command(m, profile, {});
        check(mc.program == tools / "umu-run" && mc.args.front() == ustr(tools / "sc-inject.exe") &&
                  !mc.env_value("PROTONPATH") && mc.env_value("GAMEID") == std::string("umu-212070") &&
                  mc.env_value("WINEPREFIX") == ustr(tmp / "data/prefixes/umu"),
              "command: umu-run sc-inject.exe with GAMEID and WINEPREFIX, no PROTONPATH");
        const Setup chosen = resolve(opts(game, tools, proton, std::nullopt));
        check(chosen.wine->proton == proton && command(chosen, profile, {}).env_value("PROTONPATH") == ustr(proton),
              "resolve: a chosen Proton becomes PROTONPATH");
        set_env("PROTONPATH", "GE-Proton");
        check(!command(chosen, profile, {}).env_value("PROTONPATH") ||
                  command(chosen, profile, {}).env_value("PROTONPATH") == std::string("GE-Proton"),
              "command: the player's PROTONPATH is kept");
        set_env("PROTONPATH", std::nullopt);
        check(contains(setup_error([&] { resolve(opts(game, tools, tmp / "gone", std::nullopt)); }), "no toolmanifest.vdf"),
              "resolve: umu refuses a Proton without a toolmanifest");
        fs::remove(tools / "umu-run");
        check(resolve(opts(game, tools, {}, std::string("umu"))).wine->program == bin / "umu-run", "resolve: umu-run from PATH");
        set_env("PATH", ustr(bin));  // nothing of this machine's
        fs::remove(bin / "umu-run");
        check(contains(setup_error([&] { resolve(opts(game, tools, {}, std::string("umu"))); }), "umu-launcher) is missing"),
              "resolve: no umu-run anywhere");
        touch(bin / "umu-run");
        fs::remove(bin / "python3");
        check(contains(setup_error([&] { resolve(opts(game, tools, {}, std::string("umu"))); }), "needs Python 3"),
              "resolve: umu without python3");
        touch(bin / "python3");
        set_env("PATH", ustr(bin) + ":/usr/bin");

        // The first launch in a umu or Wine prefix brings the game's data over from Steam's.
        const fs::path steam_data = library / "steamapps/compatdata/212070/pfx/drive_c/users/steamuser/AppData/Local/Targem/StarConflict";
        touch(steam_data / "user_config.xml", "<steam/>");
        touch(steam_data / "profiles/0001.prfl", "p");
        const fs::path umu_data = tmp / "data/prefixes/umu/drive_c/users/steamuser/AppData/Local/Targem/StarConflict";
        check(profiles::shared_local_appdata(m) == tmp / "data/prefixes/umu/drive_c/users/steamuser/AppData/Local",
              "shared_local_appdata: steamuser under umu, before the prefix exists");
        check(profiles::import_steam_data(m, steam_compat_data()) == steam_data &&
                  test::read_file(umu_data / "user_config.xml") == "<steam/>" && fs::exists(umu_data / "profiles/0001.prfl"),
              "import_steam_data: Steam's game data copied into the new prefix");
        touch(umu_data / "user_config.xml", "<mine/>");
        check(!profiles::import_steam_data(m, steam_compat_data()) && test::read_file(umu_data / "user_config.xml") == "<mine/>",
              "import_steam_data: only once");
        Setup fresh = m;
        fresh.wine->prefix = tmp / "fresh";
        fs::create_directories(tmp / "fresh");
        check(!profiles::import_steam_data(fresh, std::nullopt) && fs::exists(tmp / "fresh" / profiles::kImportMarker),
              "import_steam_data: no Steam prefix, nothing to copy, not looked for again");

        set_env("PATH", old_path);
        set_env("STAR_FORGE_DATA_DIR", std::nullopt);
    }
#endif

    return test::failures ? 1 : 0;
}
