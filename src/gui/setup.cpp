// Port of setup.rs: the first-run setup (where the game is, where profiles live, and for an
// AppImage, putting the launcher in the applications menu). It can run again from Settings.

#include <stdexcept>

#include "gui/internal.hpp"
#include "launch/process.hpp"
#include "launch/profiles.hpp"
#include "launch/steam.hpp"

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace sf::gui {

namespace profiles = launch::profiles;
using launch::upath;
using launch::ustr;
using ojson = nlohmann::ordered_json;

namespace {

std::optional<fs::path> detected_game() {
    try {
        return launch::find_game(launch::Options{});
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

json appimage() {
    auto path = launch::get_env("APPIMAGE");
    auto home = launch::get_env("HOME");
    if (!path || !home) return nullptr;
    const fs::path suggested = upath(*home) / "Applications";
    return ojson{{"path", *path},
                 {"suggestedDir", ustr(suggested)},
                 {"inPlace", upath(*path).parent_path() == suggested}};
}

void check_game_folder(const std::string& path) {
    if (!launch::steam::is_game_dir(upath(trim(path))))
        throw std::runtime_error(
            "That folder doesn't hold Star Conflict (no win64\\game.exe). Pick the folder Steam installed it in.");
}

}  // namespace

json Backend::get_setup_info() {
    const Preferences p = prefs();
    const std::string chosen = trim(p.game_directory);
    std::string game_dir;
    bool game_detected;
    if (chosen.empty()) {
        if (auto d = detected_game()) game_dir = ustr(*d);
        game_detected = true;
    } else {
        game_dir = chosen;
        game_detected = false;
    }
    const fs::path dir = profiles_dir();
    auto standard = profiles::standard_profiles_dir();
    auto data = profiles::data_dir();
    return ojson{
        {"needed", !p.setup_done},
        {"gameDir", game_dir},
        {"gameDetected", game_detected && !game_dir.empty()},
        {"profilesDir", ustr(dir)},
        {"standardProfilesDir", standard ? ustr(*standard) : std::string()},
        {"dataDir", data ? ustr(*data) : std::string()},
        {"profileCount", profiles::list(dir).size()},
        {"appimage", appimage()},
    };
}

// Saves the setup. An empty game_dir, or Steam's own folder, keeps finding the game through
// Steam. With move_profiles, existing profiles move to a new profiles folder.
void Backend::finish_setup(const std::string& game_dir_in, const std::string& profiles_dir_in, bool move_profiles) {
    if (busy()) throw std::runtime_error("Close the game before changing the setup.");
    const std::string game_dir = trim(game_dir_in);
    if (!game_dir.empty()) check_game_folder(game_dir);
    const bool keep_detected = game_dir.empty() || detected_game() == upath(game_dir);

    const std::string chosen = trim(profiles_dir_in);
    if (chosen.empty() || upath(chosen).is_relative()) throw std::runtime_error("Pick a full path for the profiles folder.");
    const fs::path new_dir = upath(chosen);
    const fs::path old_dir = profiles_dir();
    if (new_dir != old_dir) {
        create_dirs(new_dir);
        if (move_profiles) profiles::move_profiles(old_dir, new_dir);
        const auto standard = profiles::standard_profiles_dir();
        profiles::set_profiles_location(standard != new_dir ? std::optional<fs::path>(new_dir) : std::nullopt);
    }

    modify_prefs([&](Preferences& p) {
        p.game_directory = keep_detected ? std::string() : game_dir;
        p.setup_done = true;
    });
    refresh();
}

namespace {

// The applications-menu entry and icons, as scripts/install-desktop-entry.sh writes them for
// development builds. The icons come from <resources>/icons.
void desktop_entry(const fs::path& exe, const fs::path& resources) {
    std::optional<fs::path> data;
    if (auto x = launch::get_env("XDG_DATA_HOME")) data = upath(*x);
    else if (auto h = launch::get_env("HOME")) data = upath(*h) / ".local/share";
    if (!data) throw std::runtime_error("cannot work out the applications folder");
    auto write = [](const fs::path& path, const std::string& bytes) {
        create_dirs(path.parent_path());
        launch::write_or_throw(path, bytes);
    };
    const std::pair<const char*, const char*> icons[] = {
        {"32x32", "32x32.png"}, {"128x128", "128x128.png"}, {"256x256", "128x128@2x.png"}, {"512x512", "icon.png"}};
    for (const auto& [size, file] : icons)
        write(*data / "icons/hicolor" / size / "apps/star-forge.png", launch::read_or_throw(resources / "icons" / file));
    const fs::path icon = *data / "icons/hicolor/256x256/apps/star-forge.png";
    // Icon is a full path: by name, KDE can pick a theme's "star" icon first.
    const std::string entry =
        "[Desktop Entry]\nType=Application\nName=STAR FORGE Launcher\nComment=Star Conflict profile launcher\n"
        "Exec=\"" + ustr(exe) + "\" %u\nIcon=" + ustr(icon) +
        "\nStartupWMClass=star-forge\nCategories=Game;\nMimeType=x-scheme-handler/starforge;\n";
    write(*data / "applications/star-forge.desktop", entry);
    const std::vector<std::vector<std::string>> commands{
        {"update-desktop-database", ustr(*data / "applications")},
        {"xdg-mime", "default", "star-forge.desktop", "x-scheme-handler/starforge"}};
    for (const auto& c : commands) {
        launch::Command cmd;
        cmd.program = c[0];
        cmd.args.assign(c.begin() + 1, c.end());
        try {
            launch::spawn(cmd).wait();
        } catch (const std::exception&) {
        }
    }
}

}  // namespace

// Moves the running AppImage into `dir`, adds it to the applications menu, and restarts it from
// there. Linux AppImage only.
void Backend::install_appimage(const std::string& dir_in) {
    auto appimage_env = launch::get_env("APPIMAGE");
    if (!appimage_env) throw std::runtime_error("This launcher isn't running as an AppImage.");
    const fs::path current = upath(*appimage_env);
    const fs::path dir = upath(trim(dir_in));
    if (dir.is_relative()) throw std::runtime_error("Pick a full path for the AppImage folder.");
    create_dirs(dir);
    if (current.filename().empty()) throw std::runtime_error("the AppImage has no file name");
    const fs::path target = dir / current.filename();
    std::error_code ec;
    if (target != current) {
        fs::rename(current, target, ec);
        if (ec) {
            fs::copy_file(current, target, fs::copy_options::overwrite_existing, ec);
            if (ec) throw std::runtime_error(launch::io_error(target, ec));
            fs::remove(current, ec);
            if (ec) throw std::runtime_error(launch::io_error(current, ec));
        }
    }
#ifndef _WIN32
    fs::permissions(target, static_cast<fs::perms>(0755), ec);
    if (ec) throw std::runtime_error(launch::io_error(target, ec));
#endif
    desktop_entry(target, resources);
    if (target != current) {
        // Start the moved copy once this one has gone (a second instance would only hand over
        // to this one and quit).
        launch::Command cmd;
        cmd.program = "sh";
        cmd.args = {"-c", "sleep 1; exec \"$0\"", ustr(target)};
        try {
            launch::spawn(cmd);
        } catch (const std::exception& e) {
            throw std::runtime_error(std::string("could not restart the launcher: ") + e.what());
        }
        host_.exit();
    }
}

}  // namespace sf::gui
