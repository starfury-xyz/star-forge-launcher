#pragma once

// Port of sc-launch's profiles.rs and state.rs: where profiles live, how they're named, listing,
// moving and migrating them; and keeping the player's game settings the same across profiles.
//
// A profile is a folder under the profiles folder holding a server pack (pack.json, resources/,
// files/) and the player's local.json. Errors are std::runtime_error with a message for the player.

#include <optional>
#include <string>
#include <vector>

#include "launch/launch.hpp"
#include "pack/pack.hpp"

namespace sf::launch::profiles {

// Per-user data folder for the launcher: STAR_FORGE_DATA_DIR, else <platform data>/star-forge
// (%APPDATA% on Windows, $XDG_DATA_HOME or ~/.local/share on Linux). An old "sc-launcher" folder
// there is renamed on first use.
std::optional<fs::path> data_dir();
// The profiles folder under the data folder, used unless the player picks another.
std::optional<fs::path> standard_profiles_dir();
// The profiles folder: STAR_FORGE_PROFILES_DIR, else the one picked in setup, else the standard one.
std::optional<fs::path> default_profiles_dir();
// Remembers `dir` as the profiles folder; nullopt goes back to the standard one.
void set_profiles_location(const std::optional<fs::path>& dir);
// Moves every profile in `from` into `to`. Stops at the first profile that can't move (ones already
// moved stay moved); never overwrites a profile. Returns how many moved.
size_t move_profiles(const fs::path& from, const fs::path& to);

// A profile name is a single folder name: no separators, no dot names.
bool valid_name(const std::string& name);
// Lower-case letters, digits and dashes (at most 40), for folder names and pack ids; "profile" if nothing is left.
std::string slug(const std::string& name);
// A free folder name under `root` based on `name`: slug, then slug-2, slug-3, ...
std::string free_name(const fs::path& root, const std::string& name);
// Whether `dir` looks like a profile (possibly one still to be converted from profile.toml).
bool is_profile(const fs::path& dir);

struct Entry {
    std::string name;
    fs::path dir;
    std::optional<pack::Profile> profile;  // set when it loads...
    std::string error;                     // ... otherwise why not
    std::optional<pack::Pack> pack;        // the pack as written, when it parses
};

pack::Pack read_pack(const fs::path& dir);
pack::Local read_local(const fs::path& dir);  // a missing local.json is the default
// Writes through a temporary file (.<name>.tmp, then renamed) so a crash never leaves half a file.
void write_atomic(const fs::path& path, const std::string& text);
void write_local(const fs::path& dir, const pack::Local& local);
void write_pack(const fs::path& dir, const pack::Pack& pack);  // validated first
// Loads a profile, converting an old profile.toml first.
pack::Profile load(const fs::path& dir);
// Every profile in `profiles_dir`, sorted by name.
std::vector<Entry> list(const fs::path& profiles_dir);
// The profile folder holding the pack with this id, if any.
std::optional<fs::path> find_pack(const fs::path& profiles_dir, const std::string& id);

// --- state.rs: the game's per-user data, per profile ---
//
// The agent gives each profile its own %LOCALAPPDATA% (<profile>/state/AppData/Local), so one
// server's store cache, notes and local database never show up in another's sessions. The
// player's graphics settings and key bindings (pack::kSharedStateFiles) should still follow them
// everywhere, so they're synced with the game's normal folder before and after each session,
// newest copy wins.

// The game's normal %LOCALAPPDATA%: inside the Proton or Wine prefix on Linux and macOS.
std::optional<fs::path> shared_local_appdata(const Setup& setup);
// Where the game keeps its per-user data inside %LOCALAPPDATA%.
inline constexpr const char* kGameDataFolder = "Targem/StarConflict";
// In a umu or Wine prefix: the Steam import below was done.
inline constexpr const char* kImportMarker = ".starforge-steam-import";
// Once per umu or Wine prefix: copies the game's data (settings, key bindings, notes, the local
// database) from Steam's prefix (`steam_compat`, see steam_compat_data()) into it, unless the
// prefix already has some. Returns the folder copied from, if any. Throws std::runtime_error.
std::optional<fs::path> import_steam_data(const Setup& setup, const std::optional<fs::path>& steam_compat);
// The profile's own %LOCALAPPDATA%.
fs::path profile_local_appdata(const fs::path& profile_dir);
// Brings the profile's and the shared copies of the settings files up to date with each other.
// Call before the game starts and after it exits. Does nothing for profiles that share the
// normal folder anyway (own_game_data off) or when `shared` is unknown.
void sync_shared(const fs::path& profile_dir, const pack::Profile& profile, const std::optional<fs::path>& shared);
// sync_shared for every profile that keeps its own game data (run once when the launcher starts).
void sync_all_shared(const fs::path& profiles_dir, const std::optional<fs::path>& shared);

}  // namespace sf::launch::profiles
