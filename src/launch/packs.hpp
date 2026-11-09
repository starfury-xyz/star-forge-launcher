#pragma once

// Port of sc-launch's packs.rs: installing and updating server packs.
//
// A pack arrives as a zip (a file, or a download), a folder, or a published pack.json whose
// `download` names the zip and its SHA-256. A published pack.json without `download` is a whole
// pack by itself, for packs that only set a server and take their game files from a list
// (files_from). It is unpacked into a staging folder first so the player can review it, then
// moved into the profiles folder. Updating replaces the pack's own files and keeps the player's
// local.json.
//
// A signed pack (see signing.hpp) is checked file by file against its signature, and its key must
// match the one pinned to the pack id (see trust.hpp); the pinned key is checked again before
// every launch. Portable.

#include <optional>
#include <string>

#include "launch/paths.hpp"

namespace sf::launch::packs {

inline constexpr const char* kScheme = "starforge";  // one-click installs: starforge://install?url=...

// Packs come over HTTPS; plain HTTP only from this machine (for testing). Throws
// std::runtime_error saying why not.
void check_url(const std::string& url);

// Folder for scratch downloads, under the launcher data folder.
std::optional<fs::path> staging_root();
// A new, empty folder under staging_root(). Throws std::runtime_error.
fs::path new_staging_dir();
// Removes leftovers from reviews that never finished.
void clear_staging(const std::optional<fs::path>& profiles_dir);

}  // namespace sf::launch::packs
