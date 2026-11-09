// The launcher's native side on Linux: GTK 3 windows with WebKitGTK 4.1 (through webview/webview),
// the interface served on a custom URI scheme, GTK file choosers, and xdg-open.

#include <cstdlib>
#include <stdexcept>
#include <thread>

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

#include "webview.h"

#include "app/assets.hpp"
#include "app/native.hpp"
#include "launch/env.hpp"
#include "launch/process.hpp"

namespace sf::app {

const char* const kOrigin = "sf-ui://app";
const char* const kCsp =
    "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; font-src 'self'; connect-src 'none'; "
    "object-src 'none'; frame-src 'none'; base-uri 'self'; form-action 'none'";

namespace {

constexpr int kEdge = 6;  // pixels at a frameless window's edge that resize it

// Serves sf-ui://app/<path> from the built-in interface files, with the launcher's CSP.
void serve(WebKitURISchemeRequest* request, gpointer) {
    const std::string uri = webkit_uri_scheme_request_get_uri(request);
    const std::string origin = std::string(kOrigin) + "/";
    const Asset* asset = nullptr;
    if (uri.rfind(origin, 0) == 0) {
        std::string path = uri.substr(origin.size());
        path = path.substr(0, path.find_first_of("?#"));
        asset = find_asset(path.empty() ? "index.html" : path);
    }
    if (!asset) {
        GError* error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "not found");
        webkit_uri_scheme_request_finish_error(request, error);
        g_error_free(error);
        return;
    }
    GInputStream* stream = g_memory_input_stream_new_from_data(asset->data, static_cast<gssize>(asset->size), nullptr);
    WebKitURISchemeResponse* response = webkit_uri_scheme_response_new(stream, static_cast<gint64>(asset->size));
    webkit_uri_scheme_response_set_content_type(response, asset->mime);
    SoupMessageHeaders* headers = soup_message_headers_new(SOUP_MESSAGE_HEADERS_RESPONSE);
    soup_message_headers_append(headers, "Content-Security-Policy", kCsp);
    webkit_uri_scheme_response_set_http_headers(response, headers);  // takes the headers
    webkit_uri_scheme_request_finish_with_response(request, response);
    g_object_unref(response);
    g_object_unref(stream);
}

// Only the interface's own pages load; links elsewhere go nowhere (the interface opens them
// through the launcher).
gboolean decide_policy(WebKitWebView*, WebKitPolicyDecision* decision, WebKitPolicyDecisionType type, gpointer) {
    if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION && type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION)
        return FALSE;
    WebKitNavigationAction* action =
        webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(decision));
    const std::string uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(action));
    if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION && uri.rfind(std::string(kOrigin) + "/", 0) == 0) return FALSE;
    webkit_policy_decision_ignore(decision);
    return TRUE;
}

}  // namespace

struct NativeWindow::Impl {
    NativeWindow* owner = nullptr;
    GtkWidget* window = nullptr;
    std::unique_ptr<webview::webview> view;
    bool resizable = true, frameless = false;
    // The last mouse press on the page, for start_drag.
    gint press_button = 1;
    gdouble press_x = 0, press_y = 0;
    guint32 press_time = GDK_CURRENT_TIME;

    // The window edge under (x, y) in window coordinates, for resizing a frameless window.
    std::optional<GdkWindowEdge> edge_at(double x, double y) const {
        if (!frameless || !resizable || gtk_window_is_maximized(GTK_WINDOW(window))) return std::nullopt;
        int w = 0, h = 0;
        gtk_window_get_size(GTK_WINDOW(window), &w, &h);
        const bool l = x < kEdge, r = x >= w - kEdge, t = y < kEdge, b = y >= h - kEdge;
        if (t && l) return GDK_WINDOW_EDGE_NORTH_WEST;
        if (t && r) return GDK_WINDOW_EDGE_NORTH_EAST;
        if (b && l) return GDK_WINDOW_EDGE_SOUTH_WEST;
        if (b && r) return GDK_WINDOW_EDGE_SOUTH_EAST;
        if (t) return GDK_WINDOW_EDGE_NORTH;
        if (b) return GDK_WINDOW_EDGE_SOUTH;
        if (l) return GDK_WINDOW_EDGE_WEST;
        if (r) return GDK_WINDOW_EDGE_EAST;
        return std::nullopt;
    }
};

namespace {

gboolean on_press(GtkWidget*, GdkEventButton* e, gpointer data) {
    auto* impl = static_cast<NativeWindow::Impl*>(data);
    impl->press_button = static_cast<gint>(e->button);
    impl->press_x = e->x_root;
    impl->press_y = e->y_root;
    impl->press_time = e->time;
    if (e->type == GDK_BUTTON_PRESS && e->button == 1)
        if (auto edge = impl->edge_at(e->x, e->y)) {
            gtk_window_begin_resize_drag(GTK_WINDOW(impl->window), *edge, 1, static_cast<gint>(e->x_root),
                                         static_cast<gint>(e->y_root), e->time);
            return TRUE;
        }
    return FALSE;  // the page gets it
}

gboolean on_motion(GtkWidget* widget, GdkEventMotion* e, gpointer data) {
    auto* impl = static_cast<NativeWindow::Impl*>(data);
    if (!impl->frameless || !impl->resizable) return FALSE;
    static const char* names[] = {"nw-resize", "n-resize", "ne-resize", "w-resize", "e-resize", "sw-resize", "s-resize", "se-resize"};
    auto edge = impl->edge_at(e->x, e->y);
    GdkWindow* gw = gtk_widget_get_window(widget);
    GdkCursor* cursor = edge ? gdk_cursor_new_from_name(gdk_window_get_display(gw), names[*edge]) : nullptr;
    gdk_window_set_cursor(gw, cursor);
    if (cursor) g_object_unref(cursor);
    return FALSE;
}

gboolean on_delete(GtkWidget*, GdkEvent*, gpointer data) {
    auto* impl = static_cast<NativeWindow::Impl*>(data);
    if (impl->owner->on_close_requested) impl->owner->on_close_requested();
    return TRUE;  // the window stays unless the handler closes it
}

void run_posted(std::function<void()>* f) {
    (*f)();
    delete f;
}

}  // namespace

NativeWindow::NativeWindow(const WindowSpec& spec) : impl_(std::make_shared<Impl>()) {
    Impl& w = *impl_;
    w.owner = this;
    w.resizable = spec.resizable;
    w.frameless = spec.frameless;
    w.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    GtkWindow* win = GTK_WINDOW(w.window);
    gtk_window_set_title(win, spec.title.c_str());
    gtk_window_set_default_size(win, spec.width, spec.height);
    if (spec.min_width || spec.min_height) {
        GdkGeometry g{};
        g.min_width = spec.min_width;
        g.min_height = spec.min_height;
        gtk_window_set_geometry_hints(win, nullptr, &g, GDK_HINT_MIN_SIZE);
    }
    gtk_window_set_decorated(win, !spec.frameless);
    gtk_window_set_resizable(win, spec.resizable);
    gtk_window_set_skip_taskbar_hint(win, spec.skip_taskbar);
    gtk_window_set_keep_above(win, spec.always_on_top);
    if (spec.center) gtk_window_set_position(win, GTK_WIN_POS_CENTER);
    if (spec.transparent) {
        if (GdkVisual* rgba = gdk_screen_get_rgba_visual(gtk_widget_get_screen(w.window))) gtk_widget_set_visual(w.window, rgba);
        gtk_widget_set_app_paintable(w.window, TRUE);
    }
    g_signal_connect(w.window, "delete-event", G_CALLBACK(on_delete), &w);

    const bool debug = launch::get_env("STAR_FORGE_DEVTOOLS").has_value();
    w.view = std::make_unique<webview::webview>(debug, w.window);
    auto* page = static_cast<WebKitWebView*>(w.view->browser_controller().value());
    if (spec.transparent) {
        GdkRGBA clear{0, 0, 0, 0};
        webkit_web_view_set_background_color(page, &clear);
    }
    g_signal_connect(page, "decide-policy", G_CALLBACK(decide_policy), nullptr);
    g_signal_connect(page, "button-press-event", G_CALLBACK(on_press), &w);
    g_signal_connect(page, "motion-notify-event", G_CALLBACK(on_motion), &w);
    gtk_widget_realize(w.window);
    if (spec.click_through) {
        cairo_region_t* none = cairo_region_create();
        gtk_widget_input_shape_combine_region(w.window, none);
        cairo_region_destroy(none);
    }
}

NativeWindow::~NativeWindow() {
    impl_->owner = nullptr;
    impl_->view.reset();
    gtk_widget_destroy(impl_->window);
}

void NativeWindow::init_script(const std::string& js) { impl_->view->init(js); }

void NativeWindow::bind(const std::string& name, std::function<void(const std::string&, const std::string&)> f) {
    impl_->view->bind(
        name, [f](std::string id, std::string args, void*) { f(id, args); }, nullptr);
}

void NativeWindow::resolve(const std::string& id, bool ok, const std::string& json) {
    std::weak_ptr<Impl> weak = impl_;
    // webview's own resolve() queues a call holding its engine, which may be gone by the time it
    // runs (the splash closes as it answers); this one checks the window is still there.
    post([weak, id, ok, json] {
        if (auto w = weak.lock(); w && w->view)
            w->view->eval("window.__webview__.onReply(" + webview::detail::json_escape(id) + ", " + (ok ? "0" : "1") + ", " +
                          webview::detail::json_escape(json) + ")");
    });
}

void NativeWindow::eval(const std::string& js) {
    std::weak_ptr<Impl> weak = impl_;
    post([weak, js] {
        if (auto w = weak.lock(); w && w->view) w->view->eval(js);
    });
}

void NativeWindow::load(const std::string& page) { impl_->view->navigate(std::string(kOrigin) + "/" + page); }

void NativeWindow::show() {
    gtk_widget_show(impl_->window);
    gtk_window_deiconify(GTK_WINDOW(impl_->window));
    gtk_window_present(GTK_WINDOW(impl_->window));
}

void NativeWindow::hide() { gtk_widget_hide(impl_->window); }
void NativeWindow::minimize() { gtk_window_iconify(GTK_WINDOW(impl_->window)); }

void NativeWindow::toggle_maximize() {
    GtkWindow* win = GTK_WINDOW(impl_->window);
    if (gtk_window_is_maximized(win)) gtk_window_unmaximize(win);
    else gtk_window_maximize(win);
}

void NativeWindow::start_drag() {
    // Right after the press the page saw (its button is still down, so Wayland allows it).
    gtk_window_begin_move_drag(GTK_WINDOW(impl_->window), impl_->press_button, static_cast<gint>(impl_->press_x),
                               static_cast<gint>(impl_->press_y), impl_->press_time);
}

bool NativeWindow::visible() const { return gtk_widget_get_visible(impl_->window); }

void NativeWindow::fit_to_monitor() {
    GtkWindow* win = GTK_WINDOW(impl_->window);
    if (gtk_window_is_maximized(win)) return;
    GdkDisplay* display = gtk_widget_get_display(impl_->window);
    GdkWindow* gw = gtk_widget_get_window(impl_->window);
    GdkMonitor* monitor = gw ? gdk_display_get_monitor_at_window(display, gw) : nullptr;
    if (!monitor) monitor = gdk_display_get_primary_monitor(display);
    if (!monitor) return;
    GdkRectangle area;
    gdk_monitor_get_workarea(monitor, &area);
    int w = 0, h = 0, x = 0, y = 0;
    gtk_window_get_size(win, &w, &h);
    const int nw = std::min(w, area.width), nh = std::min(h, area.height);
    if (nw != w || nh != h) gtk_window_resize(win, nw, nh);
    gtk_window_get_position(win, &x, &y);
    const int nx = std::clamp(x, area.x, area.x + area.width - nw), ny = std::clamp(y, area.y, area.y + area.height - nh);
    if (nx != x || ny != y) gtk_window_move(win, nx, ny);
}

void NativeWindow::set_icon_png(const std::string& png) {
    GdkPixbufLoader* loader = gdk_pixbuf_loader_new();
    if (gdk_pixbuf_loader_write(loader, reinterpret_cast<const guchar*>(png.data()), png.size(), nullptr) &&
        gdk_pixbuf_loader_close(loader, nullptr))
        gtk_window_set_icon(GTK_WINDOW(impl_->window), gdk_pixbuf_loader_get_pixbuf(loader));
    g_object_unref(loader);
}

void NativeWindow::snapshot(const std::string& png_path) {
    auto* page = static_cast<WebKitWebView*>(impl_->view->browser_controller().value());
    webkit_web_view_get_snapshot(
        page, WEBKIT_SNAPSHOT_REGION_VISIBLE, WEBKIT_SNAPSHOT_OPTIONS_NONE, nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
            auto* path = static_cast<std::string*>(data);
            if (cairo_surface_t* s = webkit_web_view_get_snapshot_finish(WEBKIT_WEB_VIEW(source), result, nullptr)) {
                cairo_surface_write_to_png(s, path->c_str());
                cairo_surface_destroy(s);
            }
            delete path;
        },
        new std::string(png_path));
}

void* NativeWindow::handle() const { return impl_->window; }

void init() {
    // WebKitGTK's DMA-BUF renderer shows a blank window on some Linux GPU drivers; the fallback
    // renderer is fine for a launcher.
    if (!launch::get_env("WEBKIT_DISABLE_DMABUF_RENDERER")) launch::set_env("WEBKIT_DISABLE_DMABUF_RENDERER", "1");
    g_set_prgname("star-forge");  // the desktop entry's StartupWMClass
    if (!gtk_init_check(nullptr, nullptr)) throw std::runtime_error("cannot open a window (no display?)");
    WebKitWebContext* context = webkit_web_context_get_default();
    webkit_web_context_register_uri_scheme(context, "sf-ui", serve, nullptr, nullptr);
    WebKitSecurityManager* security = webkit_web_context_get_security_manager(context);
    webkit_security_manager_register_uri_scheme_as_secure(security, "sf-ui");
    webkit_security_manager_register_uri_scheme_as_cors_enabled(security, "sf-ui");
}

void run() { gtk_main(); }
void quit() { gtk_main_quit(); }

void post(std::function<void()> f) {
    g_idle_add_full(
        G_PRIORITY_HIGH_IDLE,
        [](gpointer p) -> gboolean {
            run_posted(static_cast<std::function<void()>*>(p));
            return G_SOURCE_REMOVE;
        },
        new std::function<void()>(std::move(f)), nullptr);
}

namespace {

std::optional<std::string> choose(NativeWindow* parent, const std::string& title, GtkFileChooserAction action,
                                  const std::optional<std::string>& start, GtkFileFilter* filter) {
    GtkFileChooserNative* dialog = gtk_file_chooser_native_new(
        title.c_str(), parent ? GTK_WINDOW(parent->handle()) : nullptr, action, nullptr, nullptr);
    if (start) gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), start->c_str());
    if (filter) gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
    std::optional<std::string> out;
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        if (char* name = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog))) {
            out = name;
            g_free(name);
        }
    }
    g_object_unref(dialog);
    return out;
}

}  // namespace

std::optional<std::string> pick_folder(NativeWindow* parent, const std::string& title, const std::optional<std::string>& start) {
    return choose(parent, title, GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, start, nullptr);
}

std::optional<std::string> pick_file(NativeWindow* parent, const std::string& title, const std::string& filter_name,
                                     const std::vector<std::string>& extensions) {
    GtkFileFilter* filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, filter_name.c_str());
    for (const auto& e : extensions) {
        gtk_file_filter_add_pattern(filter, ("*." + e).c_str());
    }
    return choose(parent, title, GTK_FILE_CHOOSER_ACTION_OPEN, std::nullopt, filter);
}

void open(const std::string& target) {
    // xdg-open started with the host's environment, not the AppImage's (see env::host_env).
    launch::Command cmd;
    cmd.program = "xdg-open";
    cmd.args = {target};
    cmd.env = launch::env::host_env();
    try {
        auto child = std::make_shared<launch::Process>(launch::spawn(cmd, launch::Output{launch::Output::Kind::Null, {}}));
        std::thread([child] { child->wait(); }).detach();
    } catch (const std::exception& e) {
        const std::string m = e.what(), prefix = "could not start xdg-open: ";
        throw std::runtime_error("could not open " + target + ": " + (m.rfind(prefix, 0) == 0 ? m.substr(prefix.size()) : m));
    }
}

}  // namespace sf::app
