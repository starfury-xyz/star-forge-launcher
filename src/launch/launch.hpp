#pragma once

// Port of sc-launch's launch.rs and compat.rs: works out the game, tools and (on Linux) Proton
// setup, and builds the command that starts sc-inject.exe for a profile.
//
// Linux: by default the game's own Steam prefix (compatdata/212070) and the Proton build that last
// ran it are reused, so settings, the local Mongo data and the prefix version stay the same as a
// normal Steam launch. Proton always runs inside the Steam Linux Runtime its build asks for
// (require_tool_appid in its toolmanifest.vdf), through the runtime's `run` script, as Steam does.
// A build that names no runtime, or whose runtime isn't installed, is refused rather than run
// against the host's libraries; the error says what to do in Steam.

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
    std::optional<fs::path> proton;     // Proton build to use instead of the one that last ran the prefix
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
    std::optional<Proton> proton;  // Linux only
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
// install: in the game's Proton prefix on Linux, so the servers share the client's local
// database and AppData. Throws std::runtime_error.
Command server_command(const Setup& setup, const fs::path& cloud_dir, const std::string& verb);

}  // namespace sf::launch
