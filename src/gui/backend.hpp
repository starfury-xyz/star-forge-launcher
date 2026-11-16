#pragma once

// Port of the STAR FORGE Launcher's Tauri backend (star-forge gui/src-tauri/src): the commands the
// HTML/JS interface invokes and the events it listens for. Windows, the tray, dialogs and opening
// links belong to the host (see Host); everything here is portable and runs without a window, so
// tests drive it directly.
//
// Commands take and return JSON the way Tauri passes them: arguments by their camelCase names,
// results as serde would write them, and a failed command as the error text the interface shows
// (a std::runtime_error here).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "launch/launch.hpp"
#include "launch/packs.hpp"
#include "launch/paths.hpp"
#include "launch/remote.hpp"
#include "gui/updater.hpp"

namespace sf::gui {

namespace fs = std::filesystem;
using nlohmann::json;

// What the backend needs from the window side. Called from any thread.
class Host {
public:
    virtual ~Host() = default;
    // Sends an event to the main window's interface.
    virtual void emit(const std::string& event, const json& payload) = 0;
    // Opens a folder, file or web link with the desktop's default program. Throws std::runtime_error.
    virtual void open(const std::string& target) = 0;
    // Folder and file pickers; nullopt when the player cancels.
    virtual std::optional<std::string> pick_folder(const std::string& title, const std::optional<std::string>& start) = 0;
    virtual std::optional<std::string> pick_file(const std::string& title, const std::string& filter_name,
                                                 const std::vector<std::string>& extensions) = 0;
    // Hides the launcher to the tray (or minimizes it without one). Throws std::runtime_error.
    virtual void hide_to_tray() = 0;
    // Brings the launcher back (game exit, a pack link).
    virtual void restore() = 0;
    // Ends the launcher; a running game keeps going.
    virtual void exit() = 0;
    // Puts a downloaded, verified launcher update in place (or starts its installer). Throws
    // std::runtime_error.
    virtual void install_update(const fs::path& package) = 0;
    // Starts the updated launcher and ends this one (an installer restarts it itself).
    virtual void restart() = 0;
};

// Sent to the interface as a number.
enum class NotificationLevel { Info = 0, Error = 2 };

enum class LaunchBehavior { Tray, KeepOpen, Exit };  // after Play
enum class CloseBehavior { Tray, Exit };             // the window's close button

// Launcher preferences, kept in <data dir>/launcher.json.
struct Preferences {
    std::string selected_profile;  // folder name of the profile the play button uses
    LaunchBehavior launch_behavior = LaunchBehavior::Tray;
    CloseBehavior close_behavior = CloseBehavior::Exit;
    std::string faction_theme = "starforge";  // starforge, starfury, empire, federation or jericho
    bool pack_branding = true;      // show the selected server pack's logo, icon, background and colour
    std::string background_folder;  // the player's own wallpapers; empty = Steam's banner
    std::string game_directory;     // empty = find through Steam
    std::string proton_path;        // Proton build folder (Linux, umu or steam-proton); empty = Steam's last
    std::string runner;             // empty = umu (Linux) / wine (macOS); steam-proton; or a Wine-style command
    std::string wine_prefix;        // prefix for umu and Wine-style runners; empty = <data dir>/prefixes/...
    bool check_updates = true;      // look for a new launcher version at startup
    bool prerelease_updates = false;  // also offer pre-release launcher versions (for testers)
    bool experimental = false;      // show unfinished features (the Mods and Servers pages)
    bool setup_done = false;        // the first-run setup was finished

    // As serde writes and reads it (camelCase, every field optional, unknown fields ignored).
    nlohmann::ordered_json to_json() const;
    static Preferences from_json(const json& j);  // throws pack::Error with serde's wording
};

// The play button's state, as the interface sees it.
enum class Phase { Checking, Ready, Launching, Running, Failed };

// What a download the player cancelled fails with.
inline constexpr const char* kCancelled = "download cancelled";

class Backend {
public:
    explicit Backend(Host& host);
    ~Backend();
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

    // Runs one interface command. Throws std::runtime_error with the message the interface shows.
    json invoke(const std::string& command, const json& args);

    // Reads launcher.json (once, at startup).
    void load_prefs();
    Preferences prefs() const;
    // Applies `f` to the preferences and saves them. Throws std::runtime_error.
    Preferences modify_prefs(const std::function<void(Preferences&)>& f);

    // The game is starting or running.
    bool busy() const;
    // Re-detects the game and Proton in the background.
    void refresh();
    // A starforge:// link: handed to the interface, or kept until it listens.
    void receive_link(const std::string& url);
    // Shows a message in the interface.
    void notify(const std::string& message, NotificationLevel level);

    // Folders searched for sc-inject.exe and sc_agent.dll after STAR_FORGE_TOOLS_DIR (by default
    // default_tool_folders()).
    std::vector<fs::path> tool_folders;
    // The launcher's own files (icons/ for the AppImage menu entry).
    fs::path resources;
    // Where the launcher looks for a newer version of itself (set before any command).
    updater::Config updater;

    // For tests: waits until no background work (detection, a launch) is running, except a
    // started game's wait.
    void wait_idle();

    // One download shown in the interface's download window (`download-progress` events).
    class Session;

private:
    struct Game;
    struct Reviews;
    struct DownloadState;
    struct Updates;

    // Starts `f` on its own thread, counted for wait_idle unless `idle`.
    void background(std::function<void()> f, bool idle = false);
    void update_game(const std::function<void(Game&)>& f);
    json snapshot() const;
    Phase phase() const;
    launch::Options options() const;
    std::optional<fs::path> tools_dir() const;
    std::optional<fs::path> game_exe() const;
    void start_game();
    void catch_up_settings(const launch::Setup& setup);
    void announce_update(const std::string& id, const launch::packs::Available& found);

    // The commands, by module (prefs.cpp, game.cpp, profiles.cpp, setup.cpp, mods.cpp, servers.cpp).
    json update_preferences(const json& args);
    json perform_action(const std::string& action);
    json detect_compat_tools();
    json get_artwork();
    json view_game_install();
    json view_launch_log();
    void cancel_download(uint64_t id);
    json list_profiles();
    json save_profile(const json& profile);
    void delete_profile(const std::string& id);
    void open_profile_folder(const std::optional<std::string>& id);
    void open_pack_link(const std::string& id, const std::string& key);
    json get_profile_branding(const std::string& id);
    json inspect_pack(const std::string& source);
    json install_pack(uint64_t token);
    void discard_pack(uint64_t token);
    json list_sample_packs();
    json sample_pack_source(const std::string& key);
    json check_pack_updates();
    json check_pack_update(const std::string& id);
    json take_pending_link();
    json get_setup_info();
    void finish_setup(const std::string& game_dir, const std::string& profiles_dir, bool move_profiles);
    void install_appimage(const std::string& dir);
    json list_mods(const std::string& kind, const std::string& id);  // mods.cpp
    json set_mod(const std::string& kind, const std::string& id, const std::string& module, const std::string& patch,
                 std::optional<bool> on);
    json list_servers();  // servers.cpp
    json add_server(const std::string& name, const std::string& cloud);
    void remove_server(const std::string& id);
    json server_status(const std::string& id);
    json install_server_proxy(const std::string& id);  // returns server_status
    json uninstall_server_proxy(const std::string& id);
    void start_server(const std::string& id);  // in the background; server_status shows the result
    void stop_server(const std::string& id);
    json server_logs(const std::string& id);
    json server_log(const std::string& id, const std::string& group, const std::string& name);  // its tail
    fs::path server_local_appdata() const;
    void open_server_folder(const std::string& id, const std::string& which);
    json suggest_server_cloud();
    json check_launcher_update();
    void install_launcher_update();
    void open_launcher_release();

    Host& host_;
    mutable std::mutex prefs_mu_;
    Preferences prefs_;
    std::unique_ptr<Game> game_;
    std::unique_ptr<Reviews> reviews_;
    std::unique_ptr<DownloadState> downloads_;
    std::unique_ptr<Updates> updates_;
    std::once_flag caught_up_;

    std::mutex idle_mu_;
    std::condition_variable idle_cv_;
    int working_ = 0;
};

class Backend::Session {
public:
    // `action` is "install" or "update"; `title` the pack's name.
    Session(Backend& backend, const char* action, std::string title);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // The progress callback for a sync; fails once the player cancels.
    std::optional<std::string> report(const launch::remote::SyncEvent& event);
    // The player pressed Cancel.
    bool cancelled() const;
    // Ends the download in the window, with what went wrong, if anything. Once.
    void finish(const std::optional<std::string>& error);

private:
    void emit(json message);

    Backend& backend_;
    uint64_t id_;
    const char* action_;
    std::string title_;
    std::shared_ptr<std::atomic<bool>> cancel_;
    bool shown_ = false;  // the window opened: there was something to download
    std::optional<std::chrono::steady_clock::time_point> last_tick_;
};

// Shared helpers (backend.cpp).
std::string trim(const std::string& s);
// An image as a data URL: PNG, JPEG or WebP by its contents, up to 8 MB.
std::optional<std::string> data_url(const fs::path& path);
// A number below `n` (> 0) that differs from run to run.
size_t random_below(size_t n);
// Path::starts_with: whole components.
bool path_starts_with(const fs::path& path, const fs::path& prefix);
// Where a launcher in `exe_dir` has its tools: in a deb or rpm (<prefix>/bin/star-forge),
// <prefix>/lib/STAR FORGE Launcher/tools, as Tauri's resource folder; else tools/ next to it, or
// right beside it.
std::vector<fs::path> default_tool_folders(const fs::path& exe_dir);
// "<path>: <why>" for a failed folder creation; throws std::runtime_error.
void create_dirs(const fs::path& dir);

}  // namespace sf::gui
