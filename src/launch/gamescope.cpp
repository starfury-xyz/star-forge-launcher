#include "launch/gamescope.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <set>
#include <type_traits>

#ifdef __linux__
#include <dlfcn.h>
#endif

namespace sf::launch::gamescope {

std::optional<std::uint32_t> app_id(const char* desktop, const char* session_desktop, const char* steam_game_id) {
    auto is_gamescope = [](const char* v) { return v && std::strcmp(v, "gamescope") == 0; };
    if (!is_gamescope(desktop) && !is_gamescope(session_desktop)) return std::nullopt;
    if (!steam_game_id || !*steam_game_id) return std::nullopt;
    char* end = nullptr;
    const unsigned long long id = std::strtoull(steam_game_id, &end, 10);
    if (*end || id == 0) return std::nullopt;
    const unsigned long long app = id >> 32 ? id >> 32 : id;
    if (app > 0xffffffffull) return std::nullopt;
    return static_cast<std::uint32_t>(app);
}

std::optional<std::uint32_t> app_id() {
    return app_id(std::getenv("XDG_CURRENT_DESKTOP"), std::getenv("XDG_SESSION_DESKTOP"), std::getenv("SteamGameId"));
}

bool is_wine_class(const std::string& cls) {
    auto ends_with = [&](const char* s) {
        const size_t n = std::strlen(s);
        return cls.size() > n && cls.compare(cls.size() - n, n, s) == 0;
    };
    return cls.rfind("steam_app_", 0) == 0 || ends_with(".exe") || ends_with(".EXE");
}

#ifdef __linux__
namespace {

// The few libxcb calls used, loaded at run time: no build dependency, and nothing happens on a
// system without it. XCB rather than Xlib: Xlib reports errors (a window closing between two
// requests) to one handler for the whole process, which GTK owns in the launcher.
struct Cookie {
    unsigned int sequence;
};
struct GenericError;
struct QueryTreeReply;
struct InternAtomReply {
    std::uint8_t response_type, pad0;
    std::uint16_t sequence;
    std::uint32_t length, atom;
};
struct GetPropertyReply {
    std::uint8_t response_type, format;
    std::uint16_t sequence;
    std::uint32_t length, type, bytes_after, value_len;
    std::uint8_t pad0[12];
};
struct Screen {
    std::uint32_t root;  // the rest is not used
};
struct ScreenIterator {
    Screen* data;
    int rem, index;
};
struct Connection;
struct Setup;

struct Xcb {
    void* lib = nullptr;
    Connection* (*connect)(const char*, int*) = nullptr;
    int (*connection_has_error)(Connection*) = nullptr;
    void (*disconnect)(Connection*) = nullptr;
    const Setup* (*get_setup)(Connection*) = nullptr;
    ScreenIterator (*setup_roots_iterator)(const Setup*) = nullptr;
    void (*screen_next)(ScreenIterator*) = nullptr;
    Cookie (*query_tree)(Connection*, std::uint32_t) = nullptr;
    QueryTreeReply* (*query_tree_reply)(Connection*, Cookie, GenericError**) = nullptr;
    std::uint32_t* (*query_tree_children)(const QueryTreeReply*) = nullptr;
    int (*query_tree_children_length)(const QueryTreeReply*) = nullptr;
    Cookie (*intern_atom)(Connection*, std::uint8_t, std::uint16_t, const char*) = nullptr;
    InternAtomReply* (*intern_atom_reply)(Connection*, Cookie, GenericError**) = nullptr;
    Cookie (*get_property)(Connection*, std::uint8_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                           std::uint32_t) = nullptr;
    GetPropertyReply* (*get_property_reply)(Connection*, Cookie, GenericError**) = nullptr;
    void* (*get_property_value)(const GetPropertyReply*) = nullptr;
    int (*get_property_value_length)(const GetPropertyReply*) = nullptr;
    Cookie (*change_property)(Connection*, std::uint8_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint8_t,
                              std::uint32_t, const void*) = nullptr;
    void* (*poll_for_event)(Connection*) = nullptr;
    int (*flush)(Connection*) = nullptr;

    bool load() {
        lib = dlopen("libxcb.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!lib) return false;
        bool ok = true;
        auto sym = [&](auto& f, const char* name) {
            f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(dlsym(lib, name));
            ok = ok && f;
        };
        sym(connect, "xcb_connect");
        sym(connection_has_error, "xcb_connection_has_error");
        sym(disconnect, "xcb_disconnect");
        sym(get_setup, "xcb_get_setup");
        sym(setup_roots_iterator, "xcb_setup_roots_iterator");
        sym(screen_next, "xcb_screen_next");
        sym(query_tree, "xcb_query_tree");
        sym(query_tree_reply, "xcb_query_tree_reply");
        sym(query_tree_children, "xcb_query_tree_children");
        sym(query_tree_children_length, "xcb_query_tree_children_length");
        sym(intern_atom, "xcb_intern_atom");
        sym(intern_atom_reply, "xcb_intern_atom_reply");
        sym(get_property, "xcb_get_property");
        sym(get_property_reply, "xcb_get_property_reply");
        sym(get_property_value, "xcb_get_property_value");
        sym(get_property_value_length, "xcb_get_property_value_length");
        sym(change_property, "xcb_change_property");
        sym(poll_for_event, "xcb_poll_for_event");
        sym(flush, "xcb_flush");
        return ok;
    }
    ~Xcb() {
        if (lib) dlclose(lib);
    }
};

constexpr std::uint32_t kAtomCardinal = 6, kAtomString = 31, kAtomWmClass = 67, kAnyPropertyType = 0;
constexpr std::uint8_t kPropModeReplace = 0;

std::uint32_t intern(Xcb& x, Connection* c, const char* name) {
    InternAtomReply* r =
        x.intern_atom_reply(c, x.intern_atom(c, 0, static_cast<std::uint16_t>(std::strlen(name)), name), nullptr);
    if (!r) return 0;
    const std::uint32_t atom = r->atom;
    std::free(r);
    return atom;
}

// The window's property bytes, or nullopt when it has none (or is gone).
std::optional<std::string> property(Xcb& x, Connection* c, std::uint32_t win, std::uint32_t atom, std::uint32_t type) {
    GetPropertyReply* r = x.get_property_reply(c, x.get_property(c, 0, win, atom, type, 0, 256), nullptr);
    if (!r) return std::nullopt;
    std::optional<std::string> v;
    if (r->type != 0) {
        const auto* p = static_cast<const char*>(x.get_property_value(r));
        v = std::string(p, p + x.get_property_value_length(r) * (r->format / 8));
    }
    std::free(r);
    return v;
}

void tag_loop(std::uint32_t app, const std::atomic<bool>& stop) {
    Xcb x;
    if (!x.load()) return;
    int screen = 0;
    Connection* c = x.connect(nullptr, &screen);
    if (!c || x.connection_has_error(c)) {
        if (c) x.disconnect(c);
        return;
    }
    ScreenIterator it = x.setup_roots_iterator(x.get_setup(c));
    for (int i = 0; i < screen && it.rem; ++i) x.screen_next(&it);
    const std::uint32_t root = it.data->root;
    const std::uint32_t steam_game = intern(x, c, "STEAM_GAME");
    std::set<std::uint32_t> seen;
    while (steam_game && !stop.load()) {
        if (x.connection_has_error(c)) break;
        if (QueryTreeReply* tree = x.query_tree_reply(c, x.query_tree(c, root), nullptr)) {
            const std::uint32_t* kids = x.query_tree_children(tree);
            const int n = x.query_tree_children_length(tree);
            std::set<std::uint32_t> now(kids, kids + n);
            std::free(tree);
            for (std::uint32_t w : now) {
                if (seen.count(w)) continue;
                // WM_CLASS is "instance\0class\0"; Wine sets it once the window is set up, so a
                // window without one yet is looked at again next time.
                auto cls = property(x, c, w, kAtomWmClass, kAtomString);
                if (!cls) continue;
                seen.insert(w);
                const std::string klass = cls->substr(cls->find('\0') == std::string::npos ? 0 : cls->find('\0') + 1);
                if (!is_wine_class(klass)) continue;
                if (property(x, c, w, steam_game, kAnyPropertyType)) continue;
                const std::uint32_t value = app;
                x.change_property(c, kPropModeReplace, w, steam_game, kAtomCardinal, 32, 1, &value);
            }
            for (auto i = seen.begin(); i != seen.end();) i = now.count(*i) ? std::next(i) : seen.erase(i);
            x.flush(c);
        }
        // Errors from change_property (a window that closed) arrive as events; drop them.
        while (void* e = x.poll_for_event(c)) std::free(e);
        for (int i = 0; i < 5 && !stop.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    x.disconnect(c);
}

}  // namespace

WindowTagger::WindowTagger(std::uint32_t app) : thread_([this, app] { tag_loop(app, stop_); }) {}
#else
WindowTagger::WindowTagger(std::uint32_t) {}
#endif

WindowTagger::~WindowTagger() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

}  // namespace sf::launch::gamescope
