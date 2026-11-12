#include "launch/mods.hpp"

#include <algorithm>
#include <sstream>
#include <stdexcept>

#include <toml.hpp>

#include "launch/profiles.hpp"
#include "patch/core.hpp"
#include "patch/sha1.hpp"

namespace sf::launch::mods {
namespace {

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

bool ends_with_ci(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && lower(s.substr(s.size() - suffix.size())) == suffix;
}

// The switches for one module (module names compared ignoring case, like Windows).
const patch::Choices* choices_for(const std::map<std::string, patch::Choices>& all, const std::string& module) {
    for (const auto& [m, c] : all)
        if (lower(m) == lower(module)) return &c;
    return nullptr;
}

// A list under construction: the patchsets stay alive so conflicts can compare their writes.
struct Builder {
    List list;
    std::vector<patch::Patchset> sets;
    std::vector<std::string> sources;  // per set

    void add(patch::Patchset set, std::string source) {
        sets.push_back(std::move(set));
        sources.push_back(std::move(source));
    }

    // `pack_default(p)`: what a patch is without a switch.
    template <class Default>
    List finish(const std::map<std::string, patch::Choices>& switches, Default pack_default) {
        std::vector<const patch::Patch*> patches;
        for (size_t i = 0; i < sets.size(); ++i) {
            const patch::Patchset& set = sets[i];
            const patch::Choices* c = choices_for(switches, set.exe);
            for (const patch::Patch& p : set.patches) {
                Toggle t;
                t.module = set.exe;
                t.id = p.id;
                t.title = p.title;
                t.description = p.description;
                t.source = sources[i];
                t.build = set.build;
                t.default_on = pack_default(i, p);
                if (c)
                    if (auto it = c->find(p.id); it != c->end()) t.chosen = it->second;
                t.on = t.chosen.value_or(t.default_on);
                t.optional = p.optional;
                list.toggles.push_back(std::move(t));
                patches.push_back(&p);
            }
        }
        for (size_t a = 0; a < patches.size(); ++a)
            for (size_t b = 0; b < patches.size(); ++b) {
                Toggle& ta = list.toggles[a];
                const Toggle& tb = list.toggles[b];
                if (a != b && lower(ta.module) == lower(tb.module) && patch::overlap(*patches[a], *patches[b]))
                    ta.conflicts.push_back(tb.module + ":" + tb.id);
            }
        for (const auto& entry : switches)  // (a lambda can't capture a structured binding in C++17)
            for (const auto& choice : entry.second) {
                const std::string &module = entry.first, &id = choice.first;
                bool known = std::any_of(list.toggles.begin(), list.toggles.end(), [&](const Toggle& t) {
                    return lower(t.module) == lower(module) && t.id == id;
                });
                if (!known) list.warnings.push_back(module + ": no patch called \"" + id + "\"");
            }
        std::stable_sort(list.toggles.begin(), list.toggles.end(),
                         [](const Toggle& x, const Toggle& y) { return lower(x.module) < lower(y.module); });
        return std::move(list);
    }
};

void read_set(Builder& b, const fs::path& file, const std::string& source) {
    std::error_code ec;
    auto text = read_text(file, &ec);
    if (!text) {
        b.list.errors.push_back(io_error(file, ec));
        return;
    }
    try {
        b.add(patch::Patchset::parse(*text, source), source);
    } catch (const std::exception& e) {
        b.list.errors.push_back(e.what());
    }
}

// Sets or clears one switch in a module -> id -> on map, keeping the module's spelling if it's
// already there.
void set_switch(std::map<std::string, patch::Choices>& switches, const Toggle& t, std::optional<bool> on) {
    std::string key = t.module;
    for (const auto& [m, c] : switches)
        if (lower(m) == lower(t.module)) key = m;
    if (on) {
        switches[key][t.id] = *on;
        return;
    }
    auto it = switches.find(key);
    if (it == switches.end()) return;
    it->second.erase(t.id);
    if (it->second.empty()) switches.erase(it);
}

// find(), or an error naming `where`.
Toggle must_find(const List& list, const std::string& module, const std::string& id, const std::string& where) {
    if (const Toggle* t = find(list, module, id)) return *t;
    throw std::runtime_error(where + " has no patch \"" + id + "\" for " + module);
}

}  // namespace

const Toggle* find(const List& list, const std::string& module, const std::string& id) {
    for (const Toggle& t : list.toggles)
        if (lower(t.module) == lower(module) && t.id == id) return &t;
    return nullptr;
}

std::vector<std::string> clashes_on(const List& list, const std::string& module, const std::string& id) {
    std::vector<std::string> out;
    const Toggle* t = find(list, module, id);
    if (!t || !t->on) return out;
    for (const std::string& c : t->conflicts) {
        const size_t colon = c.find(':');
        if (const Toggle* o = find(list, c.substr(0, colon), c.substr(colon + 1)); o && o->on) out.push_back(c);
    }
    return out;
}

// --- client profiles ---

List client(const fs::path& profile_dir, const pack::Profile& profile) {
    Builder b;
    // Every core patch is listed, so the player can turn one on that the pack doesn't ask for.
    const size_t core_sets = [&] {
        for (patch::Patchset& set : patch::core::sets(patch::core::ids())) b.add(std::move(set), "core");
        return b.sets.size();
    }();
    for (const fs::path& file : pack::patchset_files(profile_dir))
        read_set(b, file, std::string(pack::kPatchesDir) + "/" + ustr(file.filename()));
    const auto& asked = profile.patches.core;
    return b.finish(profile.patch_choices, [&](size_t set, const patch::Patch& p) {
        if (set >= core_sets) return p.default_on;
        return p.default_on && std::find(asked.begin(), asked.end(), p.id) != asked.end();
    });
}

void set_client(const fs::path& profile_dir, const std::string& module, const std::string& id, std::optional<bool> on) {
    const pack::Profile profile = profiles::load(profile_dir);
    const Toggle t = must_find(client(profile_dir, profile), module, id, "this profile");
    pack::Local local = profiles::read_local(profile_dir);
    set_switch(local.patches, t, on);
    profiles::write_local(profile_dir, local);
}

// --- server installs ---

fs::path server_root(const fs::path& cloud_dir) { return cloud_dir / kServerDir; }

ServerConfig read_server_config(const fs::path& cloud_dir) {
    const fs::path file = server_root(cloud_dir) / kServerConfig;
    std::error_code ec;
    auto text = read_text(file, &ec);
    if (!text) {
        if (not_found(ec)) return {};
        throw std::runtime_error(io_error(file, ec));
    }
    return ServerConfig::parse(*text, kServerConfig);
}

void write_server_config(const fs::path& cloud_dir, const ServerConfig& config) {
    const fs::path root = server_root(cloud_dir);
    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec) throw std::runtime_error(io_error(root, ec));
    profiles::write_atomic(root / kServerConfig, config.to_toml());
}

// A server's patchsets (starforge/patchsets/*.patchset.toml, sorted), into `b`.
void read_server_sets(Builder& b, const fs::path& cloud_dir) {
    const fs::path dir = server_root(cloud_dir) / kServerPatchsets;
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && ends_with_ci(ustr(it->path().filename()), patch::kSuffix)) files.push_back(it->path());
    std::sort(files.begin(), files.end());
    for (const fs::path& file : files)
        read_set(b, file, std::string(kServerDir) + "/" + kServerPatchsets + "/" + ustr(file.filename()));
}

// A file in `dir` by name, ignoring case (server folders copied from Windows keep its spelling).
std::optional<fs::path> find_file(const fs::path& dir, const std::string& name) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (lower(ustr(it->path().filename())) == lower(name)) return it->path();
    return std::nullopt;
}

List server(const fs::path& cloud_dir) {
    Builder b;
    read_server_sets(b, cloud_dir);
    ServerConfig config;
    try {
        config = read_server_config(cloud_dir);
    } catch (const std::exception& e) {
        b.list.errors.push_back(e.what());
    }
    return b.finish(config.patches, [](size_t, const patch::Patch& p) { return p.default_on; });
}

ServerStatus server_status(const fs::path& cloud_dir) {
    ServerStatus st;
    const auto dll = find_file(cloud_dir, kProxyDll), orig = find_file(cloud_dir, kProxyOrig);
    if (!dll) st.proxy = orig ? ServerStatus::Broken : ServerStatus::NoDll;
    else if (!orig) st.proxy = ServerStatus::Stock;
    else {
        auto a = read_text(*dll), o = read_text(*orig);
        st.proxy = !a || !o ? ServerStatus::Broken : *a == *o ? ServerStatus::Restored : ServerStatus::Installed;
    }
    try {
        st.config = read_server_config(cloud_dir);
    } catch (const std::exception& e) {
        st.errors.push_back(e.what());
    }
    Builder b;
    read_server_sets(b, cloud_dir);
    st.errors.insert(st.errors.end(), b.list.errors.begin(), b.list.errors.end());
    for (const patch::Patchset& set : b.sets) {
        ExeCheck c;
        c.module = set.exe;
        c.build = set.build;
        std::error_code ec;
        std::optional<std::string> data;
        if (auto file = find_file(cloud_dir, set.exe)) data = read_text(*file, &ec);
        if (data) {
            c.sha1 = patch::sha1_hex(data->data(), data->size());
            const patch::Choices* ch = st.config ? choices_for(st.config->patches, set.exe) : nullptr;
            c.report = patch::verify_file(set, patch::Bytes(data->begin(), data->end()), ch);
        } else {
            c.report.errors.push_back(set.exe + " is not in " + ustr(cloud_dir));
        }
        st.exes.push_back(std::move(c));
    }
    return st;
}

void set_server(const fs::path& cloud_dir, const std::string& module, const std::string& id, std::optional<bool> on) {
    const Toggle t = must_find(server(cloud_dir), module, id, "this server");
    ServerConfig config = read_server_config(cloud_dir);
    set_switch(config.patches, t, on);
    write_server_config(cloud_dir, config);
}

}  // namespace sf::launch::mods
