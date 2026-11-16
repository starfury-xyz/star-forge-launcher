// Port of prefs.rs: launcher preferences, kept in <data dir>/launcher.json.

#include <cstdio>
#include <stdexcept>

#include "gui/internal.hpp"
#include "launch/profiles.hpp"
#include "launch/steam.hpp"
#include "pack/serde.hpp"

namespace sf::gui {

namespace serde = pack::serde;

namespace {

const std::vector<const char*> kLaunch{"tray", "keepOpen", "exit"};
const std::vector<const char*> kClose{"tray", "exit"};
const std::vector<const char*> kFaction{"starforge", "starfury", "empire", "federation", "jericho"};

// A unit enum variant by name, worded as serde refuses the others.
size_t variant(const json& v, const std::vector<const char*>& names) {
    if (!v.is_string()) serde::invalid(v, "string or map");
    const std::string s = v.get<std::string>();
    for (size_t i = 0; i < names.size(); ++i)
        if (s == names[i]) return i;
    std::string expected;
    for (size_t i = 0; i < names.size(); ++i)
        expected += std::string(i == 0 ? "" : (names.size() == 2 ? " or " : ", ")) + "`" + names[i] + "`";
    throw pack::Error("unknown variant `" + s + "`, expected " + (names.size() > 2 ? "one of " : "") + expected);
}

std::optional<fs::path> file() {
    auto dir = launch::profiles::data_dir();
    if (!dir) return std::nullopt;
    return *dir / "launcher.json";
}

void save(const Preferences& p) {
    auto path = file();
    if (!path) throw std::runtime_error("cannot work out the launcher data folder");
    create_dirs(path->parent_path());
    fs::path tmp = *path;
    tmp += ".tmp";
    launch::write_or_throw(tmp, p.to_json().dump(2));
    std::error_code ec;
    fs::rename(tmp, *path, ec);
    if (ec) throw std::runtime_error(launch::io_error(*path, ec));
}

}  // namespace

nlohmann::ordered_json Preferences::to_json() const {
    return {{"selectedProfile", selected_profile},
            {"launchBehavior", kLaunch[static_cast<size_t>(launch_behavior)]},
            {"closeBehavior", kClose[static_cast<size_t>(close_behavior)]},
            {"factionTheme", faction_theme},
            {"packBranding", pack_branding},
            {"backgroundFolder", background_folder},
            {"gameDirectory", game_directory},
            {"protonPath", proton_path},
            {"runner", runner},
            {"winePrefix", wine_prefix},
            {"checkUpdates", check_updates},
            {"prereleaseUpdates", prerelease_updates},
            {"experimental", experimental},
            {"setupDone", setup_done}};
}

Preferences Preferences::from_json(const json& j) {
    serde::Obj o(j, "Preferences", {}, false);
    Preferences p;
    if (auto v = o.get("selectedProfile")) p.selected_profile = serde::str(*v);
    if (auto v = o.get("launchBehavior")) p.launch_behavior = static_cast<LaunchBehavior>(variant(*v, kLaunch));
    if (auto v = o.get("closeBehavior")) p.close_behavior = static_cast<CloseBehavior>(variant(*v, kClose));
    if (auto v = o.get("factionTheme")) p.faction_theme = kFaction[variant(*v, kFaction)];
    if (auto v = o.get("packBranding")) p.pack_branding = serde::boolean(*v);
    if (auto v = o.get("backgroundFolder")) p.background_folder = serde::str(*v);
    if (auto v = o.get("gameDirectory")) p.game_directory = serde::str(*v);
    if (auto v = o.get("protonPath")) p.proton_path = serde::str(*v);
    if (auto v = o.get("runner")) p.runner = serde::str(*v);
    if (auto v = o.get("winePrefix")) p.wine_prefix = serde::str(*v);
    if (auto v = o.get("checkUpdates")) p.check_updates = serde::boolean(*v);
    if (auto v = o.get("prereleaseUpdates")) p.prerelease_updates = serde::boolean(*v);
    if (auto v = o.get("experimental")) p.experimental = serde::boolean(*v);
    if (auto v = o.get("setupDone")) p.setup_done = serde::boolean(*v);
    return p;
}

void Backend::load_prefs() {
    std::optional<json> saved;
    if (auto path = file())
        if (auto text = launch::read_text(*path)) {
            json v = json::parse(*text, nullptr, false);
            if (!v.is_discarded()) saved = std::move(v);
        }
    // Launchers from before the first-run setup already have their folders.
    const bool upgraded = saved && !(saved->is_object() && saved->contains("setupDone"));
    Preferences p;
    if (saved) {
        try {
            p = Preferences::from_json(*saved);
        } catch (const pack::Error&) {
        }
    }
    p.setup_done |= upgraded;
    std::lock_guard<std::mutex> lock(prefs_mu_);
    prefs_ = p;
}

Preferences Backend::prefs() const {
    std::lock_guard<std::mutex> lock(prefs_mu_);
    return prefs_;
}

Preferences Backend::modify_prefs(const std::function<void(Preferences&)>& f) {
    std::lock_guard<std::mutex> lock(prefs_mu_);
    Preferences next = prefs_;
    f(next);
    save(next);
    prefs_ = next;
    return next;
}

json Backend::update_preferences(const json& update) {
    std::string key;
    json value;
    try {
        serde::Obj o(update, "PreferenceUpdate", {"key", "value"}, false);
        key = serde::str(o.req("key"));
        value = o.req("value");
    } catch (const pack::Error& e) {
        throw std::runtime_error(std::string("invalid args `update` for command `update_launcher_preferences`: ") + e.what());
    }
    // Round-trip through JSON so the value is checked against the field's type.
    json fields = prefs().to_json();
    if (!fields.contains(key)) throw std::runtime_error("unknown preference " + key);
    fields[key] = value;
    Preferences parsed;
    try {
        parsed = Preferences::from_json(fields);
    } catch (const pack::Error& e) {
        throw std::runtime_error("invalid value for " + key + ": " + e.what());
    }
    // Folders are picked, not typed: empty (the default) or a folder that exists.
    if ((key == "gameDirectory" || key == "winePrefix" || key == "backgroundFolder") && value.is_string()) {
        const std::string chosen = trim(value.get<std::string>());
        if (!chosen.empty()) {
            const fs::path dir = launch::upath(chosen);
            std::error_code ec;
            if (!dir.is_absolute() || !fs::is_directory(dir, ec)) throw std::runtime_error("invalid value for " + key + ": not a folder: " + chosen);
            if (key == "gameDirectory" && !launch::steam::is_game_dir(dir))
                throw std::runtime_error("That folder doesn't hold Star Conflict (no win64\\game.exe). Pick the folder Steam installed it in.");
        }
    }
    Preferences next = modify_prefs([&](Preferences& p) { p = parsed; });
    if (key == "gameDirectory" || key == "protonPath" || key == "runner" || key == "winePrefix") refresh();
    return next.to_json();
}

}  // namespace sf::gui
