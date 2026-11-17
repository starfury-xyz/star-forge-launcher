// The Mods page: a profile's mods (the zips and folders in its mods/ folder, docs/mods.md), and the
// patches of a profile or a server install, each with its switch.
// New in the C++ launcher; no Rust counterpart. A thin layer over launch::mod_loader and launch::mods.

#include <algorithm>
#include <stdexcept>

#include "gui/internal.hpp"
#include "launch/mod_loader.hpp"
#include "launch/mods.hpp"
#include "launch/profiles.hpp"
#include "launch/servers.hpp"
#include "launch/signing.hpp"
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

namespace ml = launch::mod_loader;
using launch::ustr;

// A profile's mods in load order, with what happened to each at the last start (or check).
json profile_mods(const fs::path& dir) {
    const auto found = ml::scan(dir);
    const auto local = launch::profiles::read_local(dir);
    const auto last = ml::last(dir);
    json mods = json::array();
    for (const auto& id : ml::load_order(found, local.mods)) {
        const ml::Found* f = nullptr;
        for (const auto& x : found)
            if (x.id() == id && !f) f = &x;
        if (!f) continue;
        const bool off = std::find(local.mods.off.begin(), local.mods.off.end(), id) != local.mods.off.end();
        ojson m{{"id", id}, {"file", ustr(f->source.filename())}, {"on", !off}};
        const mod::Manifest* man = f->manifest ? &*f->manifest : nullptr;
        m["name"] = man ? man->name : id;
        m["version"] = man ? man->version : "";
        m["description"] = man ? man->description : "";
        m["authors"] = man ? man->authors : std::vector<std::string>{};
        m["links"] = man ? json(man->links) : json::object();
        // broken: mod.json can't be read; off; loaded / failed: the last start; new: added since.
        std::string status = !man ? "broken" : off ? "off" : "new";
        std::string reason = man ? "" : f->error;
        std::vector<std::string> notes;
        if (man && !off && last)
            for (const auto& lm : last->mods)
                if (lm.id == id && lm.version == man->version) {
                    status = lm.loaded ? "loaded" : "failed";
                    reason = lm.reason;
                    notes = lm.notes;
                }
        m["status"] = status;
        m["reason"] = reason;
        m["notes"] = notes;
        mods.push_back(json(m));
    }
    json conflicts = json::array();
    if (last)
        for (const auto& c : last->conflicts) conflicts.push_back(json(ojson{{"what", c.what}, {"mods", c.mods}}));
    return json(ojson{{"folder", ustr(dir / ml::kModsDir)},
                      {"mods", mods},
                      {"conflicts", conflicts},
                      {"checked", last.has_value()},
                      {"fingerprint", last ? last->fingerprint : ""}});
}

void require_mod(const fs::path& dir, const std::string& mod) {
    for (const auto& f : ml::scan(dir))
        if (f.id() == mod) return;
    throw std::runtime_error("There is no mod " + pack::serde::debug_str(mod) + " in this profile's mods folder.");
}

}  // namespace

json Backend::list_profile_mods(const std::string& id) { return profile_mods(profile_dir(id)); }

json Backend::set_profile_mod(const std::string& id, const std::string& mod, bool on) {
    const fs::path dir = profile_dir(id);
    require_mod(dir, mod);
    ml::set_on(dir, mod, on);
    return profile_mods(dir);
}

// Moves a mod `by` places in the load order (negative: earlier), saving the whole order.
json Backend::move_profile_mod(const std::string& id, const std::string& mod, int by) {
    const fs::path dir = profile_dir(id);
    require_mod(dir, mod);
    auto order = ml::load_order(ml::scan(dir), launch::profiles::read_local(dir).mods);
    const auto it = std::find(order.begin(), order.end(), mod);
    const long from = it - order.begin();
    const long to = std::clamp<long>(from + by, 0, static_cast<long>(order.size()) - 1);
    if (to != from) {
        order.erase(order.begin() + from);
        order.insert(order.begin() + to, mod);
        ml::set_order(dir, order);
    }
    return profile_mods(dir);
}

// Loads the mods the way Play would, without starting the game: problems and conflicts now.
json Backend::check_profile_mods(const std::string& id) {
    const fs::path dir = profile_dir(id);
    const Phase p = phase();
    if (p == Phase::Launching || p == Phase::Running)
        throw std::runtime_error("The game is running; check the mods once it exits.");
    std::optional<fs::path> game_dir, exe;
    {
        std::lock_guard<std::mutex> lock(game_->mu);
        if (game_->setup) {
            game_dir = game_->setup->game_dir;
            exe = game_->setup->game_exe();
        }
    }
    if (!game_dir) throw std::runtime_error("The game folder has not been found yet.");
    ml::build(dir, *game_dir, [&] {
        std::error_code ec;
        return launch::signing::sha256_file(*exe, nullptr, ec).value_or("");
    });
    return profile_mods(dir);
}

void Backend::open_mods_folder(const std::string& id) {
    const fs::path dir = profile_dir(id) / ml::kModsDir;
    create_dirs(dir);
    host_.open(ustr(dir));
}

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
