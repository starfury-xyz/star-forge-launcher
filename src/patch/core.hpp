#pragma once

// Core patches: patchsets that ship with STAR FORGE (core-patches/ in the repository) for
// behaviour many packs need. A pack turns them on by id ("patches": {"core": [...]} in
// pack.json) instead of carrying the bytes itself; they are applied exactly like the pack's own
// patchsets. Port of crates/sc-patch/src/core.rs.

#include <string>
#include <vector>

#include "patch/patchset.hpp"

namespace sf::patch::core {

// Every core patch id.
std::vector<std::string> ids();

// The core patchsets holding just the requested patches. Throws Error naming an unknown id.
std::vector<Patchset> sets(const std::vector<std::string>& requested);

}  // namespace sf::patch::core
