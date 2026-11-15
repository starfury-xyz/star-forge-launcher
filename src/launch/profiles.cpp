#include "launch/profiles.hpp"

#include <algorithm>
#include <stdexcept>

#include <toml.hpp>

#include "launch/env.hpp"

namespace sf::launch::profiles {
namespace {

// Folder name under the platform data folder; matches the Tauri identifier, so the webview's own
// storage lands in the same place.
constexpr const char* kDataFolder = "star-forge";
constexpr const char* kOldDataFolder = "sc-launcher";  // before the STAR FORGE rename
constexpr const char* kLegacyFile = "profile.toml";    // before server packs; converted on first load
constexpr const char* kProfilesLocation = "profiles-location";  // file naming a profiles folder the player chose

bool path_exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}
bool dir_exists(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}
bool file_exists(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

[[noreturn]] void fail(const fs::path& p, const std::error_code& ec) { throw std::runtime_error(io_error(p, ec)); }

void create_dirs(const fs::path& p) {
    std::error_code ec;
    fs::create_directories(p, ec);
    if (ec) fail(p, ec);
}

std::optional<fs::path> platform_data_root() {
#ifdef _WIN32
    if (auto v = get_env("APPDATA")) return upath(*v);
    return std::nullopt;
#elif defined(__APPLE__)
    if (auto h = get_env("HOME")) return upath(*h) / "Library/Application Support";
    return std::nullopt;
#else
    // (An empty XDG_DATA_HOME counts as unset, as the XDG spec says; Rust would use "".)
    if (auto v = get_env("XDG_DATA_HOME"); v && !v->empty()) return upath(*v);
    if (auto h = get_env("HOME")) return upath(*h) / ".local/share";
    return std::nullopt;
#endif
}

void copy_dir(const fs::path& from, const fs::path& to) {
    create_dirs(to);
    std::error_code ec;
    fs::directory_iterator it(from, ec);
    if (ec) fail(from, ec);
    for (fs::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) fail(from, ec);
        const fs::path src = it->path(), dst = to / src.filename();
        if (it->is_directory(ec)) {
            copy_dir(src, dst);
        } else if (!fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec)) {
            throw std::runtime_error(ustr(src) + " -> " + ustr(dst) + ": " + ec.message());
        }
    }
}

// Converts a profile.toml profile to pack.json + local.json, keeping the old file as
// profile.toml.migrated.
void migrate_legacy(const fs::path& dir) {
    const fs::path legacy = dir / kLegacyFile;
    if (path_exists(dir / pack::kPackFile) || !file_exists(legacy)) return;
    const std::string text = read_or_throw(legacy);
    toml::table t;
    try {
        t = toml::parse(text);
    } catch (const toml::parse_error& e) {
        throw std::runtime_error(std::string(kLegacyFile) + ": " + std::string(e.description()));
    }
    for (const auto& [k, v] : t) {
        const std::string key(k.str());
        if (key != "name" && key != "server" && key != "local_server" && key != "args" && key != "trace_files" && key != "patch")
            throw std::runtime_error(std::string(kLegacyFile) + ": unknown field `" + key + "`");
    }
    if (const auto* patches = t["patch"].as_array(); patches && !patches->empty())
        throw std::runtime_error(std::string(kLegacyFile) +
                                 " has [[patch]] entries, which this launcher no longer reads: move them to a "
                                 "patches/game.exe.patchset.toml (see templates/pack/patches/README.md) and remove them from " +
                                 kLegacyFile);
    const std::string folder = ustr(dir.filename());
    const std::string name = t["name"].value_or(std::string());
    pack::Pack p;
    p.format = pack::kPackFormat;
    p.id = "local." + slug(folder);
    p.name = name.find_first_not_of(" \t\r\n") == std::string::npos ? folder : name;
    p.version = "1.0.0";
    if (auto s = t["server"].value<std::string>()) p.server.address = *s;
    p.server.local = t["local_server"].value_or(true);  // the old default
    if (const auto* args = t["args"].as_array())
        for (const auto& a : *args)
            if (auto s = a.value<std::string>()) p.game.args.push_back(*s);
    write_pack(dir, p);
    if (t["trace_files"].value_or(false)) {
        pack::Local l;
        l.trace_files = true;
        write_local(dir, l);
    }
    std::error_code ec;
    fs::rename(legacy, dir / (std::string(kLegacyFile) + ".migrated"), ec);
    if (ec) fail(legacy, ec);
}

}  // namespace

std::optional<fs::path> data_dir() {
    if (auto dir = env::path(env::kDataDir)) return dir;
    auto root = platform_data_root();
    if (!root) return std::nullopt;
    const fs::path dir = *root / kDataFolder, old = *root / kOldDataFolder;
    if (!path_exists(dir) && dir_exists(old)) {
        // Best effort: if the move fails the launcher just starts with fresh data.
        std::error_code ec;
        fs::rename(old, dir, ec);
    }
    return dir;
}

std::optional<fs::path> standard_profiles_dir() {
    auto d = data_dir();
    return d ? std::optional<fs::path>(*d / "profiles") : std::nullopt;
}

std::optional<fs::path> default_profiles_dir() {
    if (auto dir = env::path(env::kProfilesDir)) return dir;
    if (auto d = data_dir()) {
        if (auto text = read_text(*d / kProfilesLocation)) {
            std::string t = *text;
            const char* ws = " \t\r\n";
            size_t a = t.find_first_not_of(ws);
            if (a != std::string::npos) return upath(t.substr(a, t.find_last_not_of(ws) - a + 1));
        }
    }
    return standard_profiles_dir();
}

void set_profiles_location(const std::optional<fs::path>& dir) {
    auto data = data_dir();
    if (!data) throw std::runtime_error("cannot work out the launcher data folder");
    const fs::path file = *data / kProfilesLocation;
    if (dir) {
        create_dirs(*data);
        write_atomic(file, ustr(*dir));
    } else {
        std::error_code ec;
        fs::remove(file, ec);
        if (ec && !not_found(ec)) fail(file, ec);
    }
}

size_t move_profiles(const fs::path& from, const fs::path& to) {
    create_dirs(to);
    size_t moved = 0;
    for (const Entry& entry : list(from)) {
        const fs::path target = to / upath(entry.name);
        if (path_exists(target)) throw std::runtime_error(ustr(to) + " already has a profile called " + entry.name);
        std::error_code ec;
        fs::rename(entry.dir, target, ec);
        if (ec) {
            // Another drive: copy, then remove the original.
            copy_dir(entry.dir, target);
            fs::remove_all(entry.dir, ec);
            if (ec) fail(entry.dir, ec);
        }
        ++moved;
    }
    return moved;
}

bool valid_name(const std::string& name) {
    return !name.empty() && name != "." && name != ".." && name[0] != '.' &&
           name.find_first_of(std::string("/\\:\0", 4)) == std::string::npos;
}

std::string slug(const std::string& name) {
    std::string out;
    for (char c : name) {
        bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (alnum) out += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        else if (!out.empty() && out.back() != '-') out += '-';
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    out = out.substr(0, 40);
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "profile" : out;
}

std::string free_name(const fs::path& root, const std::string& name) {
    const std::string base = slug(name);
    std::string id = base;
    for (int n = 2; path_exists(root / upath(id)); ++n) id = base + "-" + std::to_string(n);
    return id;
}

bool is_profile(const fs::path& dir) { return file_exists(dir / pack::kPackFile) || file_exists(dir / kLegacyFile); }

pack::Pack read_pack(const fs::path& dir) { return pack::Pack::from_json(read_or_throw(dir / pack::kPackFile)); }

pack::Local read_local(const fs::path& dir) {
    std::error_code ec;
    auto text = read_text(dir / pack::kLocalFile, &ec);
    if (text) return pack::Local::from_json(*text);
    if (not_found(ec)) return {};
    fail(dir / pack::kLocalFile, ec);
}

void write_atomic(const fs::path& path, const std::string& text) {
    const fs::path tmp = path.parent_path() / upath("." + ustr(path.filename()) + ".tmp");
    write_or_throw(tmp, text);
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) fail(path, ec);
}

void write_local(const fs::path& dir, const pack::Local& local) { write_atomic(dir / pack::kLocalFile, local.to_json()); }

void write_pack(const fs::path& dir, const pack::Pack& p) {
    p.validate();
    write_atomic(dir / pack::kPackFile, p.to_json());
}

pack::Profile load(const fs::path& dir) {
    migrate_legacy(dir);
    return pack::Profile::load(dir);
}

std::vector<Entry> list(const fs::path& profiles_dir) {
    std::vector<Entry> out;
    std::error_code ec;
    for (fs::directory_iterator it(profiles_dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path dir = it->path();
        const std::string name = ustr(dir.filename());
        if (!valid_name(name) || !is_profile(dir)) continue;
        Entry e{name, dir, std::nullopt, "", std::nullopt};
        try {
            e.profile = load(dir);
        } catch (const std::exception& ex) {
            e.error = ex.what();
        }
        try {
            e.pack = read_pack(dir);
        } catch (const std::exception&) {
        }
        out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
    return out;
}

std::optional<fs::path> find_pack(const fs::path& profiles_dir, const std::string& id) {
    for (const Entry& e : list(profiles_dir))
        if (e.pack && e.pack->id == id) return e.dir;
    return std::nullopt;
}

// --- state.rs ---

std::optional<fs::path> shared_local_appdata(const Setup& setup) {
#ifdef _WIN32
    (void)setup;
    if (auto v = get_env("LOCALAPPDATA")) return upath(*v);
    return std::nullopt;
#else
    if (setup.wine) {
        // Proton-based runners (umu-run) name the user steamuser; Wine uses the login name.
        const fs::path users = setup.wine->prefix / "drive_c/users";
        if (dir_exists(users / "steamuser")) return users / "steamuser/AppData/Local";
        auto user = get_env("USER");
        if (!user || user->empty()) user = get_env("LOGNAME");
        if (!user || user->empty()) return std::nullopt;
        return users / upath(*user) / "AppData/Local";
    }
    if (!setup.proton) return std::nullopt;
    return setup.proton->compat_data / "pfx/drive_c/users/steamuser/AppData/Local";
#endif
}

fs::path profile_local_appdata(const fs::path& profile_dir) { return profile_dir / pack::kStateDir / "AppData" / "Local"; }

namespace {

// Copies whichever of `a` and `b` is newer over the other, if they differ.
void sync_file(const fs::path& a, const fs::path& b) {
    auto modified = [](const fs::path& p) -> std::optional<fs::file_time_type> {
        std::error_code ec;
        auto t = fs::last_write_time(p, ec);
        if (ec) return std::nullopt;
        return t;
    };
    const auto ta = modified(a), tb = modified(b);
    const fs::path *from, *to;
    if (!ta && !tb) return;
    if (ta && !tb) {
        from = &a;
        to = &b;
    } else if (!ta) {
        from = &b;
        to = &a;
    } else {
        if (read_text(a) == read_text(b)) return;
        bool a_newer = *ta >= *tb;
        from = a_newer ? &a : &b;
        to = a_newer ? &b : &a;
    }
    create_dirs(to->parent_path());
    std::error_code ec;
    if (!fs::copy_file(*from, *to, fs::copy_options::overwrite_existing, ec))
        throw std::runtime_error(ustr(*from) + " -> " + ustr(*to) + ": " + ec.message());
}

}  // namespace

void sync_shared(const fs::path& profile_dir, const pack::Profile& profile, const std::optional<fs::path>& shared) {
    if (!shared || !profile.own_game_data) return;
    const fs::path own = profile_local_appdata(profile_dir);
    for (const auto& rel : pack::kSharedStateFiles) sync_file(*shared / upath(rel), own / upath(rel));
}

void sync_all_shared(const fs::path& profiles_dir, const std::optional<fs::path>& shared) {
    for (const Entry& e : list(profiles_dir))
        if (e.profile) sync_shared(e.dir, *e.profile, shared);
}

}  // namespace sf::launch::profiles
