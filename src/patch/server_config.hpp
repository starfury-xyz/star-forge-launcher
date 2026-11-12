#pragma once

// cloud/starforge/patcher.toml: a server install's settings, read by the proxy DLL in every
// server process and written by the launcher's Mods page and `sc-launch server`.

#include <map>
#include <string>

#include "patch/patchset.hpp"

namespace sf::patch {

// Unknown keys are ignored, so an older proxy reads a newer file and the other way round.
struct ServerConfig {
    bool exit_on_error = true;              // on_error = "exit" (default) or "continue"
    std::map<std::string, Choices> patches;  // module -> patch id -> on
    // [files]: where changed game files are served from. Relative to starforge/, or absolute.
    std::string overlay = "overlay";  // the pack's files, laid out like the game folder
    std::string state = "state";      // what the servers write over game files
    // [stack]: exe -> main thread stack in MB, for exes whose stock 1 MB overflows (Shard.exe on
    // big store.xml files). Replaces patching SizeOfStackReserve in the exe on disk.
    std::map<std::string, int> stack_mb;

    // Throws std::runtime_error naming `name`.
    static ServerConfig parse(const std::string& text, const std::string& name);
    std::string to_toml() const;  // with the explanatory comments `sfp install` writes
};

}  // namespace sf::patch
