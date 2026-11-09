#pragma once

// Port of star-forge crates/sc-profile, the full model the launcher uses: pack.json, local.json,
// and the two merged into a Profile. Portable (Linux and Windows).
//
// A profile is a folder holding a server pack (pack.json, resources/, files/, patches/) as
// published by a server operator, plus the player's own local.json, which pack updates never
// touch.
//
// Reading is strict like the Rust (serde's deny_unknown_fields): an unknown or misspelt field
// is an error, so a pack author finds out at once. Writing leaves out empty and default fields,
// in the same order as the Rust, so files written by either launcher look the same.
//
// (The agent reads profiles with its own, narrower code in src/profile/, Windows-only.)

#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "launch/paths.hpp"

namespace sf::pack {

namespace fs = std::filesystem;

inline constexpr const char* kPackFile = "pack.json";          // the pack manifest
inline constexpr const char* kLocalFile = "local.json";        // the player's settings; never part of a pack
inline constexpr const char* kFilesDir = "files";              // mirrors the game root (files/data/address.cfg, ...)
inline constexpr const char* kResourcesDir = "resources";      // launcher artwork
inline constexpr const char* kManifestFile = "manifest.json";  // signed list of every file + its SHA-256
inline constexpr const char* kSignatureFile = "manifest.sig";  // ed25519 signature over manifest.json's bytes
inline constexpr const char* kPatchesDir = "patches";          // <module>.patchset.toml files and plugins
// Files under files/ the launcher writes itself (from server.address): never part of a pack's
// signature or file list.
inline const std::vector<std::string> kGeneratedFiles{"data/address.cfg"};
inline constexpr const char* kRemoteRecord = "remote-files.json";  // what files_from downloaded
inline constexpr const char* kStateDir = "state";  // the game's per-user data for this profile
// Files under %LOCALAPPDATA% that stay the same across profiles (graphics settings and key
// bindings), with / separators.
inline const std::vector<std::string> kSharedStateFiles{"Targem/StarConflict/user_config.xml",
                                                        "Targem/StarConflict/user_bindings.xml"};
inline constexpr const char* kProfileEnv = "SC_PROFILE";  // how sc-inject tells the agent the profile
inline constexpr const char* kInjectLog = "inject.log";
inline constexpr const char* kAgentLog = "agent.log";
inline constexpr uint32_t kPackFormat = 1;  // newest pack format this build understands

// What reading or validating throws; what() is the message for the player.
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Links {
    std::optional<std::string> website, discord, news, source;
    bool empty() const { return entries().empty(); }
    // (key, url) for every link that is set, in display order.
    std::vector<std::pair<std::string, std::string>> entries() const;
};

struct Requires {
    std::optional<std::string> launcher;  // minimum launcher version
    std::vector<std::string> game;        // SHA-256 of the win64\game.exe builds the pack was made for
    bool empty() const { return !launcher && game.empty(); }
};

struct Server {
    std::optional<std::string> address;  // master server host:port, written to files/data/address.cfg
    bool local = false;                  // also start the game's bundled local server (MongoDB + Hub)
};

struct Game {
    std::vector<std::string> args;
    bool empty() const { return args.empty(); }
};

struct Branding {
    std::optional<std::string> logo, icon;
    std::vector<std::string> background;  // one path or a list in the file; the launcher picks one at random
    std::optional<std::string> accent;    // #rrggbb
    bool empty() const { return !logo && !icon && background.empty() && !accent; }
};

// files_from: where a pack's game files are listed. Exactly one is set.
struct FilesFrom {
    std::optional<std::string> manifest;  // STAR FORGE JSON list: {"files": {"path": {"sha256", "size"}}}
    std::optional<std::string> scmod1;    // SCMOD1 list (tab-separated path, SHA-256, URL)
    const std::string& url() const;       // the list's URL
    bool operator==(const FilesFrom& o) const { return manifest == o.manifest && scmod1 == o.scmod1; }
};

// Where the launcher looks for newer versions of the pack.
struct Update {
    std::string url;                         // the published pack.json (with download filled in)
    std::optional<std::string> min_version;  // published copy: the oldest version the server accepts
};

// The pack's zip, in the published pack.json.
struct Download {
    std::string url;
    std::string sha256;  // hex
};

enum class PatchMethod { Inject };  // the agent patches the game in memory before it runs
enum class OnError {
    Exit,      // don't start the game (default)
    Continue,  // start it with whatever patches did apply
};

// patches in pack.json: how the pack's patchsets (every patches/*.patchset.toml) and plugins are applied.
struct Patches {
    PatchMethod method = PatchMethod::Inject;
    OnError on_error = OnError::Exit;
    std::vector<std::string> plugins;  // DLLs in patches/ the agent loads after patching
    std::vector<std::string> core;     // core patch ids shipped with the launcher, applied first
    bool is_default() const { return on_error == OnError::Exit && plugins.empty() && core.empty(); }
    bool operator==(const Patches& o) const {
        return method == o.method && on_error == o.on_error && plugins == o.plugins && core == o.core;
    }
};

// pack.json. See star-forge's schemas/pack.schema.json for the documented format.
struct Pack {
    std::optional<std::string> schema;  // "$schema"
    uint32_t format = 0;
    std::string id, name, version;
    std::string description;
    std::vector<std::string> authors;
    Links links;
    Requires requires_;  // "requires" (a C++20 keyword)
    Server server;
    Game game;
    Branding branding;
    std::optional<FilesFrom> files_from;  // game files downloaded from a list instead of shipped in files/
    Patches patches;
    std::optional<Update> update;
    std::optional<Download> download;

    // Throws Error("pack.json: ...").
    static Pack from_json(const std::string& text);
    // Pretty JSON (2-space indent) ending in a newline; empty/default fields left out.
    std::string to_json() const;
    // Checks everything that doesn't need the pack's files. Throws Error("pack.json: ...").
    void validate() const;
};

// local.json: the player's changes on top of the pack.
struct Local {
    std::optional<std::string> name;    // display name instead of the pack's
    std::optional<std::string> server;  // instead of the pack's; "" means none
    std::optional<bool> local_server;
    std::vector<std::string> args;  // added after the pack's
    bool trace_files = false;       // log every game path the agent sees (debugging; can be large)
    std::map<std::string, std::map<std::string, bool>> patches;  // module -> patch id -> on
    // Use the game's normal per-user data folder instead of the profile's own (state/).
    bool shared_game_data = false;

    static Local from_json(const std::string& text);  // throws Error("local.json: ...")
    std::string to_json() const;
};

// A profile ready to launch: the pack with the player's settings applied.
struct Profile {
    std::string name;
    std::optional<std::string> server;
    bool local_server = false;
    std::vector<std::string> args;
    bool trace_files = false;
    Patches patches;
    std::map<std::string, std::map<std::string, bool>> patch_choices;  // from local.json
    bool own_game_data = false;  // the game's per-user data lives in the profile's state/ folder

    static Profile from(const Pack& pack, const Local& local);  // Rust's Profile::new
    // Reads pack.json and the optional local.json from a profile folder. Throws Error.
    static Profile load(const fs::path& dir);
    // Full game.exe argument list: -launchLocalServer if wanted, then args.
    std::vector<std::string> full_args() const;
    // Contents of the generated address.cfg, if a server is set. Throws Error for a bad address.
    std::optional<std::string> address_cfg() const;
};

// Pack ids are a reversed domain or similar: [a-z0-9] runs joined by '.' or '-'.
bool valid_pack_id(const std::string& id);
// x.y.z with an optional -pre/+build suffix (ignored when comparing).
std::optional<std::tuple<uint64_t, uint64_t, uint64_t>> parse_version(const std::string& v);
// The path inside resources/ (or patches/) an entry points at, with / separators; nullopt unless
// it's safely inside: no '\', ':', empty parts, "." or "..".
std::optional<std::string> resource_rel(const std::string& path);
std::optional<std::string> patches_rel(const std::string& path);
// Throws Error unless `server` looks like host:port (only characters safe in address.cfg).
void check_address(const std::string& server);
// The patchset files in a profile folder (patches/*.patchset.toml, any case), sorted by name.
std::vector<fs::path> patchset_files(const fs::path& profile_dir);

}  // namespace sf::pack
