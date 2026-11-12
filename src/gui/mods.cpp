// The Mods page: the patches of a profile (and, later, of a server install), each with its switch.
// New in the C++ launcher; no Rust counterpart. A thin layer over launch::mods.

#include <stdexcept>

#include "gui/internal.hpp"
#include "launch/mods.hpp"
#include "launch/profiles.hpp"
#include "pack/serde.hpp"

namespace sf::gui {

namespace mods = launch::mods;
using ojson = nlohmann::ordered_json;

namespace {

json to_json(const mods::List& list) {
    json toggles = json::array();
    for (const mods::Toggle& t : list.toggles)
        toggles.push_back(json(ojson{
            {"module", t.module},
            {"id", t.id},
            {"title", t.title},
            {"description", t.description},
            {"source", t.source},
            {"build", t.build},
            {"on", t.on},
            {"defaultOn", t.default_on},
            {"chosen", t.chosen ? json(*t.chosen) : json(nullptr)},
            {"optional", t.optional},
            {"conflicts", t.conflicts},
            {"clashing", mods::clashes_on(list, t.module, t.id)},  // conflicts that are on right now
        }));
    return json(ojson{{"toggles", toggles}, {"errors", list.errors}, {"warnings", list.warnings}});
}

void check_kind(const std::string& kind) {
    if (kind != "profile") throw std::runtime_error("unknown mod target " + pack::serde::debug_str(kind));
}

}  // namespace

json Backend::list_mods(const std::string& kind, const std::string& id) {
    check_kind(kind);
    const fs::path dir = profile_dir(id);
    return to_json(mods::client(dir, launch::profiles::load(dir)));
}

json Backend::set_mod(const std::string& kind, const std::string& id, const std::string& module, const std::string& patch,
                      std::optional<bool> on) {
    check_kind(kind);
    const fs::path dir = profile_dir(id);
    mods::set_client(dir, module, patch, on);
    return to_json(mods::client(dir, launch::profiles::load(dir)));
}

}  // namespace sf::gui
