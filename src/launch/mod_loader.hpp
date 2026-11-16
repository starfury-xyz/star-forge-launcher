#pragma once

// The mod loader (docs/mods.md): the mods in a profile's mods/ folder, in the player's order,
// turned into what the agent serves at launch. Portable; runs in the launcher before every start.
//
// <profile>/mods/
//   retro-layout-1.0.0.zip     the player's mods: zips or folders, each with a mod.json
//   deck-glyphs/
//   .cache/<id>-<hash>/        unpacked zips (kept until the zip changes or goes away)
//   .merged/                   game files with every mod's data patches applied, rebuilt each launch
//   .layers                    what the agent reads: the folders to serve, in order, and core patches
//   .loaded.json               what happened, for the launcher's Mods page
//
// Layers, searched first to last: .merged, then each loaded mod's files/ from the last loaded to
// the first (a later mod wins), then the pack's own files/ as before.

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "launch/paths.hpp"
#include "mod/manifest.hpp"
#include "pack/pack.hpp"

namespace sf::launch::mod_loader {

inline constexpr const char* kModsDir = "mods";
inline constexpr const char* kCacheDir = ".cache";
inline constexpr const char* kMergedDir = ".merged";
inline constexpr const char* kLayersFile = ".layers";
inline constexpr const char* kReportFile = ".loaded.json";

// One mod found in mods/.
struct Found {
    fs::path source;  // the zip or folder
    std::optional<mod::Manifest> manifest;
    std::string error;  // why it can't be read (manifest empty then)
    std::string id() const;  // the manifest's id, else the file name
};

// Every zip and folder in <profile>/mods/ (not the dot folders), sorted by file name. Reads only
// each mod.json, so it's quick enough for the Mods page.
std::vector<Found> scan(const fs::path& profile_dir);

struct Conflict {
    std::string what;  // "file data/ui/x.dds", "data/ui/x.txt: Texture[name=\"A\"]#rect", "lua UI.X:F"
    std::vector<std::string> mods;  // in load order; the last one wins
};

struct Mod {
    std::string id, name, version;
    fs::path source;
    bool on = false;      // the player's switch
    bool loaded = false;  // on, and nothing stopped it
    std::string reason;   // why it isn't loaded, or "" (off, a problem with it, a missing requirement)
    std::vector<std::string> notes;  // patches left out and the like
};

struct Result {
    std::vector<Mod> mods;  // in load order (unloaded ones too, where they would have been)
    std::vector<Conflict> conflicts;
    std::vector<std::string> core_patches;  // the loaded mods' `needs`
    std::vector<std::string> layers;        // relative to the profile, '/' separators
    std::string fingerprint;                // short hash of the loaded ids and versions in order
    std::vector<std::string> notes;         // for the player: one line each, ready to show
};

// The order mods load in: the player's `order`, then the rest by after/before hints, then id.
std::vector<std::string> load_order(const std::vector<Found>& found, const pack::ModChoices& choices);

// Does the work for a launch: unpacks, orders, checks requirements, merges patches, writes
// .layers and .loaded.json (or removes them when nothing loads). `game_dir` is where game files
// not in the pack are read from; `game_exe_sha256` (called only if a mod lists builds; "" =
// unknown) checks each mod's `game`.
// Throws std::runtime_error only for I/O failures on the profile itself; a broken mod is reported
// and left out.
Result build(const fs::path& profile_dir, const fs::path& game_dir, const std::function<std::string()>& game_exe_sha256);

// What build() last reported, from .loaded.json; nullopt if it never ran (or no mods loaded).
std::optional<Result> last(const fs::path& profile_dir);

// Players' switches, saved in local.json: on/off, and moving a mod in the order.
void set_on(const fs::path& profile_dir, const std::string& id, bool on);
void set_order(const fs::path& profile_dir, const std::vector<std::string>& order);

}  // namespace sf::launch::mod_loader
