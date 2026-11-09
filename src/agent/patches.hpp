#pragma once

#include <string>

#include "profile/profile.hpp"

namespace sf::agent::patches {

// Applies the core patches the pack asks for, then the pack's own patchsets, and loads its
// plugins. Returns false when the game must not start.
bool apply_all(const std::wstring& profile_dir, const profile::Profile& profile);

}  // namespace sf::agent::patches
