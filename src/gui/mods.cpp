// The Mods page: the patches of a profile or a server install, each with its switch.
// New in the C++ launcher; no Rust counterpart. A thin layer over launch::mods.

#include <stdexcept>

#include "gui/internal.hpp"
#include "launch/mods.hpp"
#include "launch/profiles.hpp"
#include "launch/servers.hpp"
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

bool is_server(const std::string& kind) {
    if (kind != "profile" && kind != "server") throw std::runtime_error("unknown mod target " + pack::serde::debug_str(kind));
    return kind == "server";
}

fs::path cloud_of(const std::string& id) { return launch::servers::get(data_dir(), id).cloud; }

}  // namespace

json Backend::list_mods(const std::string& kind, const std::string& id) {
    if (is_server(kind)) return to_json(mods::server(cloud_of(id)));
    const fs::path dir = profile_dir(id);
    return to_json(mods::client(dir, launch::profiles::load(dir)));
}

json Backend::set_mod(const std::string& kind, const std::string& id, const std::string& module, const std::string& patch,
                      std::optional<bool> on) {
    if (is_server(kind)) {
        const fs::path cloud = cloud_of(id);
        mods::set_server(cloud, module, patch, on);
        return to_json(mods::server(cloud));
    }
    const fs::path dir = profile_dir(id);
    mods::set_client(dir, module, patch, on);
    return to_json(mods::client(dir, launch::profiles::load(dir)));
}

}  // namespace sf::gui
