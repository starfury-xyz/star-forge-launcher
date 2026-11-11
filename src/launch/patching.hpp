#pragma once

// Port of sc-launch's patching.rs: client patches from a profile's patches/ folder, seen from
// the launcher: a summary for the interface and a check against the installed game before it
// starts, so a pack made for another game build fails here with a clear message instead of
// inside the game. Applying them is the patch method's job; for `inject` that is the agent.
// Portable.

#include <optional>
#include <string>
#include <vector>

#include "launch/paths.hpp"
#include "pack/pack.hpp"
#include "patch/patchset.hpp"

namespace sf::launch::patching {

// A patchset, or why it couldn't be had (Rust's Result<Patchset, String>).
struct Loaded {
    std::optional<patch::Patchset> set;
    std::string error;
};

// Everything a profile patches: the core patches its pack or the player turns on
// (core::requested), then the pack's own patchsets.
std::vector<Loaded> all_sets(const fs::path& profile_dir, const pack::Profile& profile);
// The patchsets in a profile folder, parsed. Unreadable ones are errors.
std::vector<Loaded> load_sets(const fs::path& profile_dir);

// What a profile patches, for display.
struct Summary {
    std::vector<std::string> modules;  // modules with a patchset, e.g. game.exe
    size_t patches = 0;                // patches that are on (after the player's choices)
    size_t plugins = 0;
    std::vector<std::string> errors;
    bool operator==(const Summary& o) const {
        return modules == o.modules && patches == o.patches && plugins == o.plugins && errors == o.errors;
    }
};
Summary summary(const fs::path& profile_dir, const pack::Profile& profile);

// Checks every enabled patch against the installed files. Throws std::runtime_error when the
// game must not start (the pack's on_error is exit); the returned warnings are worth showing.
std::vector<std::string> preflight(const fs::path& profile_dir, const pack::Profile& profile, const fs::path& game_exe);

}  // namespace sf::launch::patching
