// The Servers page: the server installs the player hosts, each checked the way its processes
// will check themselves at startup. New in the C++ launcher; no Rust counterpart. A thin layer
// over launch::servers and launch::mods.

#include <stdexcept>

#include "gui/internal.hpp"
#include "launch/mods.hpp"
#include "launch/profiles.hpp"
#include "launch/servers.hpp"

namespace sf::gui {

namespace mods = launch::mods;
namespace servers = launch::servers;
using launch::ustr;
using ojson = nlohmann::ordered_json;

fs::path data_dir() {
    auto dir = launch::profiles::data_dir();
    if (!dir) throw std::runtime_error("cannot work out the launcher's data folder");
    return *dir;
}

namespace {

json to_json(const servers::Server& s) { return json(ojson{{"id", s.id}, {"name", s.name}, {"cloud", ustr(s.cloud)}}); }

const char* proxy_name(mods::ServerStatus::Proxy p) {
    switch (p) {
    case mods::ServerStatus::NoDll: return "missing";
    case mods::ServerStatus::Stock: return "stock";
    case mods::ServerStatus::Installed: return "installed";
    case mods::ServerStatus::Restored: return "restored";
    case mods::ServerStatus::Broken: return "broken";
    }
    return "";
}

}  // namespace

json Backend::list_servers() {
    json out = json::array();
    for (const servers::Server& s : servers::list(data_dir())) out.push_back(to_json(s));
    return out;
}

json Backend::add_server(const std::string& name, const std::string& cloud) {
    return to_json(servers::add(data_dir(), trim(name), launch::upath(trim(cloud))));
}

void Backend::remove_server(const std::string& id) { servers::remove(data_dir(), id); }

json Backend::server_status(const std::string& id) {
    const servers::Server s = servers::get(data_dir(), id);
    const mods::ServerStatus st = mods::server_status(s.cloud);
    const mods::List list = mods::server(s.cloud);
    json exes = json::array();
    for (const mods::ExeCheck& c : st.exes)
        exes.push_back(json(ojson{
            {"module", c.module},
            {"build", c.build},
            {"sha1", c.sha1 ? json(*c.sha1) : json(nullptr)},
            {"ok", c.report.ok},
            {"skipped", c.report.skipped},
            {"warnings", c.report.warnings},
            {"errors", c.report.errors},
        }));
    size_t on = 0;
    for (const mods::Toggle& t : list.toggles) on += t.on;
    return json(ojson{
        {"server", to_json(s)},
        {"proxy", proxy_name(st.proxy)},
        {"onError", st.config ? json(st.config->exit_on_error ? "exit" : "continue") : json(nullptr)},
        {"exes", exes},
        {"errors", st.errors},
        {"patches", list.toggles.size()},
        {"patchesOn", on},
    });
}

void Backend::open_server_folder(const std::string& id, const std::string& which) {
    const servers::Server s = servers::get(data_dir(), id);
    fs::path dir = s.cloud;
    if (which == "logs") dir = mods::server_root(s.cloud) / "logs";
    else if (which != "cloud") throw std::runtime_error("unknown folder " + which);
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) throw std::runtime_error(ustr(dir) + " doesn't exist yet");
    host_.open(ustr(dir));
}

json Backend::suggest_server_cloud() {
    try {
        if (auto c = servers::game_cloud(launch::find_game(options()))) return ustr(*c);
    } catch (const std::exception&) {
    }
    return nullptr;
}

}  // namespace sf::gui
