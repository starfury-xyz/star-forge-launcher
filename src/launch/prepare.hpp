#pragma once

// Port of sc-launch's prepare.rs: the steps before every launch, shared by `sc-launch run` and
// the launcher: check the pack, ask its server about updates, refresh its game files, check its
// patches, sync shared game settings, and build the command. Portable.

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "launch/launch.hpp"
#include "launch/packs.hpp"
#include "launch/remote.hpp"
#include "launch/trust.hpp"
#include "pack/pack.hpp"

namespace sf::launch::prepare {

// What prepare() reports on the way.
struct Event {
    enum Kind {
        Note,      // something the player should know (`text`); not an error
        Update,    // the pack's server has a newer version (`update`; if required, the launch stops)
        Sync,      // game-file download progress (`sync`, see remote::sync)
        SyncDone,  // the game-file refresh finished, with `sync_error` if it failed
    } kind;
    std::string text;
    const packs::Available* update = nullptr;
    const remote::SyncEvent* sync = nullptr;
    std::optional<std::string> sync_error;
    explicit Event(Kind k) : kind(k) {}
};

// Hears what prepare() does. Returning a message stops it with that error (say, a cancelled
// download).
using Events = std::function<std::optional<std::string>(const Event&)>;

// A profile ready to start.
struct Prepared {
    Command command;  // starts sc-inject for the profile
    pack::Profile profile;
    trust::Trust trust;
    std::optional<fs::path> shared;  // the shared copy of the game's settings; sync again after the game exits
};

// Gets the profile in `dir` ready to start with `setup`. Throws std::runtime_error.
Prepared prepare(const Setup& setup, const fs::path& dir, const std::vector<std::string>& game_args, const Events& events);

}  // namespace sf::launch::prepare
