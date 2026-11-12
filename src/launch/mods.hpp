#pragma once

// Patch toggles: one list of patches with an on/off switch each, the same for a client profile
// and a server install, for the CLI and the launcher window.
//
// Client: a profile's patches are the core patches (all of them; off unless the pack asks for
// one) and the pack's own patchsets (patches/*.patchset.toml). The player's switches live in
// local.json, `"patches": {"game.exe": {"<id>": true}}`, which the agent already reads.
//
// Server: a server install is the game's cloud/ folder. Its patches are the patchsets the proxy
// DLL loads (cloud/starforge/patchsets/*.patchset.toml), and the switches live in
// cloud/starforge/patcher.toml, `[patches."Shard.exe"] "<id>" = false`, which every server
// process reads at startup (so a change needs a server restart).
//
// Either way a switch the player never touched follows the patch's `default`; setting it back
// to "default" removes the entry instead of writing the default down. Portable.

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "launch/paths.hpp"
#include "pack/pack.hpp"
#include "patch/patchset.hpp"
#include "patch/server_config.hpp"

namespace sf::launch::mods {

struct Toggle {
    std::string module;  // e.g. game.exe, Shard.exe
    std::string id;
    std::string title;
    std::string description;
    std::string source;  // "core", or the patchset file it comes from (patches/x.patchset.toml)
    std::string build;   // game build the patchset was made for (informational; may be empty)
    bool on = false;
    bool default_on = false;  // what `on` is without a switch: for core patches, whether the pack asks for it
    std::optional<bool> chosen;  // the player's switch, if they set one
    bool optional = false;       // a mismatch skips it instead of stopping the game or server
    // Other patches of the same module that write some of the same bytes, as "module:id". Two of
    // them can't be on together.
    std::vector<std::string> conflicts;
};

struct List {
    std::vector<Toggle> toggles;      // sorted by module, then source order
    std::vector<std::string> errors;  // patchsets that couldn't be read
    std::vector<std::string> warnings;  // switches naming a patch that doesn't exist, and the like
};

// The patch `id` of `module` (ignoring case) in `list`, if it's there.
const Toggle* find(const List& list, const std::string& module, const std::string& id);
// If that patch is on: the patches it clashes with that are on too ("module:id"), else nothing.
std::vector<std::string> clashes_on(const List& list, const std::string& module, const std::string& id);

// --- client profiles ---

List client(const fs::path& profile_dir, const pack::Profile& profile);
// Sets (true/false) or clears (nullopt) the player's switch in local.json. Throws
// std::runtime_error for a patch the profile doesn't have.
void set_client(const fs::path& profile_dir, const std::string& module, const std::string& id, std::optional<bool> on);

// --- server installs ---

inline constexpr const char* kServerDir = "starforge";          // under cloud/
inline constexpr const char* kServerConfig = "patcher.toml";    // in starforge/
inline constexpr const char* kServerPatchsets = "patchsets";    // in starforge/

// cloud/starforge/patcher.toml, shared with the proxy DLL.
using ServerConfig = patch::ServerConfig;

fs::path server_root(const fs::path& cloud_dir);  // cloud/starforge
ServerConfig read_server_config(const fs::path& cloud_dir);  // a missing file is the default
void write_server_config(const fs::path& cloud_dir, const ServerConfig& config);

List server(const fs::path& cloud_dir);

// The proxy DLL: the game's own libcrypto renamed to ..._orig.dll, ours in its place.
inline constexpr const char* kProxyDll = "libcrypto-1_1_tg.dll";
inline constexpr const char* kProxyOrig = "libcrypto-1_1_tg_orig.dll";

struct ExeCheck {
    std::string module;
    std::string build;               // the patchset's
    std::optional<std::string> sha1;  // of the exe in cloud/; nullopt if it isn't there
    patch::Report report;             // its enabled patches checked against that file
};

struct ServerStatus {
    enum Proxy {
        NoDll,      // no libcrypto-1_1_tg.dll at all: probably not a cloud/ folder
        Stock,      // the game's own DLL, no proxy
        Installed,  // proxy in place, original kept
        Restored,   // both files the same: Steam "verify files" put the original back
        Broken,     // the original's copy is gone, or the like
    } proxy = NoDll;
    std::optional<ServerConfig> config;  // nullopt if patcher.toml is unreadable (then in errors)
    std::vector<ExeCheck> exes;          // one per patchset
    std::vector<std::string> errors;     // patchsets or config that couldn't be read
};
// Checks a server install the way the proxy DLL will at startup, without starting anything.
ServerStatus server_status(const fs::path& cloud_dir);
// Like set_client, in patcher.toml.
void set_server(const fs::path& cloud_dir, const std::string& module, const std::string& id, std::optional<bool> on);

}  // namespace sf::launch::mods
