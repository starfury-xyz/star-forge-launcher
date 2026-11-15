#pragma once

// Port of sc-launch's launch.rs and compat.rs: works out the game, tools and (on Linux) Proton
// setup, and builds the command that starts sc-inject.exe for a profile.
//
// Linux runs our Windows tools through a runner:
// - "umu" (the default): umu-launcher (umu-run, bundled in tools/ by the packages, else from
//   PATH) with Proton inside the Steam Linux Runtime, both of which umu fetches itself. Proton is
//   the one chosen in Settings, else the one Steam last ran the game with, else umu's own.
// - "steam-proton" (deprecated, to be removed after 0.4): Steam's own setup, below.
// - Any other name or path: a Wine-style command, below.
//
// steam-proton: the game's own Steam prefix (compatdata/212070) and the Proton build that last
// ran it are reused, so settings, the local Mongo data and the prefix version stay the same as a
// normal Steam launch. Proton always runs inside the Steam Linux Runtime its build asks for
// (require_tool_appid in its toolmanifest.vdf), through the runtime's `run` script, as Steam does.
// A build that names no runtime, or whose runtime isn't installed, is refused rather than run
// against the host's libraries; the error says what to do in Steam.
//
// umu and Wine-style runners (any program run as `<runner> program.exe args` with WINEPREFIX
// set: wine, a Wine build, umu-run, a wrapper script) use a prefix of their own (by default
// <data dir>/prefixes/<umu|wine>), need nothing from Steam, and so also work with a game outside
// a Steam library, without Steam, and (Wine) on macOS.

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "launch/paths.hpp"
#include "launch/process.hpp"

namespace sf::launch {

inline constexpr const char* kInjectExe = "sc-inject.exe";
inline constexpr const char* kAgentDll = "sc_agent.dll";
inline constexpr const char* kServerExe = "sc-server.exe";  // starts and stops a server stack

// Something the player fixes in Steam, offered as a button next to the error.
struct SteamFix {
    enum class Kind {
        InstallGame,     // Star Conflict isn't installed
        StartOnce,       // installed but never started from Steam (on Linux: no Proton prefix yet)
        InstallRuntime,  // the Proton build needs a Steam Linux Runtime (appid) that isn't installed
    } kind;
    uint32_t appid = 0;  // InstallRuntime only

    std::string url() const;    // steam://install/212070, steam://rungameid/212070, ...
    std::string label() const;  // the button's text
    bool operator==(const SteamFix& o) const { return kind == o.kind && appid == o.appid; }
};

// Why the game can't be started yet, and what in Steam fixes it, if anything.
struct SetupError : std::runtime_error {
    std::optional<SteamFix> fix;
    explicit SetupError(const std::string& message, std::optional<SteamFix> f = std::nullopt)
        : std::runtime_error(message), fix(f) {}
};

struct Options {
    std::optional<fs::path> game_dir;   // holding win64\ and data\; found through Steam if unset
    std::optional<fs::path> tools_dir;  // sc-inject.exe + sc_agent.dll; defaults to this program's folder
    std::optional<fs::path> proton;     // Proton build for umu and steam-proton, instead of the one Steam last used
    // How to run our Windows tools (Linux, macOS): "umu" (the Linux default), "steam-proton", or a
    // Wine-style command: a name looked up on PATH or a path (the default on macOS is "wine").
    std::optional<std::string> runner;
    std::optional<fs::path> wine_prefix;  // WINEPREFIX for umu and Wine-style runners
};

inline constexpr const char* kUmuRunner = "umu";
inline constexpr const char* kSteamProtonRunner = "steam-proton";  // deprecated
inline constexpr const char* kUmuExe = "umu-run";                   // bundled in tools/

// umu or a Wine-style runner: `program exe args` with WINEPREFIX=prefix.
struct Wine {
    fs::path program;  // umu-run, or the command as found on PATH
    fs::path prefix;
    bool umu = false;               // umu-run: also given GAMEID (umu-212070) unless the environment sets one
    std::optional<fs::path> proton;  // umu: PROTONPATH, unless the environment sets one; unset = umu's own
};

struct Proton {
    fs::path dir;          // folder holding the `proton` script
    fs::path runtime;      // the Steam Linux Runtime `run` script the build asks for
    fs::path compat_data;  // STEAM_COMPAT_DATA_PATH (the folder that holds pfx/)
    fs::path steam_root;   // STEAM_COMPAT_CLIENT_INSTALL_PATH
};

struct Setup {
    fs::path game_dir;
    fs::path tools_dir;
    std::optional<Proton> proton;  // Linux, with Steam's Proton
    std::optional<Wine> wine;      // with a Wine-style runner instead
    fs::path game_exe() const { return game_dir / "win64" / "game.exe"; }
};

// --- compat (Proton) ---

// Proton folder recorded in a prefix's config_info: line 2 is a path inside the build, e.g.
// <proton>/files/share/fonts/.
std::optional<fs::path> proton_from_config_info(const std::string& text);
// Every Proton build under the Steam libraries and the usual tool folders, sorted.
std::vector<fs::path> proton_candidates(const std::vector<fs::path>& roots, const std::vector<fs::path>& libraries);
// The runtime `run` script a Proton build requires via require_tool_appid. Throws SetupError.
fs::path runtime_for(const fs::path& proton_dir, const std::vector<fs::path>& libraries);
// The Proton setup for the game's prefix in `game_library`. Throws SetupError.
Proton detect_proton(const fs::path& steam_root, const fs::path& game_library, const std::vector<fs::path>& libraries,
                     const std::optional<fs::path>& proton_override);

// --- Wine-style runners ---

// The runner Options ask for, after STAR_FORGE_RUNNER and the platform default.
std::string runner_name(const Options& opts);
// The game's Steam prefix folder (steamapps/compatdata/212070, holding pfx/) in any Steam library.
std::optional<fs::path> steam_compat_data();
// The Proton Steam last ran the game with, if it is still there and umu can use it (it has a
// toolmanifest.vdf).
std::optional<fs::path> steam_recorded_proton();
// The umu setup: tools/umu-run, else umu-run on PATH; PROTONPATH from `proton`, else
// steam_recorded_proton(), else umu's own; `prefix` default <data dir>/prefixes/umu (created).
// Throws SetupError.
Wine umu_setup(const fs::path& tools_dir, const std::optional<fs::path>& prefix, const std::optional<fs::path>& proton);
// `name` on the host's PATH (an AppImage's own folders don't count), or `name` itself if it holds
// a '/' and is a file. nullopt if not found.
std::optional<fs::path> find_on_path(const std::string& name);
// The Wine setup for runner `name` and `prefix` (default <data dir>/prefixes/wine, created if
// missing). Throws SetupError.
Wine wine_setup(const std::string& name, const std::optional<fs::path>& prefix);
// Wine-style runners found on PATH, for the settings page.
std::vector<std::string> wine_candidates();

// --- launch ---

// <library> for a game folder at <library>/steamapps/common/<name>.
std::optional<fs::path> library_of(const fs::path& game_dir);
// The game install folder alone: --game-dir, STAR_FORGE_GAME_DIR, or Steam. Throws SetupError.
fs::path find_game(const Options& opts);
// Everything needed to start the game; options fall back to STAR_FORGE_* variables. Throws SetupError.
Setup resolve(const Options& opts);
// Why the injector couldn't start the game, from the profile's inject.log (its last "error: " line).
std::optional<std::string> inject_error(const fs::path& profile_dir);
// A path as the injector (a Windows program) sees it: unchanged on Windows; under wine Z:\...,
// which needs an absolute path without ':', '\' or "..". Throws std::runtime_error.
std::string tool_path(const fs::path& path);
// The command that runs sc-inject.exe for the profile in `profile_dir`. Throws std::runtime_error.
Command command(const Setup& setup, const fs::path& profile_dir, const std::vector<std::string>& game_args);
// The command that runs `sc-server.exe <verb> <cloud>` (verb "run" or "stop") for a server
// install: in the game's prefix on Linux and macOS (Proton's, or the Wine-style runner's), so the
// servers share the client's local database and AppData. Throws std::runtime_error.
Command server_command(const Setup& setup, const fs::path& cloud_dir, const std::string& verb);

}  // namespace sf::launch
