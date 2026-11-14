#pragma once

// File redirects: hooks the Win32 file API so a path inside the game root is served from a
// folder laid out like the game root, when that folder has the file. Nothing on disk is moved.
//
// The client agent has one layer (<profile>\files). The server proxy has the pack's overlay, and
// a writes folder: anything a server writes over a game file lands there instead (copy on write),
// so the game install and the overlay never change.

#include <string>
#include <vector>

namespace sf::redirect {

struct Config {
    std::wstring game_root;
    std::vector<std::wstring> layers;  // searched in order; the first that has the file wins
    // "" = files are opened where reads point (the agent: a profile file is edited in place).
    // Otherwise every write to a game file goes here, and this folder is searched before `layers`.
    std::wstring writes;
    std::vector<std::wstring> skip;  // folders relative to the game root never redirected (e.g. cloud\starforge)
    bool trace = false;              // log every game path seen
    void (*log)(const char* line) = nullptr;
};

// Hooks the file API. Returns "" once hooked, else why not; on any problem nothing is hooked.
std::string install(Config config);

}  // namespace sf::redirect
