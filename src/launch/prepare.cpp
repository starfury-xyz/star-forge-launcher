#include "launch/prepare.hpp"

#include <stdexcept>

#include "launch/mod_loader.hpp"
#include "launch/patching.hpp"
#include "launch/profiles.hpp"
#include "launch/signing.hpp"

namespace sf::launch::prepare {
namespace {

void tell(const Events& events, const Event& e) {
    if (auto stop = events(e)) throw std::runtime_error(*stop);
}

Event note(const std::string& text) {
    Event e(Event::Note);
    e.text = text;
    return e;
}

}  // namespace

Prepared prepare(const Setup& setup, const fs::path& dir, const std::vector<std::string>& game_args, const Events& events) {
    pack::Profile profile = profiles::load(dir);
    trust::Trust t = packs::verify_installed(dir);
    const pack::Pack pack = profiles::read_pack(dir);
    if (auto warning = packs::game_build_warning(pack, setup.game_exe())) tell(events, note(*warning));
    if (pack.update) {
        std::optional<packs::Available> found;
        std::string failed;
        try {
            found = packs::check_update(pack);
        } catch (const std::exception& e) {
            failed = e.what();
        }
        if (!failed.empty()) {
            tell(events, note("Could not check " + pack.name + " for updates: " + failed));
        } else if (found) {
            Event u(Event::Update);
            u.update = &*found;
            tell(events, u);
            if (found->required)
                throw std::runtime_error(pack.name + "'s server needs pack version " + found->version + " or newer (you have " +
                                         pack.version + "). Update the pack first.");
            tell(events, note(pack.name + " " + found->version + " is available."));
        }
    }
    std::optional<std::string> synced;
    std::optional<std::string> sync_error;
    try {
        synced = remote::launch_sync(dir, pack, t.key ? &*t.key : nullptr, [&](const remote::SyncEvent& s) {
            Event e(Event::Sync);
            e.sync = &s;
            return events(e);
        });
    } catch (const std::exception& e) {
        sync_error = e.what();
    }
    Event done(Event::SyncDone);
    done.sync_error = sync_error;
    tell(events, done);
    if (sync_error) throw std::runtime_error(*sync_error);
    if (synced) tell(events, note(*synced));
    for (const std::string& warning : patching::preflight(dir, profile, setup.game_exe())) tell(events, note(warning));
    // The player's mods (docs/mods.md). A failure here starts the game without them rather than
    // not at all, and never with an older run's layers.
    std::vector<std::string> mod_notes;
    try {
        mod_notes = mod_loader::build(dir, setup.game_dir, [&] {
                        std::error_code ec;
                        return signing::sha256_file(setup.game_exe(), nullptr, ec).value_or("");
                    }).notes;
    } catch (const std::exception& e) {
        std::error_code ec;
        fs::remove(dir / mod_loader::kModsDir / mod_loader::kLayersFile, ec);
        mod_notes = {std::string("Starting without mods: ") + e.what()};
    }
    for (const std::string& n : mod_notes) tell(events, note(n));
    auto shared = profiles::shared_local_appdata(setup);
    if (setup.wine)
        if (auto from = profiles::import_steam_data(setup, steam_compat_data()))
            tell(events, note("Copied your Star Conflict settings and data from Steam's prefix (" + ustr(*from) + ")."));
    profiles::sync_shared(dir, profile, shared);
    Command command = launch::command(setup, dir, game_args);
    return Prepared{std::move(command), std::move(profile), std::move(t), shared};
}

}  // namespace sf::launch::prepare
