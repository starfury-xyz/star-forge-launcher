// STAR FORGE Launcher: the C++ host for the HTML/JS interface (port of main.rs and lifecycle.rs).
// The interface is unchanged: it talks to window.__TAURI__ (core.invoke, event.listen and three
// window actions; see gui/ui/js/launcher/bridge.js), which kShim below provides on top of
// webview bindings. Commands run in sf::gui::Backend on worker threads; window commands here.

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <thread>

#include "app/assets.hpp"
#include "app/lifecycle.hpp"
#include "app/native.hpp"
#include "gui/backend.hpp"
#include "launch/env.hpp"
#include "launch/packs.hpp"
#include "launch/profiles.hpp"

#ifdef _WIN32
#include <windows.h>

#include <shellapi.h>
#else
#include <unistd.h>

extern char** environ;
#endif

namespace sf::app {

namespace {

using gui::json;

// window.__TAURI__ for bridge.js, and data-tauri-drag-region (layout.js) as Tauri handles it.
const char* kShim = R"JS((() => {
  const listeners = new Map();
  let next = 1;
  window.__sf_emit = (event, payload) => {
    for (const [id, l] of [...listeners]) {
      if (l.event !== event) continue;
      try { l.callback({ event, id, payload }); } catch (error) { console.error(error); }
    }
  };
  const invoke = (command, args) => window.__sf_invoke(command, args ?? {});
  const listen = async (event, callback) => {
    const id = next++;
    listeners.set(id, { event, callback });
    return () => { listeners.delete(id); };
  };
  const action = (name) => () => window.__sf_window(name);
  const current = Object.freeze({
    minimize: action('minimize'), toggleMaximize: action('toggleMaximize'),
    close: action('close'), startDragging: action('startDragging'),
  });
  Object.defineProperty(window, '__TAURI__', {
    value: Object.freeze({ core: Object.freeze({ invoke }), event: Object.freeze({ listen }),
                           window: Object.freeze({ getCurrentWindow: () => current }) }),
  });
  document.addEventListener('mousedown', (event) => {
    if (event.button !== 0 || !(event.target instanceof Element) || !event.target.hasAttribute('data-tauri-drag-region')) return;
    event.preventDefault();
    window.__sf_window(event.detail === 2 ? 'toggleMaximize' : 'startDragging');
  });
})();)JS";

// JSON text for the page; text that isn't UTF-8 is replaced rather than failing the reply.
std::string dump(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

class App final : public gui::Host {
public:
    App() : backend_(*this) {}

    int run(const std::vector<std::string>& links) {
        // One launcher per data folder: a second one passes its links to the first and goes.
        const auto data = launch::profiles::data_dir();
        const std::string key = data ? data->u8string() : "";
        if (hand_over(key, links)) return 0;
        app::init();
        listen_for_instances(key, [this](const std::vector<std::string>& args) {
            if (trace_) std::fprintf(stderr, "another launcher handed over %zu argument(s)\n", args.size());
            restore();
            for (const auto& a : args) backend_.receive_link(a);
        });
        configure_updates();
        backend_.load_prefs();
        launch::packs::clear_staging(launch::profiles::default_profiles_dir());
        create_windows();
        if (data) state_file_ = *data / "window-state.json";
        load_window_state();
        if (const Asset* icon = find_asset("icons/128x128.png"))
            tray_ = Tray::create(std::string(reinterpret_cast<const char*>(icon->data), icon->size), "STAR FORGE Launcher",
                                 [this] { restore(); }, [] { app::quit(); });
        if (!tray_) std::fprintf(stderr, "system tray unavailable; the taskbar still works\n");
        for (const auto& l : links) backend_.receive_link(l);
        register_links_once();
        if (gate_.native_ready()) show_main();
        // The watchdog: show the launcher even if the interface never says it's ready.
        std::thread([this] {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            post([this] {
                if (gate_.watchdog()) {
                    std::fprintf(stderr, "startup watchdog revealed the launcher before the interface reported ready\n");
                    show_main();
                }
            });
        }).detach();
        app::run();
        save_window_state();
        // Like Tauri's app.exit: gone at once, whatever workers are still doing.
        std::fflush(nullptr);
        std::_Exit(0);
    }

    // --- gui::Host ---

    void emit(const std::string& event, const json& payload) override {
        // The events go to the main window only, as with emit_to("main", ...).
        main_->eval("window.__sf_emit && window.__sf_emit(" + dump(event) + "," + dump(payload) + ")");
    }
    void open(const std::string& target) override { app::open(target); }
    std::optional<std::string> pick_folder(const std::string& title, const std::optional<std::string>& start) override {
        return on_ui<std::optional<std::string>>([&] { return app::pick_folder(main_.get(), title, start); });
    }
    std::optional<std::string> pick_file(const std::string& title, const std::string& filter,
                                         const std::vector<std::string>& extensions) override {
        return on_ui<std::optional<std::string>>([&] { return app::pick_file(main_.get(), title, filter, extensions); });
    }
    void hide_to_tray() override {
        if (tray_) {
            on_ui<int>([&] {
                main_->hide();
                return 0;
            });
            return;
        }
        // Without a tray: minimized, and said so.
        on_ui<int>([&] {
            main_->minimize();
            return 0;
        });
        backend_.notify("System tray is unavailable. Restore STAR FORGE Launcher from the taskbar.", gui::NotificationLevel::Info);
    }
    void restore() override {
        post([this] {
            if (gate_.revealed || gate_.finish_sting()) show_main();
        });
    }
    void exit() override { post([] { app::quit(); }); }

    void install_update(const std::filesystem::path& package) override {
#ifdef _WIN32
        // The NSIS installer, as tauri-plugin-updater runs it in passive mode: progress only,
        // then it starts the launcher again (/R).
        const std::wstring file = package.wstring();
        const auto r = reinterpret_cast<INT_PTR>(
            ShellExecuteW(nullptr, L"open", file.c_str(), L"/P /R /UPDATE /ARGS", nullptr, SW_SHOW));
        if (r <= 32) throw std::runtime_error("could not start the installer (error " + std::to_string(r) + ")");
#else
        const auto appimage = launch::get_env("APPIMAGE");
        if (!appimage) throw std::runtime_error("only the AppImage updates itself");
        gui::updater::replace_file(*appimage, package);
#endif
    }

    void restart() override {
#ifdef _WIN32
        exit();  // the installer starts the new launcher
#else
        on_ui<int>([&] {
            save_window_state();
            return 0;
        });
        // The new AppImage in this process's place, with the desktop's environment (not this
        // AppImage's), as a fresh start from the menu would have it.
        const std::string appimage = launch::get_env("APPIMAGE").value_or("");
        std::map<std::string, std::string> vars;
        for (char** e = environ; *e; ++e) {
            const std::string kv = *e;
            const size_t eq = kv.find('=');
            if (eq != std::string::npos) vars[kv.substr(0, eq)] = kv.substr(eq + 1);
        }
        for (const auto& [k, v] : launch::env::host_env()) {
            if (v) vars[k] = *v;
            else vars.erase(k);
        }
        std::vector<std::string> strings;
        for (const auto& [k, v] : vars) strings.push_back(k + "=" + v);
        std::vector<char*> envp, argv{const_cast<char*>(appimage.c_str()), nullptr};
        for (auto& kv : strings) envp.push_back(kv.data());
        envp.push_back(nullptr);
        std::fflush(nullptr);
        execve(appimage.c_str(), argv.data(), envp.data());
        std::fprintf(stderr, "could not restart %s: %s\n", appimage.c_str(), std::strerror(errno));
        exit();
#endif
    }

private:
    // Runs `f` on the UI thread and waits for it (from a worker thread).
    template <class T, class F>
    T on_ui(F f) {
        std::promise<T> done;
        auto result = done.get_future();
        post([&] {
            try {
                done.set_value(f());
            } catch (...) {
                done.set_exception(std::current_exception());
            }
        });
        return result.get();
    }

    void create_windows() {
        WindowSpec m;
        m.title = "STAR FORGE Launcher";
        m.width = 1280;
        m.height = 720;
        m.min_width = 800;
        m.min_height = 450;
        m.frameless = true;
        main_ = std::make_unique<NativeWindow>(m);
        main_->init_script(kShim);
        main_->bind("__sf_invoke", [this](const std::string& id, const std::string& args) { invoke(id, args); });
        main_->bind("__sf_window", [this](const std::string& id, const std::string& args) { window_action(id, args); });
        main_->on_close_requested = [this] { close_main(); };
        if (const Asset* icon = find_asset("icons/128x128.png"))
            main_->set_icon_png(std::string(reinterpret_cast<const char*>(icon->data), icon->size));
        main_->load("index.html");

        WindowSpec s;
        s.title = "STAR FORGE Launcher";
        s.width = 420;
        s.height = 240;
        s.center = true;
        s.transparent = true;
        s.frameless = true;
        s.resizable = false;
        s.skip_taskbar = true;
        s.always_on_top = true;
        s.click_through = true;
        splash_ = std::make_unique<NativeWindow>(s);
        splash_->init_script(kShim);
        // The splash may only say it's done (main.rs refuses it every other command).
        splash_->bind("__sf_invoke", [this](const std::string& id, const std::string& args) {
            if (!splash_) return;  // a call queued before the splash closed
            const json a = json::parse(args, nullptr, false);
            if (a.is_array() && !a.empty() && a[0] == "splash_finished") {
                splash_->resolve(id, true, "null");
                if (gate_.finish_sting()) post([this] { show_main(); });
            } else {
                splash_->resolve(id, false, dump("This window cannot invoke launcher commands"));
            }
        });
        splash_->load("splash.html");
        splash_->show();
    }

    void show_main() {
        splash_.reset();
        main_->fit_to_monitor();
        main_->show();
        // Development aid: STAR_FORGE_SNAPSHOT=<file.png> saves the page a few seconds later.
        if (auto path = launch::get_env("STAR_FORGE_SNAPSHOT"))
            std::thread([this, p = *path] {
                std::this_thread::sleep_for(std::chrono::seconds(4));
                post([this, p] { main_->snapshot(p); });
            }).detach();
    }

    // Where updates come from: set when building (SF_UPDATE_ENDPOINT; none by default), with
    // tauri.conf.json's key and platform names.
    void configure_updates() {
        auto& u = backend_.updater;
        u.endpoint = SF_UPDATE_ENDPOINT;
        u.prerelease_endpoint = SF_PRERELEASE_ENDPOINT;
        u.pubkey = SF_UPDATE_PUBKEY;
        u.current = SF_VERSION;
#ifdef _WIN32
        u.targets = {"windows-x86_64-nsis", "windows-x86_64"};
#else
        u.targets = {"linux-x86_64-appimage", "linux-x86_64"};
        if (!u.endpoint.empty() && !launch::get_env("APPIMAGE"))
            u.unsupported = "Only the AppImage updates itself: update STAR FORGE Launcher the way you installed it.";
#endif
    }

    // The main window's size, position and maximized state, kept between runs like
    // tauri-plugin-window-state does.
    void load_window_state() {
        if (state_file_.empty()) return;
        std::ifstream in(state_file_);
        const json j = json::parse(in, nullptr, false);
        if (!j.is_object()) return;
        NativeWindow::State s;
        try {
            s.x = j.at("x");
            s.y = j.at("y");
            s.width = j.at("width");
            s.height = j.at("height");
            s.maximized = j.at("maximized");
        } catch (const std::exception&) {
            return;
        }
        if (s.width >= 800 && s.height >= 450) main_->set_state(s);  // never below the minimum size
    }

    void save_window_state() {
        if (state_file_.empty() || !main_) return;
        const NativeWindow::State s = main_->state();
        if (s.width <= 0 || s.height <= 0) return;
        try {
            launch::profiles::write_atomic(
                state_file_,
                json{{"x", s.x}, {"y", s.y}, {"width", s.width}, {"height", s.height}, {"maximized", s.maximized}}.dump(2) + "\n");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "could not save the window state: %s\n", e.what());
        }
    }

    // main.rs's setup_pack_links: packages register starforge:// when installed; a portable copy
    // (AppImage, development build) registers itself, which also points links at wherever it is
    // now. STAR_FORGE_NO_REGISTER leaves the desktop's handler alone (tests, side-by-side builds).
    void register_links_once() {
        if (launch::get_env("STAR_FORGE_NO_REGISTER")) return;
#ifndef _WIN32
        std::error_code ec;
        const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (!launch::get_env("APPIMAGE") && !ec && exe.string().rfind("/usr/", 0) == 0) return;
#endif
        std::thread([] {
            try {
                register_links();
            } catch (const std::exception& e) {
                std::fprintf(stderr, "could not register %s:// links: %s\n", launch::packs::kScheme, e.what());
            }
        }).detach();
    }

    // lifecycle.rs's close_main_window.
    void close_main() {
        if (backend_.prefs().close_behavior == gui::CloseBehavior::Tray) {
            std::thread([this] { hide_to_tray(); }).detach();
        } else {
            app::quit();
        }
    }

    void window_action(const std::string& id, const std::string& args) {
        const json a = json::parse(args, nullptr, false);
        const std::string action = a.is_array() && !a.empty() && a[0].is_string() ? a[0].get<std::string>() : "";
        if (action == "minimize") main_->minimize();
        else if (action == "toggleMaximize") main_->toggle_maximize();
        else if (action == "close") close_main();
        else if (action == "startDragging") main_->start_drag();
        else return main_->resolve(id, false, dump("unknown window action " + action));
        main_->resolve(id, true, "null");
    }

    // Commands from the main window: window and lifecycle ones here, the rest in the backend.
    void invoke(const std::string& id, const std::string& args) {
        const json a = json::parse(args, nullptr, false);
        if (!a.is_array() || a.empty() || !a[0].is_string()) return main_->resolve(id, false, dump("invalid command"));
        const std::string command = a[0];
        const json params = a.size() > 1 ? a[1] : json::object();
        if (trace_) std::fprintf(stderr, "invoke %s %s\n", command.c_str(), dump(params).c_str());
        if (command == "main_ready") {
            if (gate_.ready()) show_main();
            return main_->resolve(id, true, "null");
        }
        if (command == "splash_finished") {
            if (gate_.finish_sting()) show_main();
            return main_->resolve(id, true, "null");
        }
        if (command == "exit_launcher") {
            // A running game is not tied to the launcher, so exiting is always safe.
            main_->resolve(id, true, "null");
            return app::quit();
        }
        if (command == "app_version") return main_->resolve(id, true, dump(SF_VERSION));
        std::thread([this, id, command, params] {
            try {
                const json result = backend_.invoke(command, params);
                main_->resolve(id, true, dump(result));
            } catch (const std::exception& e) {
                main_->resolve(id, false, dump(e.what()));
            }
        }).detach();
    }

    gui::Backend backend_;
    const bool trace_ = launch::get_env("STAR_FORGE_DEVTOOLS").has_value();  // log commands to stderr
    StartupGate gate_;  // UI thread only
    std::unique_ptr<NativeWindow> main_, splash_;
    std::unique_ptr<Tray> tray_;  // set before any command runs
    std::filesystem::path state_file_;
};

}  // namespace

}  // namespace sf::app

#ifdef _WIN32
// A GUI program: no console window behind the launcher.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> links;
    for (int i = 1; i < argc; ++i) {
        const std::string a = std::filesystem::path(wargv[i]).u8string();
        if (a.rfind(std::string(sf::launch::packs::kScheme) + ":", 0) == 0) links.push_back(a);
    }
    LocalFree(wargv);
#else
int main(int argc, char** argv) {
    std::vector<std::string> links;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind(std::string(sf::launch::packs::kScheme) + ":", 0) == 0) links.push_back(a);
    }
#endif
    sf::launch::env::load_dotenv();
    try {
        static sf::app::App app;  // lives until exit: background threads may still use it
        return app.run(links);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "STAR FORGE Launcher could not start: %s\n", e.what());
        return 1;
    }
}
