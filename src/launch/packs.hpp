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
#include <vector>

#include "launch/paths.hpp"
#include "launch/remote.hpp"
#include "launch/trust.hpp"
#include "pack/pack.hpp"

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

// Where a pack comes from.
struct Source {
    enum Kind { Path, Url } kind = Path;
    fs::path path;    // Path: a zip file or a pack folder
    std::string url;  // Url: a published pack.json or a zip

    // A path, an http(s):// URL, or a starforge://install?url= link. Throws std::runtime_error.
    static Source parse(const std::string& text);
    bool operator==(const Source& o) const { return kind == o.kind && path == o.path && url == o.url; }
};

// What a pack can change, from least to most powerful.
struct Reach {
    size_t files = 0;      // files the game reads (data, scripts, configs) under files/
    size_t patchsets = 0;  // the pack's own patchsets, which change the game's code in memory
    size_t core = 0;       // core patches it turns on; their code ships with the launcher
    size_t plugins = 0;    // native DLLs loaded into the game

    static Reach of(const fs::path& root, const pack::Pack& pack);
    // Whether the pack brings its own code for the game, beyond its data and the core patches.
    bool runs_code() const { return patchsets > 0 || plugins > 0; }
};

// A pack unpacked for review, not yet installed.
struct Staged {
    fs::path dir;      // staging folder; removed by discard() or install()
    fs::path root;     // folder inside `dir` holding pack.json
    pack::Pack pack;
    std::string origin;     // where it came from, for display
    bool verified = false;  // the download matched the SHA-256 in a published pack.json
    trust::Trust trust;     // who signed it, if anyone, and whether that key is already trusted
    Reach reach;            // what the pack changes, for the review
    std::optional<remote::FileList> remote;  // its files_from list, fetched and checked; downloaded on install
    std::vector<std::string> files;          // paths under files/, with / separators, sorted
    std::vector<std::string> warnings;       // worth telling the player; they don't stop the install
};

// Unpacks a pack for review, next to the profiles it will be installed into. On error nothing is
// left behind. Throws std::runtime_error.
Staged stage(const Source& source, const fs::path& profiles_dir);
// Paths under a profile's files/, with / separators, sorted.
std::vector<std::string> file_list(const fs::path& profile_dir);
void discard(const Staged& staged);

// An installed pack.
struct Installed {
    std::string name;                    // the profile's folder name
    std::optional<std::string> warning;  // installed, but its listed game files didn't all download
};

// Installs a reviewed pack: updates the profile that already has this pack id, or creates a new
// one, then downloads any listed game files, telling `progress` how that goes (see
// remote::sync). The staging folder is removed either way. Throws std::runtime_error.
Installed install(const Staged& staged, const fs::path& profiles_dir, const remote::Progress& progress);

// Checks an installed pack before it starts: its files against its signature (code fully, files/
// by size) and its key against the pinned one. A signed pack installed without the launcher gets
// its key pinned here. Throws std::runtime_error.
trust::Trust verify_installed(const fs::path& profile_dir);

// A newer published version of an installed pack.
struct Available {
    std::string version, url;
    bool required = false;  // the server no longer accepts the installed version
};

// Asks the pack's update address whether there is a newer version. Throws std::runtime_error.
std::optional<Available> check_update(const pack::Pack& pack);

// A warning when the pack lists the game builds it was made for and the installed game.exe is
// none of them.
std::optional<std::string> game_build_warning(const pack::Pack& pack, const fs::path& game_exe);

// For tests: what a pack may hold (relative path, / separators), and a path made plain.
bool allowed(const std::string& rel);
std::optional<std::string> clean_rel(const std::string& path);

}  // namespace sf::launch::packs
