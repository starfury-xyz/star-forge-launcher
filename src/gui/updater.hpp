#pragma once

// The launcher's self-update (port of updater.rs, which uses tauri-plugin-updater): a signed
// latest.json names the newest version and, per platform, a package and its minisign signature.
// The parts here are portable and tested; the Backend commands use them, and the host puts the
// package in place (Host::install_update).

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "launch/paths.hpp"

namespace sf::gui::updater {

namespace fs = std::filesystem;

// Where to look and what to trust. An empty endpoint means this build doesn't update itself.
struct Config {
    std::string endpoint;  // latest.json's URL
    // With pre-release updates on: a GitHub API release list (see latest_in); empty if none.
    std::string prerelease_endpoint;
    std::string pubkey;    // tauri.conf.json's: base64 of a minisign public key file
    std::string current;   // this launcher's version
    // latest.json platform keys, most specific first ("linux-x86_64-appimage", "linux-x86_64").
    std::vector<std::string> targets;
    // Why this copy can't update itself (say, installed by a package manager), if it can't.
    std::optional<std::string> unsupported;
};

// The update latest.json offers.
struct Release {
    std::string version, url, signature;
    std::optional<std::string> page;  // release notes, when an https:// link
};

// Semantic-version order of "x.y.z", "x.y.z-pre" or the same with a leading v: <0, 0 or >0.
// A pre-release comes before its release (0.3.1-rc.2 < 0.3.1); build metadata (+...) is ignored.
// Throws std::runtime_error for a version that isn't x.y.z.
int compare_versions(const std::string& a, const std::string& b);

// The latest.json URL of the newest release in a GitHub API release list (pre-releases
// included; drafts and releases without a latest.json skipped), or nullopt if there is none.
// Throws std::runtime_error if `releases` isn't a release list.
std::optional<std::string> latest_in(const nlohmann::json& releases);

// The release in `latest` if it is newer than `current`; nullopt if not. Throws
// std::runtime_error for a malformed latest.json or one without any of `targets`.
std::optional<Release> newer(const nlohmann::json& latest, const std::string& current, const std::vector<std::string>& targets);

// Throws std::runtime_error unless `signature` (latest.json's: base64 of a minisign signature
// file) is a good signature of `file` by `pubkey` (base64 of a minisign public key file), as
// minisign-verify checks it: the key id, the signature (of the file's BLAKE2b-512 hash, or of the
// file itself for a legacy one) and the global signature over the trusted comment.
void verify(const std::string& pubkey, const std::string& signature, const fs::path& file);

// Swaps `package` in for the AppImage at `target`, keeping its permissions: written next to it,
// then renamed over it (the running copy keeps its old file). Throws std::runtime_error.
void replace_file(const fs::path& target, const fs::path& package);

}  // namespace sf::gui::updater
