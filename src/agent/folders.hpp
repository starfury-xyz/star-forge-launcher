#pragma once

#include <string>

namespace sf::agent::folders {

// Points the game's %LOCALAPPDATA% at `dir`, creating it. On any problem the game keeps the
// normal folder, and the log says why.
void install(const std::wstring& dir);

}  // namespace sf::agent::folders
