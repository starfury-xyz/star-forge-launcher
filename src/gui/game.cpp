// Port of game.rs: game detection, launch state and the play button, and the play screen's art.

#include <algorithm>
#include <cstdio>
#include <stdexcept>

#include "gui/internal.hpp"
#include "launch/diagnostics.hpp"
#include "launch/env.hpp"
#include "launch/gamescope.hpp"
#include "launch/prepare.hpp"
#include "launch/process.hpp"
#include "launch/profiles.hpp"
#include "launch/steam.hpp"

namespace sf::gui {

using launch::ustr;

namespace {

const char* phase_name(Phase p) {
    switch (p) {
    case Phase::Checking: return "checking";
    case Phase::Ready: return "ready";
    case Phase::Launching: return "launching";
    case Phase::Running: return "running";
    case Phase::Failed: return "failed";
    }
    return "checking";
}

std::optional<fs::path> launch_log() {
    auto dir = launch::profiles::data_dir();
    if (!dir) return std::nullopt;
    return *dir / "game-launch.log";
}

json opt_str(const std::optional<std::string>& s) { return s ? json(*s) : json(nullptr); }

}  // namespace

json Backend::snapshot() const {
    std::lock_guard<std::mutex> lock(game_->mu);
    const Game& g = *game_;
    const Phase phase = g.phase.value_or(Phase::Checking);
    std::optional<std::string> proton, runner;
#ifndef _WIN32
    if (g.setup && g.setup->proton) {
        const auto& p = *g.setup->proton;
        const fs::path runtime = p.runtime.parent_path().filename();
        proton = ustr(p.dir) + " inside " + (runtime.empty() ? std::string("the Steam Linux Runtime") : ustr(runtime));
        runner = proton;
    }
    if (g.setup && g.setup->wine) {
        const auto& w = *g.setup->wine;
        if (w.umu)
            runner = "umu-launcher with " + (w.proton ? ustr(*w.proton) : std::string("umu's own Proton")) +
                     ", prefix " + ustr(w.prefix);
        else
            runner = ustr(w.program) + ", prefix " + ustr(w.prefix);
    }
#endif
    json action = nullptr;
    if (phase == Phase::Ready) action = "launch";
    else if (phase == Phase::Failed) action = g.fix && !g.fix_opened ? "fix" : "retry";
    return nlohmann::ordered_json{
        {"revision", g.revision},
        {"phase", phase_name(phase)},
        {"primaryAction", action},
        {"busy", phase == Phase::Checking || phase == Phase::Launching},
        {"blockingReason", opt_str(g.reason)},
        {"fixLabel", g.fix ? json(g.fix->label()) : json(nullptr)},
        {"gameDirectory", g.setup ? json(ustr(g.setup->game_dir)) : json(nullptr)},
        {"proton", opt_str(proton)},
        {"runner", opt_str(runner)},
    };
}

void Backend::update_game(const std::function<void(Game&)>& f) {
    json snap;
    {
        std::lock_guard<std::mutex> lock(game_->mu);
        f(*game_);
        ++game_->revision;
    }
    snap = snapshot();
    host_.emit("launcher-state-changed", snap);
}

Phase Backend::phase() const {
    std::lock_guard<std::mutex> lock(game_->mu);
    return game_->phase.value_or(Phase::Checking);
}

bool Backend::busy() const {
    const Phase p = phase();
    return p == Phase::Launching || p == Phase::Running;
}

std::optional<fs::path> Backend::game_exe() const {
    std::lock_guard<std::mutex> lock(game_->mu);
    if (!game_->setup) return std::nullopt;
    return game_->setup->game_exe();
}

// Where sc-inject.exe and sc_agent.dll are: STAR_FORGE_TOOLS_DIR, else the first of tool_folders
// that has both.
std::optional<fs::path> Backend::tools_dir() const {
    if (auto dir = launch::env::path(launch::env::kToolsDir)) return dir;
    for (const auto& d : tool_folders) {
        std::error_code ec;
        if (fs::is_regular_file(d / launch::kInjectExe, ec) && fs::is_regular_file(d / launch::kAgentDll, ec)) return d;
    }
    return std::nullopt;
}

launch::Options Backend::options() const {
    const Preferences p = prefs();
    auto path = [](const std::string& s) -> std::optional<fs::path> {
        const std::string t = trim(s);
        if (t.empty()) return std::nullopt;
        return launch::upath(t);
    };
    launch::Options o;
    o.game_dir = path(p.game_directory);
    o.tools_dir = tools_dir();
    o.proton = path(p.proton_path);
    if (const std::string r = trim(p.runner); !r.empty()) o.runner = r;
    o.wine_prefix = path(p.wine_prefix);
    return o;
}

void Backend::refresh() {
    if (busy()) return;
    update_game([](Game& g) {
        g.phase = Phase::Checking;
        g.reason.reset();
        g.fix.reset();
        g.fix_opened = false;
    });
    background([this] {
        std::optional<launch::Setup> setup;
        std::string reason;
        std::optional<launch::SteamFix> fix;
        try {
            setup = launch::resolve(options());
        } catch (const launch::SetupError& e) {
            reason = e.what();
            fix = e.fix;
        } catch (const std::exception& e) {
            reason = e.what();
        }
        if (setup) catch_up_settings(*setup);
        update_game([&](Game& g) {
            g.setup = setup;
            if (setup) {
                g.phase = Phase::Ready;
            } else {
                g.phase = Phase::Failed;
                g.reason = reason;
                g.fix = fix;
            }
        });
    });
}

// Once per launcher run: saves settings from a game session the launcher didn't wait for (the
// "Exit" launch behaviour quits before the game does).
void Backend::catch_up_settings(const launch::Setup& setup) {
    std::call_once(caught_up_, [&] {
        try {
            launch::profiles::sync_all_shared(profiles_dir(), launch::profiles::shared_local_appdata(setup));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "could not sync game settings between profiles: %s\n", e.what());
        }
    });
}

void Backend::start_game() {
    std::optional<launch::Setup> setup;
    {
        std::lock_guard<std::mutex> lock(game_->mu);
        setup = game_->setup;
    }
    if (!setup) throw std::runtime_error("the game setup has not been detected yet");
    const std::string selected = prefs().selected_profile;
    if (selected.empty() || !launch::profiles::valid_name(selected))
        throw std::runtime_error("Choose a server profile first.");
    const fs::path dir = profiles_dir() / launch::upath(selected);
    std::string pack_name = selected;
    try {
        pack_name = launch::profiles::read_pack(dir).name;
    } catch (const std::exception&) {
    }

    auto download = std::make_unique<Session>(*this, "update", pack_name);
    using launch::prepare::Event;
    launch::prepare::Prepared prepared = launch::prepare::prepare(*setup, dir, {}, [&](const Event& e) -> std::optional<std::string> {
        switch (e.kind) {
        case Event::Note: notify(e.text, NotificationLevel::Info); break;
        case Event::Update: announce_update(selected, *e.update); break;
        case Event::Sync: return download ? download->report(*e.sync) : std::nullopt;
        case Event::SyncDone:
            if (download) {
                download->finish(e.sync_error);
                download.reset();
            }
            break;
        }
        return std::nullopt;
    });
    download.reset();

    // Proton and the injector are chatty; keep their output for troubleshooting.
    launch::Output out{launch::Output::Kind::Null, {}};
    if (auto log = launch_log()) {
        std::error_code ec;
        fs::create_directories(log->parent_path(), ec);
        out = {launch::Output::Kind::File, *log};
    }
    std::shared_ptr<launch::Process> child;
    try {
        child = std::make_shared<launch::Process>(launch::spawn(prepared.command, out, true));
    } catch (const std::exception& e) {
        // spawn's "could not start <program>: <why>" names sc-inject or the runtime; the
        // launcher says what it was starting.
        const std::string m = e.what(), prefix = "could not start " + ustr(prepared.command.program) + ": ";
        throw std::runtime_error("could not start the game: " + (m.rfind(prefix, 0) == 0 ? m.substr(prefix.size()) : m));
    }
    update_game([](Game& g) { g.phase = Phase::Running; });

    // Steam Deck Game Mode: tag the game's windows so gamescope shows them (gamescope.hpp). The
    // launcher has to stay running for that, so "exit" keeps it open there.
    std::shared_ptr<launch::gamescope::WindowTagger> tagger;
    if (auto app = launch::gamescope::app_id()) tagger = std::make_shared<launch::gamescope::WindowTagger>(*app);

    switch (prefs().launch_behavior) {
    case LaunchBehavior::Tray:
        try {
            host_.hide_to_tray();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "could not hide the launcher: %s\n", e.what());
        }
        break;
    case LaunchBehavior::KeepOpen: break;
    case LaunchBehavior::Exit:
        if (tagger) break;
        // The game keeps running on its own.
        host_.exit();
        return;
    }

    auto shared = prepared.shared;
    auto profile = prepared.profile;
    background(
        [this, child, dir, shared, profile, tagger]() mutable {
            const std::optional<int> code = child->wait();
            tagger.reset();
            try {
                launch::profiles::sync_shared(dir, profile, shared);
            } catch (const std::exception& e) {
                notify(std::string("Could not save your game settings for other profiles: ") + e.what(), NotificationLevel::Error);
            }
            update_game([](Game& g) { g.phase = Phase::Ready; });
            if (code != 0) {
                if (code) std::fprintf(stderr, "game exited with exit status: %d\n", *code);
                else std::fprintf(stderr, "game exited with a signal\n");
                if (auto e = launch::inject_error(dir)) notify("The game did not start: " + *e, NotificationLevel::Error);
            }
            if (prefs().launch_behavior == LaunchBehavior::Tray) host_.restore();
        },
        true);
}

json Backend::perform_action(const std::string& action) {
    const Phase p = phase();
    if (action == "fix" && p == Phase::Failed) {
        std::optional<launch::SteamFix> fix;
        {
            std::lock_guard<std::mutex> lock(game_->mu);
            fix = game_->fix;
        }
        if (!fix) throw std::runtime_error("Nothing to fix in Steam.");
        host_.open(fix->url());
        update_game([](Game& g) { g.fix_opened = true; });
        return nullptr;
    }
    if (action == "retry" && (p == Phase::Failed || p == Phase::Ready)) {
        refresh();
        return nullptr;
    }
    if (action == "launch" && p == Phase::Ready) {
        update_game([](Game& g) { g.phase = Phase::Launching; });
        // Off the interface's thread: checks hash the pack and may ask its server.
        background([this] {
            try {
                start_game();
            } catch (const std::exception& e) {
                update_game([](Game& g) { g.phase = Phase::Ready; });
                if (std::string(e.what()) == kCancelled)
                    notify("Download cancelled; the game was not started.", NotificationLevel::Info);
                else
                    notify(e.what(), NotificationLevel::Error);
            }
        });
        return nullptr;
    }
    throw std::runtime_error("This action is not available right now.");
}

json Backend::detect_compat_tools() {
#ifdef _WIN32
    return nlohmann::ordered_json{{"supported", false}, {"proton", json::array()}, {"wine", json::array()}};
#else
    const auto roots = launch::steam::steam_roots();
    json proton = json::array();
    for (const auto& p : launch::proton_candidates(roots, launch::steam::libraries(roots))) proton.push_back(ustr(p));
    return nlohmann::ordered_json{{"supported", true}, {"proton", proton}, {"wine", launch::wine_candidates()}};
#endif
}

namespace {

// A file from Steam's library artwork cache for the game, in either the current
// (<appid>/<hash>/<name>) or older (<appid>_<name>) layout.
std::optional<fs::path> steam_art(const std::string& name) {
    std::error_code ec;
    for (const auto& root : launch::steam::steam_roots()) {
        const fs::path cache = root / "appcache" / "librarycache";
        const fs::path flat = cache / launch::upath(std::string(launch::steam::kGameAppid) + "_" + name);
        if (fs::is_regular_file(flat, ec)) return flat;
        fs::directory_iterator it(cache / launch::steam::kGameAppid, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            const fs::path p = it->path();
            if (fs::is_regular_file(p / launch::upath(name), ec)) return p / launch::upath(name);
            if (ustr(p.filename()) == name && fs::is_regular_file(p, ec)) return p;
        }
    }
    return std::nullopt;
}

// A random image from the player's background folder (not its subfolders).
std::optional<std::string> folder_background(const std::string& dir) {
    if (trim(dir).empty()) return std::nullopt;
    std::vector<fs::path> images;
    std::error_code ec;
    fs::directory_iterator it(launch::upath(trim(dir)), ec), end;
    if (ec) return std::nullopt;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        const fs::path p = it->path();
        std::string ext = ustr(p.extension());
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::error_code fe;
        if (fs::is_regular_file(p, fe) && (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".webp"))
            images.push_back(p);
    }
    std::sort(images.begin(), images.end());
    if (images.empty()) return std::nullopt;
    // Start at a random image and take the first one that loads.
    const size_t start = random_below(images.size());
    for (size_t i = 0; i < images.size(); ++i)
        if (auto url = data_url(images[(start + i) % images.size()])) return url;
    return std::nullopt;
}

}  // namespace

// The default play screen art: a random image from the player's background folder, else Star
// Conflict's banner from the local Steam install (nothing is bundled).
json Backend::get_artwork() {
    auto hero = folder_background(prefs().background_folder);
    if (!hero)
        if (auto p = steam_art("library_hero.jpg")) hero = data_url(*p);
    std::optional<std::string> logo;
    if (auto p = steam_art("logo.png")) logo = data_url(*p);
    return nlohmann::ordered_json{{"hero", opt_str(hero)}, {"logo", opt_str(logo)}};
}

json Backend::view_game_install() {
    std::optional<fs::path> dir;
    {
        std::lock_guard<std::mutex> lock(game_->mu);
        if (game_->setup) dir = game_->setup->game_dir;
    }
    if (!dir) throw std::runtime_error("The game folder has not been found yet.");
    host_.open(ustr(*dir));
    return nullptr;
}

// The game writes its logs under LOCALAPPDATA: the profile's own state/ folder, or the shared one
// (the prefix's on Linux) for a profile that shares the game's data.
json Backend::view_game_logs() {
    const std::string selected = prefs().selected_profile;
    if (selected.empty() || !launch::profiles::valid_name(selected)) throw std::runtime_error("Choose a server profile first.");
    const fs::path dir = profiles_dir() / launch::upath(selected);
    bool own = true;
    try {
        own = pack::Profile::load(dir).own_game_data;
    } catch (const std::exception&) {
    }
    std::optional<fs::path> local;
    if (own) {
        local = launch::profiles::profile_local_appdata(dir);
    } else {
        std::lock_guard<std::mutex> lock(game_->mu);
        if (game_->setup) local = launch::profiles::shared_local_appdata(*game_->setup);
    }
    if (!local) throw std::runtime_error("The game folder has not been found yet.");
    fs::path logs = *local / launch::upath(launch::profiles::kGameDataFolder) / "logs";
    logs.make_preferred();  // kGameDataFolder has a /, which Explorer would be shown as is
    std::error_code ec;
    if (!fs::is_directory(logs, ec)) throw std::runtime_error("The game has not written any logs for this profile yet.");
    host_.open(ustr(logs));
    return nullptr;
}

json Backend::view_launch_log() {
    auto log = launch_log();
    std::error_code ec;
    if (!log || !fs::is_regular_file(*log, ec)) throw std::runtime_error("The game has not been started from the launcher yet.");
    host_.open(ustr(*log));
    return nullptr;
}

json Backend::save_diagnostics() {
    // Where first: collecting takes a moment (it hashes game.exe), and cancelling should be instant.
    std::optional<fs::path> start = launch::diagnostics::default_dir();
    if (!start) start = launch::profiles::data_dir();
    const auto picked = host_.pick_save_file("Save diagnostics", "Zip file", "zip",
                                             start ? std::optional<std::string>(ustr(*start)) : std::nullopt,
                                             launch::diagnostics::default_name());
    if (!picked) return nullptr;
    const fs::path out = launch::upath(*picked);
    launch::diagnostics::Input in{"STAR FORGE Launcher", launch::profiles::data_dir(), profiles_dir(), std::nullopt, ""};
    {
        std::lock_guard<std::mutex> lock(game_->mu);
        in.setup = game_->setup;
        if (!in.setup && game_->reason) in.setup_error = *game_->reason;
    }
    launch::diagnostics::save(launch::diagnostics::collect(in), out);
    return json(ustr(out));
}

}  // namespace sf::gui
