#include "launch/mods.hpp"

#include <algorithm>
#include <sstream>
#include <stdexcept>

#include <toml.hpp>

#include "launch/env.hpp"
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

// The DLL and its original as they are in a server install, and what that means.
struct ProxyFiles {
    std::optional<fs::path> dll, orig;
    ServerStatus::Proxy state = ServerStatus::NoDll;
};

ProxyFiles proxy_files(const fs::path& cloud_dir) {
    ProxyFiles f{find_file(cloud_dir, kProxyDll), find_file(cloud_dir, kProxyOrig)};
    if (!f.dll) f.state = f.orig ? ServerStatus::Broken : ServerStatus::NoDll;
    else if (auto text = read_text(*f.dll); !text) f.state = ServerStatus::Broken;
    else if (is_proxy(*text)) f.state = f.orig ? ServerStatus::Installed : ServerStatus::Broken;
    else f.state = f.orig ? ServerStatus::Restored : ServerStatus::Stock;
    return f;
}

ServerStatus server_status(const fs::path& cloud_dir) {
    ServerStatus st;
    st.proxy = proxy_files(cloud_dir).state;
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

bool is_proxy(const std::string& dll) {
    static const std::string marker("sfp_version", sizeof "sfp_version");  // the export's name, with its NUL
    return dll.find(marker) != std::string::npos;
}

std::optional<fs::path> find_proxy(const std::optional<fs::path>& tools_dir) {
    std::error_code ec;
    if (auto file = env::path(env::kServerProxy)) return fs::is_regular_file(*file, ec) ? file : std::nullopt;
    if (!tools_dir) return std::nullopt;
    const fs::path file = *tools_dir / "server" / kProxyDll;
    if (fs::is_regular_file(file, ec)) return file;
    return std::nullopt;
}

ServerStatus::Proxy install_proxy(const fs::path& cloud_dir, const fs::path& proxy) {
    const std::string build = read_or_throw(proxy);
    if (!is_proxy(build)) throw std::runtime_error(ustr(proxy) + " is not a STAR FORGE server proxy build");
    const ProxyFiles f = proxy_files(cloud_dir);
    switch (f.state) {
    case ServerStatus::NoDll:
        throw std::runtime_error("no " + std::string(kProxyDll) + " in " + ustr(cloud_dir) + ": is this the game's cloud folder?");
    case ServerStatus::Broken:
        throw std::runtime_error(ustr(cloud_dir) + " has the proxy without the original " + kProxyDll +
                                 " (or the other way round); verify the game files in Steam, then install again");
    case ServerStatus::Stock:
    case ServerStatus::Restored: {
        // The game's DLL becomes the original; on a failure below it goes back.
        const fs::path orig = cloud_dir / kProxyOrig;
        std::error_code ec;
        if (f.orig && *f.orig != orig) fs::remove(*f.orig, ec);  // another spelling of the stale copy
        fs::rename(*f.dll, orig, ec);
        if (ec) throw std::runtime_error(io_error(*f.dll, ec));
        try {
            profiles::write_atomic(*f.dll, build);
        } catch (...) {
            fs::rename(orig, *f.dll, ec);
            throw;
        }
        break;
    }
    case ServerStatus::Installed: profiles::write_atomic(*f.dll, build); break;
    }
    const fs::path root = server_root(cloud_dir);
    for (const fs::path& dir : {root / kServerPatchsets, root / "logs"}) {
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) throw std::runtime_error(io_error(dir, ec));
    }
    std::error_code ec;
    if (!fs::exists(root / kServerConfig, ec)) write_server_config(cloud_dir, ServerConfig{});
    return f.state;
}

void uninstall_proxy(const fs::path& cloud_dir) {
    const ProxyFiles f = proxy_files(cloud_dir);
    std::error_code ec;
    switch (f.state) {
    case ServerStatus::Installed: fs::rename(*f.orig, *f.dll, ec); break;
    case ServerStatus::Restored: fs::remove(*f.orig, ec); break;
    case ServerStatus::Stock: throw std::runtime_error("the proxy isn't installed in " + ustr(cloud_dir));
    case ServerStatus::NoDll: throw std::runtime_error("no " + std::string(kProxyDll) + " in " + ustr(cloud_dir));
    case ServerStatus::Broken:
        throw std::runtime_error(ustr(cloud_dir) + " has the proxy without the original " + kProxyDll + "; verify the game files in Steam");
    }
    if (ec) throw std::runtime_error(io_error(*f.orig, ec));
}

}  // namespace sf::launch::mods
