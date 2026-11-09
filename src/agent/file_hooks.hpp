#pragma once

#include <string>

namespace sf::agent::file_hooks {

// Hooks the Win32 file API so a path inside `game_root` is replaced with the profile's copy
// when <profile_dir>\files\<same relative path> exists. `trace` logs every game path seen.
// On any problem nothing is hooked, and the log says why.
void install(const std::wstring& game_root, const std::wstring& profile_dir, bool trace);

}  // namespace sf::agent::file_hooks
