// STAR FORGE Launcher: the C++ host for the HTML/JS interface (port of main.rs and lifecycle.rs).
// The interface is unchanged: it talks to window.__TAURI__ (core.invoke, event.listen and three
// window actions; see gui/ui/js/launcher/bridge.js), which kShim below provides on top of
// webview bindings. Commands run in sf::gui::Backend on worker threads; window commands here.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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
        app::init();
        backend_.load_prefs();
        launch::packs::clear_staging(launch::profiles::default_profiles_dir());
        create_windows();
        for (const auto& l : links) backend_.receive_link(l);
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
        // No tray yet: minimized, as Tauri does without one.
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
        if (command == "check_launcher_update" || command == "install_launcher_update" || command == "open_launcher_release")
            return main_->resolve(id, false, dump("Development builds do not update themselves."));
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
