// Port of profiles.rs: server profiles (list, add, edit, delete) and installing or updating
// server packs from the launcher.

#include <algorithm>
#include <cstdio>
#include <stdexcept>

#include "gui/internal.hpp"
#include "gui/samples.hpp"  // generated from samples/ by CMake
#include "launch/patching.hpp"
#include "launch/profiles.hpp"
#include "launch/trust.hpp"
#include "pack/serde.hpp"

namespace sf::gui {

namespace profiles = launch::profiles;
namespace packs = launch::packs;
namespace serde = pack::serde;
using launch::upath;
using launch::ustr;
using ojson = nlohmann::ordered_json;

namespace {

json opt_str(const std::optional<std::string>& s) { return s ? json(*s) : json(nullptr); }

const char* link_label(const std::string& key) {
    if (key == "website") return "Website";
    if (key == "discord") return "Discord";
    if (key == "news") return "News";
    return "Source";
}

const char* kAddressHint =
    "The server address can only use letters, digits, '.', '-', '_' and ':' (for example STARFURY.XYZ:3801).";
const char* kSchemaUrl = "https://raw.githubusercontent.com/starfury-xyz/star-forge-launcher/main/schemas/pack.schema.json";

// Throws kAddressHint when the profile's server address can't go in address.cfg.
void check_address(const pack::Pack& pack, const pack::Local& local) {
    try {
        pack::Profile::from(pack, local).address_cfg();
    } catch (const pack::Error&) {
        throw std::runtime_error(kAddressHint);
    }
}

// Removes an address.cfg this launcher generated, so clearing the server field really clears it.
// A hand-written file is left alone.
void remove_generated_address(const fs::path& dir) {
    const fs::path cfg = dir / pack::kFilesDir / "data" / "address.cfg";
    auto text = launch::read_text(cfg);
    if (!text) return;
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text->size()) {
        size_t end = text->find('\n', start);
        if (end == std::string::npos) end = text->size();
        std::string line = text->substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!trim(line).empty()) lines.push_back(line);
        start = end + 1;
    }
    if (lines.size() == 1 && lines[0].rfind("set net_masterServerAddress ", 0) == 0) {
        std::error_code ec;
        fs::remove(cfg, ec);
    }
}

// The game data choice lives in local.json for every kind of profile.
void set_shared_game_data(const fs::path& dir, bool shared) {
    pack::Local local = profiles::read_local(dir);
    if (local.shared_game_data != shared) {
        local.shared_game_data = shared;
        profiles::write_local(dir, local);
    }
}

void write_own_pack(const fs::path& dir, const pack::Pack& pack) {
    check_address(pack, pack::Local{});
    profiles::write_pack(dir, pack);
}

// A pack image as a data URL. The file must really sit inside the pack's resources/ (symlinks
// out are refused).
std::optional<std::string> resource_image(const fs::path& dir, const std::optional<std::string>& path) {
    if (!path) return std::nullopt;
    auto rel = pack::resource_rel(*path);
    if (!rel) return std::nullopt;
    std::error_code ec;
    const fs::path root = fs::canonical(dir / pack::kResourcesDir, ec);
    if (ec) return std::nullopt;
    const fs::path file = fs::canonical(root / upath(*rel), ec);
    if (ec || !path_starts_with(file, root)) return std::nullopt;
    return data_url(file);
}

constexpr size_t kFilesShown = 50;

// The community servers listed from the start, installed when first played: from the server's
// own published pack, or a pack.json kept with STAR FORGE (samples/).
struct Sample {
    const char* key;
    const char* url;   // Link; what the list shows comes from the fields below
    const char* json;  // Bundled; what the list shows comes from the pack
    const char* id;
    const char* name;
    const char* server;
    const char* description;
};
const Sample kSamples[] = {
    {"starfury", "https://starfury.web.garage.cosmosim.dev/pack.json", nullptr, "xyz.starfury", "STARFURY",
     "STARFURY.XYZ:3801", "STARFURY.XYZ community server."},
    {"sc-classic-europe", nullptr, samples::kScClassicEurope, nullptr, nullptr, nullptr, nullptr},
};

json update_available(const std::string& id, const packs::Available& a) {
    return ojson{{"id", id}, {"version", a.version}, {"url", a.url}};
}

}  // namespace

json Backend::list_profiles() {
    json out = json::array();
    for (const auto& e : profiles::list(profiles_dir())) {
        const pack::Pack* pk = e.pack ? &*e.pack : nullptr;
        json links = json::array();
        if (pk)
            for (const auto& [key, url] : pk->links.entries()) links.push_back(json(ojson{{"key", key}, {"label", link_label(key)}}));
        std::string name = e.name;
        json server = nullptr, error = nullptr;
        bool local_server = false, own_game_data = true;
        size_t patches = 0;
        if (e.profile) {
            const pack::Profile& p = *e.profile;
            const auto patching = launch::patching::summary(e.dir, p);
            if (!trim(p.name).empty()) name = p.name;
            server = opt_str(p.server);
            local_server = p.local_server;
            own_game_data = p.own_game_data;
            patches = patching.patches;
            if (!patching.errors.empty()) error = patching.errors.front();
        } else {
            error = e.error;
        }
        out.push_back(json(ojson{
            {"id", e.name},
            {"name", name},
            {"server", server},
            {"localServer", local_server},
            {"ownGameData", own_game_data},
            {"patches", patches},
            {"error", error},
            {"packId", pk ? json(pk->id) : json(nullptr)},
            {"version", pk ? json(pk->version) : json(nullptr)},
            {"description", pk ? pk->description : std::string()},
            {"managed", pk && pk->update.has_value()},
            {"links", links},
        }));
    }
    return out;
}

json Backend::save_profile(const json& input) {
    std::optional<std::string> in_id;
    std::string in_name, in_server;
    bool in_local = false, in_own = false;
    try {
        serde::Obj o(input, "ProfileInput", {"id", "name", "server", "localServer", "ownGameData"}, false);
        if (auto v = o.opt("id")) in_id = serde::str(*v);
        in_name = serde::str(o.req("name"));
        in_server = serde::str(o.req("server"));
        in_local = serde::boolean(o.req("localServer"));
        in_own = serde::boolean(o.req("ownGameData"));
    } catch (const pack::Error& e) {
        throw std::runtime_error(std::string("invalid args `profile` for command `save_profile`: ") + e.what());
    }

    const fs::path root = profiles_dir();
    const std::string name = trim(in_name);
    if (name.empty()) throw std::runtime_error("Give the profile a name.");
    size_t chars = 0;
    for (unsigned char c : name) chars += (c & 0xc0) != 0x80;
    if (chars > 64) throw std::runtime_error("The profile name is too long (64 characters at most).");
    const std::string server = trim(in_server);

    std::string id;
    if (in_id) {
        profile_dir(*in_id);
        id = *in_id;
    } else {
        id = profiles::free_name(root, name);
    }
    const fs::path dir = root / upath(id);

    if (in_id && profiles::read_pack(dir).update) {
        // A pack from a server operator: keep the player's changes in local.json so updates
        // don't undo them.
        const pack::Pack pk = profiles::read_pack(dir);
        pack::Local local = profiles::read_local(dir);
        local.name = name != pk.name ? std::optional<std::string>(name) : std::nullopt;
        const std::string pack_server = pk.server.address.value_or("");
        local.server = server != pack_server ? std::optional<std::string>(server) : std::nullopt;
        local.local_server = in_local != pk.server.local ? std::optional<bool>(in_local) : std::nullopt;
        local.shared_game_data = !in_own;
        check_address(pk, local);
        profiles::write_local(dir, local);
    } else if (in_id) {
        // The player's own pack: edit it directly.
        pack::Pack pk = profiles::read_pack(dir);
        pk.name = name;
        pk.server.address = server.empty() ? std::nullopt : std::optional<std::string>(server);
        pk.server.local = in_local;
        write_own_pack(dir, pk);
        set_shared_game_data(dir, !in_own);
    } else {
        pack::Pack pk;
        pk.schema = kSchemaUrl;
        pk.format = pack::kPackFormat;
        pk.id = "local." + profiles::slug(id);
        pk.name = name;
        pk.version = "1.0.0";
        pk.server.address = server.empty() ? std::nullopt : std::optional<std::string>(server);
        pk.server.local = in_local;
        create_dirs(dir);
        try {
            write_own_pack(dir, pk);
            set_shared_game_data(dir, !in_own);
        } catch (...) {
            std::error_code ec;
            fs::remove_all(dir, ec);
            throw;
        }
    }
    if (server.empty()) remove_generated_address(dir);
    modify_prefs([&](Preferences& p) {
        if (p.selected_profile.empty()) p.selected_profile = id;
    });
    return id;
}

void Backend::delete_profile(const std::string& id) {
    if (busy()) throw std::runtime_error("Close the game before deleting a profile.");
    const fs::path dir = profile_dir(id);
    std::optional<std::string> pack_id;
    try {
        pack_id = profiles::read_pack(dir).id;
    } catch (const std::exception&) {
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (ec) throw std::runtime_error(launch::io_error(dir, ec));
    // A reinstall starts over, so a server that changed its signing key can be installed again.
    if (pack_id) launch::trust::unpin(*pack_id);
    modify_prefs([&](Preferences& p) {
        if (p.selected_profile == id) p.selected_profile.clear();
    });
}

void Backend::open_profile_folder(const std::optional<std::string>& id) {
    fs::path dir;
    if (id) {
        dir = profile_dir(*id);
    } else {
        dir = profiles_dir();
        create_dirs(dir);
    }
    host_.open(ustr(dir));
}

// Opens one of a pack's links. The URL comes from the pack itself, never from the interface, so
// only http(s) links the pack declares can be opened.
void Backend::open_pack_link(const std::string& id, const std::string& key) {
    const pack::Pack pk = profiles::read_pack(profile_dir(id));
    std::optional<std::string> url;
    for (const auto& [k, u] : pk.links.entries())
        if (k == key) {
            url = u;
            break;
        }
    if (!url) throw std::runtime_error("This pack has no such link.");
    if (url->rfind("https://", 0) != 0 && url->rfind("http://", 0) != 0)
        throw std::runtime_error("Not opening " + *url + ": only web links are allowed.");
    host_.open(*url);
}

// The selected pack's artwork and colour; missing entries use the defaults.
json Backend::get_profile_branding(const std::string& id) {
    const fs::path dir = profile_dir(id);
    ojson out{{"logo", nullptr}, {"icon", nullptr}, {"background", nullptr}, {"accent", nullptr}};
    pack::Pack pk;
    try {
        pk = profiles::read_pack(dir);
    } catch (const std::exception&) {
        return out;
    }
    const auto& b = pk.branding;
    out["logo"] = opt_str(resource_image(dir, b.logo));
    out["icon"] = opt_str(resource_image(dir, b.icon));
    if (!b.background.empty()) out["background"] = opt_str(resource_image(dir, b.background[random_below(b.background.size())]));
    out["accent"] = opt_str(b.accent);
    return out;
}

namespace {

json review_of(uint64_t token, const packs::Staged& staged, const std::optional<fs::path>& game_exe) {
    const pack::Pack& pk = staged.pack;
    std::vector<std::string> warnings = staged.warnings;
    const pack::Profile plain = pack::Profile::from(pk, pack::Local{});
    if (game_exe) {
        if (auto w = packs::game_build_warning(pk, *game_exe)) warnings.push_back(*w);
        // Tells the player now, not at Play, when the patches can't apply to their game.
        try {
            for (auto& w : launch::patching::preflight(staged.root, plain, *game_exe)) warnings.push_back(w);
        } catch (const std::exception& e) {
            warnings.push_back(e.what());
        }
    }
    const auto patching = launch::patching::summary(staged.root, plain);
    json update = nullptr;
    if (auto dir = profiles::find_pack(profiles_dir(), pk.id)) {
        const pack::Pack old = profiles::read_pack(*dir);
        const auto old_files = packs::file_list(*dir);
        size_t added = 0, removed = 0;
        for (const auto& f : staged.files) added += !std::binary_search(old_files.begin(), old_files.end(), f);
        for (const auto& f : old_files) removed += !std::binary_search(staged.files.begin(), staged.files.end(), f);
        pack::Profile installed;
        try {
            installed = profiles::load(*dir);
        } catch (const std::exception&) {
        }
        update = ojson{
            {"profile", ustr(dir->filename())},
            {"name", old.name},
            {"version", old.version},
            {"server", opt_str(old.server.address)},
            {"patches", launch::patching::summary(*dir, installed).patches},
            {"filesAdded", added},
            {"filesRemoved", removed},
        };
    }
    std::string origin = staged.origin;
    if (auto root = packs::staging_root())
        if (path_starts_with(upath(staged.origin), *root / "samples")) origin = "Included with STAR FORGE";
    const auto& t = staged.trust;
    json files = json::array();
    for (size_t i = 0; i < staged.files.size() && i < kFilesShown; ++i) files.push_back(staged.files[i]);
    return ojson{
        {"token", token},
        {"id", pk.id},
        {"name", pk.name},
        {"version", pk.version},
        {"description", pk.description},
        {"authors", pk.authors},
        {"origin", origin},
        {"verified", staged.verified},
        {"signer", t.key ? json(t.key->fingerprint()) : json(nullptr)},
        {"publisher", t.kind == launch::trust::Trust::Known ? json(t.name) : json(nullptr)},
        {"newKey", t.kind == launch::trust::Trust::NewKey},
        {"runsCode", staged.reach.runs_code()},
        {"managed", pk.update.has_value()},
        {"server", opt_str(pk.server.address)},
        {"localServer", pk.server.local},
        {"args", pk.game.args},
        {"patches", patching.patches},
        {"plugins", patching.plugins},
        {"fileCount", staged.files.size()},
        {"files", files},
        {"filesHost", staged.remote ? json(staged.remote->host()) : json(nullptr)},
        {"downloadBytes", staged.remote && staged.remote->bytes() ? json(*staged.remote->bytes()) : json(nullptr)},
        {"warnings", warnings},
        {"update", update},
    };
}

}  // namespace

// Downloads or unpacks a pack and describes what installing it would do.
json Backend::inspect_pack(const std::string& source) {
    const packs::Source src = packs::Source::parse(source);
    const auto exe = game_exe();
    packs::Staged staged = packs::stage(src, profiles_dir());
    uint64_t token;
    {
        std::lock_guard<std::mutex> lock(reviews_->mu);
        token = ++reviews_->next;
    }
    json review;
    try {
        review = review_of(token, staged, exe);
    } catch (...) {
        packs::discard(staged);
        throw;
    }
    std::lock_guard<std::mutex> lock(reviews_->mu);
    reviews_->staged.emplace(token, std::move(staged));
    return review;
}

json Backend::install_pack(uint64_t token) {
    if (busy()) throw std::runtime_error("Close the game before installing or updating a pack.");
    std::optional<packs::Staged> staged;
    {
        std::lock_guard<std::mutex> lock(reviews_->mu);
        auto it = reviews_->staged.find(token);
        if (it != reviews_->staged.end()) {
            staged = std::move(it->second);
            reviews_->staged.erase(it);
        }
    }
    if (!staged) throw std::runtime_error("This pack review has expired; open the pack again.");
    const fs::path root = profiles_dir();
    packs::Installed installed;
    bool cancelled;
    {
        Session download(*this, "install", staged->pack.name);
        try {
            installed = packs::install(*staged, root, [&](const launch::remote::SyncEvent& e) { return download.report(e); });
        } catch (const std::exception& e) {
            download.finish(std::string(e.what()));
            throw;
        }
        cancelled = download.cancelled();
        download.finish(installed.warning);
    }
    if (cancelled)
        notify("Download cancelled. The pack is installed; its remaining game files download before you play.",
               NotificationLevel::Info);
    else if (installed.warning)
        notify(*installed.warning, NotificationLevel::Error);
    const std::string id = installed.name;
    modify_prefs([&](Preferences& p) { p.selected_profile = id; });
    return id;
}

void Backend::discard_pack(uint64_t token) {
    std::optional<packs::Staged> staged;
    {
        std::lock_guard<std::mutex> lock(reviews_->mu);
        auto it = reviews_->staged.find(token);
        if (it == reviews_->staged.end()) return;
        staged = std::move(it->second);
        reviews_->staged.erase(it);
    }
    packs::discard(*staged);
}

json Backend::list_sample_packs() {
    json out = json::array();
    for (const auto& s : kSamples) {
        std::string id, name, description;
        json server = nullptr;
        if (s.url) {
            id = s.id, name = s.name, server = s.server, description = s.description;
        } else {
            try {
                const pack::Pack pk = pack::Pack::from_json(s.json);
                id = pk.id, name = pk.name, server = opt_str(pk.server.address), description = pk.description;
            } catch (const std::exception&) {
                continue;
            }
        }
        out.push_back(json(ojson{{"key", s.key}, {"packId", id}, {"name", name}, {"server", server}, {"description", description}}));
    }
    return out;
}

// What the review should open for a sample: the server's pack link, or a folder holding the
// bundled pack.
json Backend::sample_pack_source(const std::string& key) {
    const Sample* s = std::find_if(std::begin(kSamples), std::end(kSamples), [&](const Sample& x) { return key == x.key; });
    if (s == std::end(kSamples)) throw std::runtime_error("unknown sample pack");
    if (s->url) return s->url;
    auto root = packs::staging_root();
    if (!root) throw std::runtime_error("cannot work out the launcher data folder");
    const fs::path dir = *root / "samples" / upath(key);
    create_dirs(dir);
    profiles::write_atomic(dir / pack::kPackFile, s->json);
    return ustr(dir);
}

// Asks every pack with an update address for a newer version. Packs that can't be reached are
// skipped quietly; this runs in the background.
json Backend::check_pack_updates() {
    json out = json::array();
    for (const auto& e : profiles::list(profiles_dir())) {
        if (!e.pack) continue;
        try {
            if (auto a = packs::check_update(*e.pack)) out.push_back(update_available(e.name, *a));
        } catch (const std::exception& err) {
            std::fprintf(stderr, "update check for %s: %s\n", e.name.c_str(), err.what());
        }
    }
    return out;
}

// Asks the selected profile's pack for a newer version. Unlike check_pack_updates, a failure is
// an error the player sees.
json Backend::check_pack_update(const std::string& id) {
    const pack::Pack pk = profiles::read_pack(profile_dir(id));
    if (!pk.update) throw std::runtime_error(pk.name + " doesn't publish updates.");
    auto a = packs::check_update(pk);
    return a ? json(update_available(id, *a)) : json(nullptr);
}

// Tells the interface a pack has a newer version (found at launch), so it can offer the update.
void Backend::announce_update(const std::string& id, const packs::Available& found) {
    host_.emit("pack-update-found", update_available(id, found));
}

}  // namespace sf::gui
