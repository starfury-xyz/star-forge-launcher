// The Servers page: the server installs the player hosts, each checked the way its processes
// will check themselves at startup. New in the C++ launcher; no Rust counterpart. A thin layer
// over launch::servers and launch::mods.

#include <ctime>
#include <memory>
#include <stdexcept>

#include "gui/internal.hpp"
#include "launch/env.hpp"
#include "launch/launch.hpp"
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

const char* run_name(servers::RunState::State s) {
    switch (s) {
    case servers::RunState::Stopped: return "stopped";
    case servers::RunState::Running: return "running";
    case servers::RunState::Stale: return "stale";
    }
    return "";
}

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
    const auto build = mods::find_proxy(tools_dir());
    const servers::RunState run = servers::run_state(s.cloud, std::time(nullptr));
    return json(ojson{
        {"server", to_json(s)},
        {"run", ojson{{"state", run_name(run.state)}, {"started", run.started}, {"mongoPort", run.mongo_port}, {"ownMongo", run.own_mongo}}},
        {"proxy", proxy_name(st.proxy)},
        {"proxyBuild", build ? json(ustr(*build)) : json(nullptr)},
        {"onError", st.config ? json(st.config->exit_on_error ? "exit" : "continue") : json(nullptr)},
        {"exes", exes},
        {"errors", st.errors},
        {"patches", list.toggles.size()},
        {"patchesOn", on},
    });
}

json Backend::install_server_proxy(const std::string& id) {
    const servers::Server s = servers::get(data_dir(), id);
    const auto build = mods::find_proxy(tools_dir());
    if (!build)
        throw std::runtime_error("no server proxy build found: it goes in server/ beside the launcher's tools, or set " +
                                 std::string(launch::env::kServerProxy));
    mods::install_proxy(s.cloud, *build);
    return server_status(id);
}

json Backend::uninstall_server_proxy(const std::string& id) {
    mods::uninstall_proxy(servers::get(data_dir(), id).cloud);
    return server_status(id);
}

// sc-server.exe runs as long as the server does; a background task waits for it (so it's reaped)
// and reports a quick failure. Starting or stopping takes a while; the page polls server_status.
void Backend::start_server(const std::string& id) {
    const servers::Server s = servers::get(data_dir(), id);
    if (servers::run_state(s.cloud, std::time(nullptr)).state == servers::RunState::Running)
        throw std::runtime_error(s.name + " is already running");
    const launch::Command cmd = launch::server_command(launch::resolve(options()), s.cloud, "run");
    const fs::path out = mods::server_root(s.cloud) / "logs" / "sc-server.stdout";
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    auto child = std::make_shared<launch::Process>(launch::spawn(cmd, {launch::Output::Kind::File, out}, true));
    const std::string name = s.name;
    background(
        [this, child, name] {
            const std::optional<int> code = child->wait();
            if (code != 0)
                notify(name + " stopped with a problem; see its starforge/logs/sc-server.log", NotificationLevel::Error);
        },
        true);
}

void Backend::stop_server(const std::string& id) {
    const servers::Server s = servers::get(data_dir(), id);
    const launch::Command cmd = launch::server_command(launch::resolve(options()), s.cloud, "stop");
    auto child = std::make_shared<launch::Process>(launch::spawn(cmd, {launch::Output::Kind::Null, {}}, true));
    const std::string name = s.name;
    background(
        [this, child, name] {
            if (child->wait() != 0)
                notify(name + " is taking long to stop; see its starforge/logs/sc-server.log", NotificationLevel::Error);
        },
        true);
}

// The game's LocalAppData (in the Proton prefix on Linux), where Hub writes the services' logs.
// Throws std::runtime_error if the game or its prefix can't be found.
fs::path Backend::server_local_appdata() const {
    auto dir = launch::profiles::shared_local_appdata(launch::resolve(options()));
    if (!dir) throw std::runtime_error("cannot work out the game's AppData folder");
    return *dir;
}

json Backend::server_logs(const std::string& id) {
    const servers::Server s = servers::get(data_dir(), id);
    std::optional<fs::path> local;
    json note = nullptr;
    try {
        local = server_local_appdata();
    } catch (const std::exception& e) {
        note = std::string("The game's own logs can't be shown: ") + e.what();
    }
    json out = json::array();
    for (const servers::LogFile& f : servers::logs(s.cloud, local))
        out.push_back(json(ojson{{"group", f.group}, {"label", f.label}, {"name", f.name}, {"size", f.size}, {"modified", f.modified}}));
    return json(ojson{{"logs", out}, {"note", note}});
}

json Backend::server_log(const std::string& id, const std::string& group, const std::string& name) {
    const servers::Server s = servers::get(data_dir(), id);
    std::optional<fs::path> local;
    if (group == "game") local = server_local_appdata();
    // Only a file the listing has: the name never becomes a path by itself.
    for (const servers::LogFile& f : servers::logs(s.cloud, local)) {
        if (f.group != group || f.name != name) continue;
        bool cut = false;
        std::string text = servers::log_tail(f.path, 256 * 1024, &cut);
        return json(ojson{{"text", text}, {"cut", cut}, {"size", f.size}, {"modified", f.modified}});
    }
    throw std::runtime_error("no log " + name + " for " + s.name);
}

void Backend::open_server_folder(const std::string& id, const std::string& which) {
    const servers::Server s = servers::get(data_dir(), id);
    fs::path dir = s.cloud;
    if (which == "logs") dir = mods::server_root(s.cloud) / "logs";
    else if (which == "game-logs") dir = server_local_appdata() / servers::kGameLogs;
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
