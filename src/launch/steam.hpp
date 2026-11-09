#pragma once

// Port of sc-launch's steam.rs: finding Steam, its library folders and installed apps.

#include <optional>
#include <string>
#include <vector>

#include "launch/paths.hpp"

namespace sf::launch::steam {

inline constexpr const char* kGameAppid = "212070";  // Star Conflict's Steam app id

// Every value following a quoted `key` (ignoring ASCII case) in a text VDF file (Steam's
// "key" "value" format), with \\ and \" unescaped. Good enough for the flat lookups needed here;
// not a full VDF parser.
std::vector<std::string> vdf_values(const std::string& text, const std::string& key);

// Likely Steam install folders on this OS, existing ones (with a steamapps folder) only, each
// once even when reached through a symlink (~/.steam/steam usually points at
// ~/.local/share/Steam). Windows: the registry's SteamPath, then Program Files.
std::vector<fs::path> steam_roots();

// All library folders known to the given Steam roots (roots included, each once), from
// steamapps/libraryfolders.vdf.
std::vector<fs::path> libraries(const std::vector<fs::path>& roots);

struct App {
    fs::path library;  // the library folder it's in
    fs::path dir;      // its install folder (<library>/steamapps/common/<installdir>)
};
// Where an app is installed, from its appmanifest_<appid>.acf in the first library that has it.
std::optional<App> find_app(const std::vector<fs::path>& libraries, const std::string& appid);

// True if `dir` looks like a Star Conflict install: win64/game.exe and data/.
bool is_game_dir(const fs::path& dir);

}  // namespace sf::launch::steam
