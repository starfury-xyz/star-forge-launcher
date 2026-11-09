#pragma once

// Port of sc-launch's remote.rs: game files from a published file list (files_from in
// pack.json).
//
// Instead of shipping its game files in files/, a pack can point at a list of them on the
// server's site. Two list formats are read, so servers can keep the one they already publish:
//
// - STAR FORGE file list (JSON): {"files": {"data/x.lua": {"sha256": "...", "size": 123}}}, with
//   each file at the list's folder plus its path (or base_url plus its path, or its own url).
//   Extra fields such as version are ignored, so per-version manifests from other tools work.
// - SCMOD1: a text file whose first line is SCMOD1, then one path<TAB>sha256<TAB>https-url per
//   file (# lines are comments).
//
// Paths are relative to the game folder, like files/. Files are downloaded into the profile's
// files/ (never the game folder), each checked against its SHA-256, and only new or changed files
// are fetched. For a signed pack the list must carry a detached signature by the same key at
// <list>.sig, which covers every file through the hashes. Portable.

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "launch/paths.hpp"
#include "launch/signing.hpp"
#include "pack/pack.hpp"

namespace sf::launch::remote {

// One file in a list.
struct Entry {
    std::string path;    // relative to the game folder, / separators
    std::string sha256;  // lower-case hex
    std::optional<uint64_t> size;
    std::string url;
    bool operator==(const Entry& o) const { return path == o.path && sha256 == o.sha256 && size == o.size && url == o.url; }
};

// A fetched and checked file list.
struct FileList {
    std::string source;
    std::vector<Entry> entries;
    bool is_signed = false;  // carried a valid signature by the pack's key

    std::optional<uint64_t> bytes() const;  // total size, when the list gives every file's size
    std::string host() const;               // the host the files come from, for display
};

// Why a list couldn't be used: the network (worth retrying, and old files may do) or the list
// itself (never use it).
struct ListError : std::runtime_error {
    bool network;
    ListError(bool network, const std::string& msg) : std::runtime_error(msg), network(network) {}
};

// The parsers throw std::runtime_error with the reason.
std::vector<Entry> parse_json(const std::string& bytes, const std::string& list_url);
std::vector<Entry> parse_scmod1(const std::string& text);

// Downloads a pack's file list and checks it. `key` is the pack's signing key: a signed pack's
// list must be signed by it too. Throws ListError.
FileList fetch_list(const pack::FilesFrom& from, const signing::PublicKey* key);

// A file list for every file under `folder` (laid out like the game folder). SCMOD1 needs
// `base_url`, since it names each file's address; a STAR FORGE list without one expects the files
// next to it. Throws std::runtime_error.
std::string build_list(const fs::path& folder, const std::optional<std::string>& base_url, bool scmod1);

// What a sync did.
struct Synced {
    size_t downloaded = 0, removed = 0;
    bool operator==(const Synced& o) const { return downloaded == o.downloaded && removed == o.removed; }
};

// What a sync is doing, for a progress display.
struct SyncEvent {
    enum Kind {
        // Before anything downloads: `files` of the list's files are new or changed (`bytes` in
        // all, when the list gives every size), `kept` are already up to date, and `remove` were
        // dropped from the list.
        Plan,
        File,      // `path` starts downloading; `size` is from the list, if it says
        Bytes,     // `done` bytes of that file have arrived, of `total` (the list's size, else the server's)
        Verified,  // `path` arrived whole and matches `sha256`; `done` is its size
        Removed,   // `path` was dropped from the list, and deleted
    } kind;
    explicit SyncEvent(Kind k) : kind(k) {}
    const FileList* list = nullptr;  // Plan
    size_t files = 0, kept = 0, remove = 0;
    std::optional<uint64_t> bytes;  // Plan
    std::string path, sha256;
    std::optional<uint64_t> size;   // File
    uint64_t done = 0;              // Bytes, Verified
    std::optional<uint64_t> total;  // Bytes
};

// Hears what a sync does. Returning a message stops the sync with that error.
using Progress = std::function<std::optional<std::string>(const SyncEvent&)>;
// A progress callback that ignores everything.
std::optional<std::string> quiet(const SyncEvent&);

// Exclusive hold on a profile's game files while they change.
class ProfileLock {
public:
    ~ProfileLock();
    ProfileLock(ProfileLock&& o) noexcept;
    ProfileLock(const ProfileLock&) = delete;
    ProfileLock& operator=(const ProfileLock&) = delete;

private:
    friend ProfileLock lock_profile(const fs::path& profile_dir);
    ProfileLock() = default;
#ifdef _WIN32
    void* handle_ = nullptr;
#else
    int fd_ = -1;
#endif
};

// Takes the profile's sync lock, or throws std::runtime_error saying what holds it.
ProfileLock lock_profile(const fs::path& profile_dir);

// Brings a profile's files/ in line with the list: downloads what's new or changed, deletes what
// the list dropped. Files the list never named (say, the launcher's own address.cfg) are left
// alone. Nothing in files/ changes until every file has downloaded and matched its checksum; then
// they all move in at once. A failed or stopped sync leaves the old files as they were (finished
// downloads wait for the next try). Only one sync per profile runs at a time. Throws
// std::runtime_error.
Synced sync(const fs::path& profile_dir, const FileList& list, const Progress& progress);

// Whether every file from the last sync is still there, at its size.
bool intact(const fs::path& profile_dir);

// Before a launch: refreshes a files_from pack's files. If the list can't be reached but the last
// download is intact, the game starts with those (and the returned note says so); a list that
// fails its checks stops it (throws std::runtime_error).
std::optional<std::string> launch_sync(const fs::path& profile_dir, const pack::Pack& pack, const signing::PublicKey* key,
                                       const Progress& progress);

// Forgets which files a profile downloaded (when a pack stops using a list).
void forget(const fs::path& profile_dir);

inline constexpr const char* kStagingDir = ".sync-staging";  // downloads wait here, then move in together
inline constexpr const char* kSwappingMark = ".sync-swapping";  // present while staged files move in
inline constexpr const char* kLockFile = ".sync.lock";  // held while a profile syncs

}  // namespace sf::launch::remote
