#pragma once

// The launcher's native side, one implementation per platform: native_gtk.cpp (GTK 3 +
// WebKitGTK) and native_win.cpp (Win32 + WebView2). A window holds one webview (webview/webview)
// showing a page of the built-in interface (see assets.hpp), served from kOrigin with the
// launcher's Content-Security-Policy; navigating anywhere else is refused.
//
// Everything here runs on the UI thread unless it says otherwise.

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sf::app {

struct WindowSpec {
    std::string title;
    int width = 800, height = 600;
    int min_width = 0, min_height = 0;
    bool frameless = false;     // no title bar or borders (the page draws its own)
    bool transparent = false;   // see-through where the page is
    bool resizable = true;
    bool center = false;
    bool skip_taskbar = false;
    bool always_on_top = false;
    bool click_through = false;  // mouse events go to whatever is below
};

class NativeWindow {
public:
    // Creates the window and its webview, hidden. Throws std::runtime_error.
    explicit NativeWindow(const WindowSpec& spec);
    ~NativeWindow();
    NativeWindow(const NativeWindow&) = delete;
    NativeWindow& operator=(const NativeWindow&) = delete;

    // Script run at the start of every page, before the page's own.
    void init_script(const std::string& js);
    // window.<name>(...args) in the page calls `f(id, args as a JSON array)`; answer with resolve().
    void bind(const std::string& name, std::function<void(const std::string& id, const std::string& args)> f);
    // Settles the page's promise for call `id` with `json` (a rejection unless `ok`). Any thread.
    void resolve(const std::string& id, bool ok, const std::string& json);
    // Runs `js` in the page. Any thread.
    void eval(const std::string& js);
    // Shows a page of the interface ("index.html").
    void load(const std::string& page);

    void show();  // shown, un-minimized and focused
    void hide();
    void minimize();
    void toggle_maximize();
    void start_drag();  // moves with the mouse while its button is held
    bool visible() const;
    // Shrinks and moves the window to fit the monitor it's on (unless maximized).
    void fit_to_monitor();
    void set_icon_png(const std::string& png);
    // Development aid: saves what the page shows as a PNG.
    void snapshot(const std::string& png_path);

    // The window's own close button, Alt+F4, the taskbar's Close: the window stays unless this
    // closes it (by destroying the NativeWindow).
    std::function<void()> on_close_requested;

    void* handle() const;  // GtkWindow* / HWND

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;  // shared with callbacks queued for the UI thread, which check it's still there
};

// Origin the interface is served from.
extern const char* const kOrigin;
// Content-Security-Policy for its pages (tauri.conf.json's).
extern const char* const kCsp;

// Starts the platform (before any window). Throws std::runtime_error.
void init();
// Runs the UI loop until quit().
void run();
void quit();
// Runs `f` on the UI thread soon. Any thread.
void post(std::function<void()> f);

// Folder and file pickers, modal to `parent`; nullopt when cancelled.
std::optional<std::string> pick_folder(NativeWindow* parent, const std::string& title, const std::optional<std::string>& start);
std::optional<std::string> pick_file(NativeWindow* parent, const std::string& title, const std::string& filter_name,
                                     const std::vector<std::string>& extensions);
// Opens a folder, file or web link with the desktop's default program. Any thread. Throws
// std::runtime_error.
void open(const std::string& target);

}  // namespace sf::app
