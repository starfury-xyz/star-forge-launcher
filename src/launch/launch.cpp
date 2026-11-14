#include "launch/launch.hpp"

#include <algorithm>

#include "launch/env.hpp"
#include "launch/steam.hpp"

namespace sf::launch {
namespace {

bool is_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}
bool is_dir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

bool iequals(const std::string& a, const char* b) {
    std::string x = a, y = b;
    for (auto* s : {&x, &y})
        for (auto& c : *s)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return x == y;
}

// The path without a trailing separator ("a/b/" -> "a/b"), so parent/filename behave like Rust's.
fs::path trimmed(const fs::path& p) { return p.has_filename() || !p.has_parent_path() ? p : p.parent_path(); }

SetupError start_once(const std::string& what) {
    return SetupError(what + " Start Star Conflict once from Steam so it sets up Proton and the game's prefix, close it, "
                             "then retry.",
                      SteamFix{SteamFix::Kind::StartOnce});
}

#ifndef _WIN32
// Sets up the Steam overlay the way Steam does for the games it starts: its renderer is preloaded
// and its Vulkan layer (which draws over DXVK) enabled.
void add_overlay(Command& cmd, const fs::path& steam_root) {
    std::vector<std::string> libs;
    for (const char* d : {"ubuntu12_32", "ubuntu12_64"}) {
        fs::path p = steam_root / d / "gameoverlayrenderer.so";
        if (is_file(p)) libs.push_back(ustr(p));
    }
    if (libs.empty()) return;
    std::string preload = get_env("LD_PRELOAD").value_or("");
    for (const auto& s : libs) {
        if (s.find_first_of(": ") != std::string::npos) continue;
        // Already there? Entries are separated by ':' or ' '.
        bool present = false;
        size_t start = 0;
        while (start <= preload.size() && !present) {
            size_t end = preload.find_first_of(": ", start);
            if (end == std::string::npos) end = preload.size();
            present = preload.compare(start, end - start, s) == 0 && end - start == s.size();
            start = end + 1;
        }
        if (present) continue;
        if (!preload.empty()) preload += ':';
        preload += s;
    }
    cmd.set_env("LD_PRELOAD", preload)
        .set_env("ENABLE_VK_LAYER_VALVE_steam_overlay_1", "1")
        .set_env("SteamOverlayGameId", steam::kGameAppid);
}
#endif

}  // namespace

std::string SteamFix::url() const {
    switch (kind) {
    case Kind::InstallGame: return std::string("steam://install/") + steam::kGameAppid;
    case Kind::StartOnce: return std::string("steam://rungameid/") + steam::kGameAppid;
    case Kind::InstallRuntime: return "steam://install/" + std::to_string(appid);
    }
    return "";
}

std::string SteamFix::label() const {
    switch (kind) {
    case Kind::InstallGame: return "Install from Steam";
    case Kind::StartOnce: return "Start once from Steam";
    case Kind::InstallRuntime: return "Install runtime from Steam";
    }
    return "";
}

// --- compat ---

std::optional<fs::path> proton_from_config_info(const std::string& text) {
    size_t start = text.find('\n');
    while (start != std::string::npos && start < text.size()) {
        size_t end = text.find('\n', start + 1);
        std::string line = text.substr(start + 1, end == std::string::npos ? std::string::npos : end - start - 1);
        if (size_t at = line.find("/files/"); at != std::string::npos) return upath(line.substr(0, at));
        start = end;
    }
    return std::nullopt;
}

std::vector<fs::path> proton_candidates(const std::vector<fs::path>& roots, const std::vector<fs::path>& libraries) {
    std::vector<fs::path> dirs;
    for (const auto& l : libraries) dirs.push_back(l / "steamapps" / "common");
    for (const auto& r : roots) dirs.push_back(r / "compatibilitytools.d");
    dirs.push_back("/usr/share/steam/compatibilitytools.d");
    std::vector<fs::path> out;
    for (const auto& d : dirs) {
        std::error_code ec;
        for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) {
            const fs::path p = it->path();
            if (is_file(p / "proton") && std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

fs::path runtime_for(const fs::path& proton_dir, const std::vector<fs::path>& libraries) {
    const fs::path dir = trimmed(proton_dir);
    const std::string name = dir.has_filename() ? ustr(dir.filename()) : ustr(dir);
    std::optional<uint32_t> appid;
    if (auto m = read_text(dir / "toolmanifest.vdf")) {
        auto ids = steam::vdf_values(*m, "require_tool_appid");
        // Like Rust's u32 parse: digits only, in range.
        if (!ids.empty() && !ids[0].empty() && ids[0].size() <= 10 &&
            ids[0].find_first_not_of("0123456789") == std::string::npos) {
            unsigned long long v = std::stoull(ids[0]);
            if (v <= 0xffffffffull) appid = static_cast<uint32_t>(v);
        }
    }
    if (!appid)
        throw SetupError(name +
                         " doesn't run inside the Steam Linux Runtime, so the launcher won't use it. Choose a current "
                         "Proton for Star Conflict in Steam (Properties \xe2\x86\x92 Compatibility), start the game once "
                         "from Steam, then retry.");
    if (auto app = steam::find_app(libraries, std::to_string(*appid)); app && is_file(app->dir / "run"))
        return app->dir / "run";
    throw SetupError(name + " needs the Steam Linux Runtime (Steam app " + std::to_string(*appid) +
                         "), which isn't installed. Install it from Steam, then retry.",
                     SteamFix{SteamFix::Kind::InstallRuntime, *appid});
}

Proton detect_proton(const fs::path& steam_root, const fs::path& game_library, const std::vector<fs::path>& libraries,
                     const std::optional<fs::path>& proton_override) {
    const fs::path compat_data = game_library / "steamapps" / "compatdata" / steam::kGameAppid;
    // Steam builds the prefix (and runs the game's first-time installers); the launcher only
    // ever reuses it.
    if (!is_dir(compat_data / "pfx")) throw start_once("Star Conflict hasn't been started from Steam on this computer yet.");
    fs::path dir;
    if (proton_override) {
        dir = *proton_override;
    } else {
        auto text = read_text(compat_data / "config_info");
        auto found = text ? proton_from_config_info(*text) : std::nullopt;
        if (!found) throw start_once("Steam hasn't recorded which Proton runs Star Conflict.");
        dir = *found;
    }
    if (!is_file(dir / "proton")) {
        if (proton_override) throw SetupError(ustr(dir) + " has no proton script; choose another Proton in Settings.");
        throw start_once("The Proton build Steam last used for Star Conflict (" + ustr(dir) + ") is gone.");
    }
    fs::path runtime = runtime_for(dir, libraries);
    return Proton{dir, runtime, compat_data, steam_root};
}

// --- launch ---

std::optional<fs::path> library_of(const fs::path& game_dir) {
    const fs::path g = trimmed(game_dir);
    if (!g.has_parent_path() || g.parent_path() == g) return std::nullopt;
    const fs::path common = g.parent_path();
    if (!common.has_parent_path() || common.parent_path() == common) return std::nullopt;
    const fs::path steamapps = common.parent_path();
    if (!iequals(ustr(common.filename()), "common") || !iequals(ustr(steamapps.filename()), "steamapps"))
        return std::nullopt;
    if (!steamapps.has_parent_path() || steamapps.parent_path() == steamapps) return std::nullopt;
    return steamapps.parent_path();
}

fs::path find_game(const Options& opts) {
    std::optional<fs::path> game_dir = opts.game_dir ? opts.game_dir : env::path(env::kGameDir);
    if (!game_dir) {
        auto app = steam::find_app(steam::libraries(steam::steam_roots()), steam::kGameAppid);
        if (!app)
            throw SetupError("Star Conflict was not found in any Steam library. Install it from Steam (or set the game "
                             "folder in Settings).",
                             SteamFix{SteamFix::Kind::InstallGame});
        game_dir = app->dir;
    }
    if (!steam::is_game_dir(*game_dir)) throw SetupError(ustr(*game_dir) + " does not look like a Star Conflict install");
    return *game_dir;
}

Setup resolve(const Options& given) {
    Options opts{given.game_dir ? given.game_dir : env::path(env::kGameDir),
                 given.tools_dir ? given.tools_dir : env::path(env::kToolsDir),
                 given.proton ? given.proton : env::path(env::kProton)};
    const auto roots = steam::steam_roots();
    const auto libraries = steam::libraries(roots);
    Setup setup;
    setup.game_dir = find_game(opts);

    if (opts.tools_dir) {
        setup.tools_dir = *opts.tools_dir;
    } else {
        fs::path exe = current_exe();
        if (exe.empty() || !exe.has_parent_path()) throw SetupError("cannot find this program's folder");
        setup.tools_dir = exe.parent_path();
    }
    for (const char* f : {kInjectExe, kAgentDll})
        if (!is_file(setup.tools_dir / f))
            throw SetupError(std::string(f) + " is missing from " + ustr(setup.tools_dir) +
                             " (pass --tools-dir or set STAR_FORGE_TOOLS_DIR)");

#ifndef _WIN32
    auto library = library_of(setup.game_dir);
    if (!library) throw SetupError("the game folder is not inside a Steam library, so its Proton prefix is unknown");
    const fs::path steam_root = roots.empty() ? *library : roots.front();
    setup.proton = detect_proton(steam_root, *library, libraries, opts.proton);
#endif
    return setup;
}

std::optional<std::string> inject_error(const fs::path& profile_dir) {
    auto text = read_text(profile_dir / "inject.log");
    if (!text) return std::nullopt;
    std::optional<std::string> found;
    size_t start = 0;
    while (start < text->size()) {  // the last one wins
        size_t end = text->find('\n', start);
        if (end == std::string::npos) end = text->size();
        std::string line = text->substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("error: ", 0) == 0) found = line.substr(7);
        start = end + 1;
    }
    return found;
}

std::string tool_path(const fs::path& path) {
    const std::string text = ustr(path);
#ifdef _WIN32
    return text;
#else
    bool ok = path.is_absolute() && text.find_first_of(std::string(":\\\0", 3)) == std::string::npos;
    for (const auto& part : path)
        if (part == "..") ok = false;
    if (!ok) throw std::runtime_error(text + ": Wine needs an absolute path without ':' or '\\'");
    // Wine's Z: drive maps the Unix root.
    std::string out = "Z:" + text;
    std::replace(out.begin(), out.end(), '/', '\\');
    return out;
#endif
}

namespace {

// `exe args`, one of our Windows tools: as it is on Windows; on Linux in the game's Proton prefix,
// with Proton inside the Steam Linux Runtime it asks for, as under Steam (detect_proton refuses
// builds without one). `verb` is Proton's (run, waitforexitandrun); `shared` are folders the
// tool needs besides the prefix, the game and the tools.
Command tool_command(const Setup& setup, const char* verb, const fs::path& exe, const std::vector<std::string>& args,
                     std::initializer_list<const fs::path*> shared) {
    Command cmd;
#ifdef _WIN32
    (void)verb;
    (void)shared;
    cmd.program = exe;
    cmd.args = args;
#else
    if (!setup.proton) throw std::runtime_error("no Proton setup");
    const Proton& pr = *setup.proton;
    cmd.program = pr.runtime;
    cmd.arg("--").arg(ustr(pr.dir / "proton"));
    // pressure-vessel (the runtime's container) only shares a fixed set of host paths read-write.
    std::string value = get_env("PRESSURE_VESSEL_FILESYSTEMS_RW").value_or("");
    std::vector<const fs::path*> dirs{&pr.compat_data, &setup.game_dir};
    dirs.insert(dirs.end(), shared.begin(), shared.end());
    dirs.push_back(&setup.tools_dir);
    for (const fs::path* d : dirs) {
        const std::string s = ustr(*d);
        if (s.find(':') != std::string::npos) throw std::runtime_error(s + ": paths shared with Proton cannot contain ':'");
        if (!value.empty()) value += ':';
        value += s;
    }
    cmd.set_env("PRESSURE_VESSEL_FILESYSTEMS_RW", value);
    cmd.arg(verb).arg(ustr(exe));
    for (const auto& a : args) cmd.arg(a);
    cmd.set_env("STEAM_COMPAT_DATA_PATH", ustr(pr.compat_data))
        .set_env("STEAM_COMPAT_CLIENT_INSTALL_PATH", ustr(pr.steam_root))
        .set_env("STEAM_COMPAT_APP_ID", steam::kGameAppid)
        .set_env("SteamAppId", steam::kGameAppid)
        .set_env("SteamGameId", steam::kGameAppid);
#endif
    // Applied last, like Rust's use_host_env: an AppImage's paths never reach the game.
    for (auto& change : env::host_env()) cmd.env.push_back(change);
    return cmd;
}

}  // namespace

Command command(const Setup& setup, const fs::path& profile_dir_in, const std::vector<std::string>& game_args) {
    std::error_code ec;
    fs::path profile_dir = fs::canonical(profile_dir_in, ec);
    if (ec) throw std::runtime_error(ustr(profile_dir_in) + ": " + ec.message());
#ifdef _WIN32
    std::string p = ustr(profile_dir);
    if (p.rfind("\\\\?\\", 0) == 0) profile_dir = upath(p.substr(4));
#endif

    const fs::path inject = setup.tools_dir / kInjectExe;
    std::vector<std::string> args{"--game",    tool_path(setup.game_exe()),
                                  "--profile", tool_path(profile_dir),
                                  "--dll",     tool_path(setup.tools_dir / kAgentDll)};
    if (!game_args.empty()) {
        args.push_back("--");
        args.insert(args.end(), game_args.begin(), game_args.end());
    }

    Command cmd = tool_command(setup, "waitforexitandrun", inject, args, {&profile_dir});
#ifndef _WIN32
    add_overlay(cmd, setup.proton->steam_root);
#endif
    cmd.cwd = setup.game_dir / "win64";
    return cmd;
}

Command server_command(const Setup& setup, const fs::path& cloud_dir, const std::string& verb) {
    const fs::path exe = setup.tools_dir / kServerExe;
    if (!is_file(exe)) throw std::runtime_error(std::string(kServerExe) + " is missing from " + ustr(setup.tools_dir));
    std::error_code ec;
    const fs::path cloud = fs::absolute(cloud_dir, ec);
    if (ec) throw std::runtime_error(ustr(cloud_dir) + ": " + ec.message());
    // `run`, not waitforexitandrun: the server can share the prefix with a game that's running.
    Command cmd = tool_command(setup, "run", exe, {verb, tool_path(cloud)}, {&cloud});
    cmd.cwd = cloud;
    return cmd;
}

}  // namespace sf::launch
