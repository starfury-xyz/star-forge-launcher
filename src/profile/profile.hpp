#pragma once

// Port of star-forge crates/sc-profile: the profile folder's files and how they combine.
// Only what the agent needs so far; the launcher-side parts (full pack validation, server
// address, game args, ...) come when the launcher is ported.

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace sf::profile {

// Names shared with the Rust launcher; they must match sc_profile's constants.
inline constexpr const wchar_t* kProfileEnv = L"SC_PROFILE";  // env var sc-launch sets for the agent
inline constexpr const wchar_t* kAgentLog = L"agent.log";
inline constexpr const wchar_t* kPackFile = L"pack.json";     // from the server's pack
inline constexpr const wchar_t* kLocalFile = L"local.json";   // the player's changes, optional
inline constexpr unsigned kPackFormat = 1;                    // newest pack format we understand

// `patches.method` in pack.json. Only "inject" exists today.
enum class PatchMethod { Inject };
// `patches.on_error`: what happens when a required patch doesn't match the game.
enum class OnError {
    Exit,      // don't start the game (default)
    Continue,  // start it with whatever patches did apply
};

// `patches` in pack.json: how the pack's patchsets and plugins are applied.
struct Patches {
    PatchMethod method = PatchMethod::Inject;
    OnError on_error = OnError::Exit;
    std::vector<std::string> plugins;  // DLLs in patches/ loaded into the game after patching
    std::vector<std::string> core;     // core patch ids shipped with the launcher
};

// A profile ready to launch: pack.json with the player's local.json applied.
struct Profile {
    std::string name;   // local.json name if set (and not blank), else the pack's
    bool trace_files = false;
    Patches patches;
    // The player's patch choices from local.json: module -> patch id -> on.
    std::map<std::string, std::map<std::string, bool>> patch_choices;
    // The game's per-user data lives in the profile's state\ folder (local.json
    // shared_game_data turns this off).
    bool own_game_data = true;
};

// What load() throws; what() is the message the agent logs.
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Reads pack.json and the optional local.json from a profile folder. Throws Error.
Profile load(const std::wstring& dir);

// Same as sc_profile::normalize: drops a \\?\ or \??\ prefix, turns / into \, collapses
// repeated separators and strips trailing ones (keeping a drive root like C:\).
std::wstring normalize(const std::wstring& path);

}  // namespace sf::profile
