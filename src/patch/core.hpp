#pragma once

// Core patches: patchsets that ship with STAR FORGE (core-patches/ in the repository) for
// behaviour many packs need. A pack turns them on by id ("patches": {"core": [...]} in
// pack.json) instead of carrying the bytes itself; they are applied exactly like the pack's own
// patchsets. Port of crates/sc-patch/src/core.rs.

#include <map>
#include <string>
#include <vector>

#include "patch/patchset.hpp"

namespace sf::patch::core {

// Every core patch id.
std::vector<std::string> ids();

// The core patchsets holding just the requested patches. Throws Error naming an unknown id.
std::vector<Patchset> sets(const std::vector<std::string>& requested);

// The core patches a profile loads: the ones its pack asks for, then any other core patch the
// player switched on in local.json (`patches.<module>.<id> = true`, the same place they switch a
// pack's own patches). Loading one doesn't make it on: a pack's patch the player switched off
// stays off through the patchset's choices.
std::vector<std::string> requested(const std::vector<std::string>& pack_core,
                                   const std::map<std::string, Choices>& choices);

// One warning per player switch naming a patch that none of `sets` (the patchsets loaded for a
// profile) has for that module and that isn't a core patch either. Checked across all sets of a
// module, unlike Patchset::enabled's warnings, which can't know about the module's other sets.
std::vector<std::string> unknown_choices(const std::vector<const Patchset*>& sets,
                                         const std::map<std::string, Choices>& choices);

}  // namespace sf::patch::core
