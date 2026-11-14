#pragma once

// Port of sc-launch's env.rs: machine-specific overrides, read from STAR_FORGE_* environment
// variables. Explicit settings (command-line flags, launcher preferences) win over these, and
// these win over auto-detection. See .env.example in the star-forge repo.

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "launch/paths.hpp"

namespace sf::launch::env {

inline constexpr const char* kGameDir = "STAR_FORGE_GAME_DIR";        // Star Conflict install (win64\, data\)
inline constexpr const char* kToolsDir = "STAR_FORGE_TOOLS_DIR";      // sc-inject.exe + sc_agent.dll
inline constexpr const char* kServerProxy = "STAR_FORGE_SERVER_PROXY";  // the x86 server proxy's libcrypto-1_1_tg.dll
inline constexpr const char* kProton = "STAR_FORGE_PROTON";           // Proton build folder (Linux)
inline constexpr const char* kDataDir = "STAR_FORGE_DATA_DIR";        // preferences, profiles, launch log
inline constexpr const char* kProfilesDir = "STAR_FORGE_PROFILES_DIR";  // if not the one chosen in setup

// A path variable, ignoring unset and empty values.
std::optional<fs::path> path(const char* name);

// Development builds (configured with -DSF_DOTENV=ON) read <repo>/.env, so overrides apply however
// the program is started; variables already set are kept. Other builds never read a .env file.
// Call before starting any threads.
void load_dotenv();

// KEY=value lines; blank lines and # comments are skipped, an optional "export " prefix and
// matching surrounding quotes are dropped. Lines without '=' are ignored.
std::vector<std::pair<std::string, std::string>> parse_dotenv(const std::string& text);

// Changes to give a program we start the host's environment rather than an AppImage's: its AppRun
// points library, Python, Perl and GTK search paths into the image (LD_LIBRARY_PATH, PYTHONHOME,
// GTK_PATH, ...). Inherited by Proton, its Python can't find its standard library and the game
// never starts. Each change is a variable and its new value, or nullopt to remove it.
using Changes = std::vector<std::pair<std::string, std::optional<std::string>>>;

// For this process: nothing unless it runs from an AppImage (APPIMAGE and APPDIR set).
Changes host_env();

// The rule itself, for `vars` and the image's mount folder `appdir`: path lists lose their
// entries inside it (removed when nothing is left), other values pointing into it are removed,
// and so is the GTK theme AppRun forces.
Changes host_env_changes(const std::vector<std::pair<std::string, std::string>>& vars, std::string appdir);

}  // namespace sf::launch::env
