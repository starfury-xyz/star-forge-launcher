#pragma once

// mod.json: what a mod is and what it needs (docs/mods.md#modjson). Read strictly, like pack.json:
// an unknown field is an error, so an author finds a typo at once. Portable.

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace sf::mod {

inline constexpr const char* kManifestFile = "mod.json";
inline constexpr const char* kFilesDir = "files";      // new and replaced files, laid out like the game folder
inline constexpr const char* kPatchesDir = "patches";  // *.sfpatch
inline constexpr unsigned kModFormat = 1;

// The Lua API (src/mod/sfmod.lua): `requires: {"sfmod": ">=1"}` checks against this, and the
// loader registers sfmod.lua at this game path before any mod's script.
inline constexpr const char* kSfmodId = "sfmod";
inline constexpr const char* kSfmodVersion = "1.0.0";
inline constexpr const char* kSfmodScript = "ui\\sfmod\\sfmod.lua";
inline constexpr const char* kSfmodFile = "data/ui/sfmod/sfmod.lua";

struct Script {
    std::string file;                  // game path with backslashes, as uiresources.txt writes it
    std::optional<std::string> after;  // a script already in the list; else after the last of `groups`
    std::string type = "SCRIPT";       // resourceType: SCRIPT or TEXTURE
    std::optional<std::string> groups;  // loadingGroups; unset: the `after` script's, else APP_START
};

struct Manifest {
    unsigned format = 0;
    std::string id, name, version, description;
    std::vector<std::string> authors;
    std::map<std::string, std::string> links;
    std::vector<std::string> game;   // SHA-256 of the game.exe builds it was made for; empty = any
    std::vector<std::string> needs;  // core patch ids
    std::map<std::string, std::string> requires_;  // mod id -> version range (">=1.0", "*")
    std::vector<std::string> after, before;        // load-order hints
    std::vector<Script> scripts;
    std::vector<std::string> lua_replaces;  // "UI.Class:Function"
    bool server = false;

    // Throws pack::Error("mod.json: ...").
    static Manifest from_json(const std::string& text);
};

// [a-z0-9] runs joined by '-' (and '.'/'_' inside), at most 64 characters.
bool valid_id(const std::string& id);

// Whether `version` (x.y.z) satisfies `range`: "*", "x.y.z" (exactly), ">=x.y.z", ">x.y.z",
// "<=x.y.z", "<x.y.z", or several separated by spaces (all must hold). False for anything
// unreadable.
bool version_matches(const std::string& version, const std::string& range);

}  // namespace sf::mod
