#pragma once

// The interface's files (gui/ui) and icons (gui/icons), built into the program by CMake
// (cmake/embed_assets.cmake), so a launcher is one file.

#include <cstddef>
#include <string>

namespace sf::app {

struct Asset {
    const char* path;  // "index.html", "js/launcher/app.js", "icons/128x128.png", ...
    const char* mime;
    const unsigned char* data;
    size_t size;
};

// The file at `path`, or nullptr.
const Asset* find_asset(const std::string& path);

}  // namespace sf::app
