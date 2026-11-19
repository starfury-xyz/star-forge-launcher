#pragma once

// A diagnostics zip for support: a report.txt of what the launcher sees (versions, folders,
// the game setup, and for every profile whether its pack.json and local.json can really be
// read, parsed and replaced, with the exact error and, on Windows, which programs hold them),
// plus copies of the logs and profile files that explain a failure. Paths under the player's
// home folder are written as ~ so the zip can be posted in a support channel.

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "launch/launch.hpp"

namespace sf::launch::diagnostics {

struct Input {
    std::string app;                    // "STAR FORGE Launcher" or "sc-launch"
    std::optional<fs::path> data_dir;   // the launcher's data folder
    fs::path profiles_dir;
    std::optional<Setup> setup;         // the game setup, if found
    std::string setup_error;            // why it wasn't, if known
};

struct Report {
    std::string text;                                      // report.txt
    std::vector<std::pair<std::string, fs::path>> files;  // copied into the zip: name in the zip -> file
};

// Looks at everything; never throws for what it finds (a failure is part of the report).
Report collect(const Input& in);

// The zip's bytes: report.txt, then each file (the last 2 MB of a bigger one), home paths as ~.
std::string zip(const Report& report);

// starforge-diagnostics-<date>-<time>.zip
std::string default_name();

// Writes the zip to `file`. Throws std::runtime_error.
void save(const Report& report, const fs::path& file);

// Where to offer to save it: Downloads, else the home folder.
std::optional<fs::path> default_dir();

// `text` with the player's home folder (in its Unix, Windows and Wine Z:\ spellings) as ~.
std::string redact(std::string text);

}  // namespace sf::launch::diagnostics
