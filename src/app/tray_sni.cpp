// The tray icon on Linux: a StatusNotifierItem with a com.canonical.dbusmenu menu, spoken over
// D-Bus with GIO. That is the protocol libappindicator (which Tauri's tray uses) speaks to KDE,
// GNOME's AppIndicator extension, waybar and the rest, without needing that library installed.
// Like the Rust launcher's, a left click opens the menu.

#include <unistd.h>

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>

#include "app/native.hpp"

namespace sf::app {

namespace {

const char* kWatcher = "org.kde.StatusNotifierWatcher";
const char* kItemPath = "/StatusNotifierItem";
const char* kMenuPath = "/MenuBar";
enum MenuId { kRoot = 0, kShow = 1, kExit = 2 };

const char* kXml = R"xml(<node>
 <interface name="org.kde.StatusNotifierItem">
  <property name="Category" type="s" access="read"/>
  <property name="Id" type="s" access="read"/>
  <property name="Title" type="s" access="read"/>
  <property name="Status" type="s" access="read"/>
  <property name="WindowId" type="i" access="read"/>
  <property name="IconName" type="s" access="read"/>
  <property name="IconPixmap" type="a(iiay)" access="read"/>
  <property name="ToolTip" type="(sa(iiay)ss)" access="read"/>
  <property name="ItemIsMenu" type="b" access="read"/>
  <property name="Menu" type="o" access="read"/>
  <method name="ContextMenu"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
  <method name="Activate"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
  <method name="SecondaryActivate"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
  <method name="Scroll"><arg name="delta" type="i" direction="in"/><arg name="orientation" type="s" direction="in"/></method>
  <signal name="NewIcon"/>
  <signal name="NewToolTip"/>
  <signal name="NewStatus"><arg name="status" type="s"/></signal>
 </interface>
 <interface name="com.canonical.dbusmenu">
  <property name="Version" type="u" access="read"/>
  <property name="TextDirection" type="s" access="read"/>
  <property name="Status" type="s" access="read"/>
  <property name="IconThemePath" type="as" access="read"/>
  <method name="GetLayout">
   <arg type="i" name="parentId" direction="in"/><arg type="i" name="recursionDepth" direction="in"/>
   <arg type="as" name="propertyNames" direction="in"/>
   <arg type="u" name="revision" direction="out"/><arg type="(ia{sv}av)" name="layout" direction="out"/>
  </method>
  <method name="GetGroupProperties">
   <arg type="ai" name="ids" direction="in"/><arg type="as" name="propertyNames" direction="in"/>
   <arg type="a(ia{sv})" name="properties" direction="out"/>
  </method>
  <method name="GetProperty">
   <arg type="i" name="id" direction="in"/><arg type="s" name="name" direction="in"/><arg type="v" name="value" direction="out"/>
  </method>
  <method name="Event">
   <arg type="i" name="id" direction="in"/><arg type="s" name="eventId" direction="in"/>
   <arg type="v" name="data" direction="in"/><arg type="u" name="timestamp" direction="in"/>
  </method>
  <method name="EventGroup"><arg type="a(isvu)" name="events" direction="in"/><arg type="ai" name="idErrors" direction="out"/></method>
  <method name="AboutToShow"><arg type="i" name="id" direction="in"/><arg type="b" name="needUpdate" direction="out"/></method>
  <method name="AboutToShowGroup">
   <arg type="ai" name="ids" direction="in"/><arg type="ai" name="updatesNeeded" direction="out"/><arg type="ai" name="idErrors" direction="out"/>
  </method>
  <signal name="ItemsPropertiesUpdated"><arg type="a(ia{sv})" name="updatedProps"/><arg type="a(ias)" name="removedProps"/></signal>
  <signal name="LayoutUpdated"><arg type="u" name="revision"/><arg type="i" name="parent"/></signal>
 </interface>
</node>)xml";

class SniTray final : public Tray {
public:
    SniTray(GDBusConnection* conn, std::string tooltip, std::function<void()> on_show, std::function<void()> on_exit)
        : conn_(conn), tooltip_(std::move(tooltip)), on_show_(std::move(on_show)), on_exit_(std::move(on_exit)) {}

    ~SniTray() override {
        if (watch_) g_bus_unwatch_name(watch_);
        for (guint id : registrations_) g_dbus_connection_unregister_object(conn_, id);
        if (pixmap_) g_variant_unref(pixmap_);
        g_object_unref(conn_);
    }

    bool start(const std::string& icon_png) {
        pixmap_ = g_variant_ref_sink(pixmap_of(icon_png));
        GError* error = nullptr;
        GDBusNodeInfo* node = g_dbus_node_info_new_for_xml(kXml, &error);
        if (!node) {
            g_error_free(error);
            return false;
        }
        static const GDBusInterfaceVTable vtable{call, property, nullptr, {}};
        for (auto [path, iface] : {std::pair{kItemPath, 0}, std::pair{kMenuPath, 1}}) {
            guint id = g_dbus_connection_register_object(conn_, path, node->interfaces[iface], &vtable, this, nullptr, nullptr);
            if (id) registrations_.push_back(id);
        }
        g_dbus_node_info_unref(node);
        if (registrations_.size() != 2) return false;
        // The item's own bus name, as the spec has it: org.kde.StatusNotifierItem-<pid>-<n>.
        name_ = "org.kde.StatusNotifierItem-" + std::to_string(getpid()) + "-1";
        GVariant* r = g_dbus_connection_call_sync(conn_, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                                  "RequestName", g_variant_new("(su)", name_.c_str(), 4u /* do not queue */),
                                                  G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);
        if (!r) return false;
        g_variant_unref(r);
        // Register with the tray host now, and again whenever it restarts.
        watch_ = g_bus_watch_name_on_connection(
            conn_, kWatcher, G_BUS_NAME_WATCHER_FLAGS_NONE,
            [](GDBusConnection* c, const gchar*, const gchar*, gpointer data) {
                auto* self = static_cast<SniTray*>(data);
                g_dbus_connection_call(c, kWatcher, "/StatusNotifierWatcher", kWatcher, "RegisterStatusNotifierItem",
                                       g_variant_new("(s)", self->name_.c_str()), nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
                                       nullptr, nullptr);
            },
            nullptr, this, nullptr);
        return true;
    }

private:
    // The icon as SNI's a(iiay): width, height, ARGB32 in network byte order.
    static GVariant* pixmap_of(const std::string& png) {
        GVariantBuilder icons;
        g_variant_builder_init(&icons, G_VARIANT_TYPE("a(iiay)"));
        GdkPixbufLoader* loader = gdk_pixbuf_loader_new();
        if (gdk_pixbuf_loader_write(loader, reinterpret_cast<const guchar*>(png.data()), png.size(), nullptr) &&
            gdk_pixbuf_loader_close(loader, nullptr)) {
            GdkPixbuf* pix = gdk_pixbuf_add_alpha(gdk_pixbuf_loader_get_pixbuf(loader), FALSE, 0, 0, 0);
            const int w = gdk_pixbuf_get_width(pix), h = gdk_pixbuf_get_height(pix), stride = gdk_pixbuf_get_rowstride(pix);
            const guchar* px = gdk_pixbuf_get_pixels(pix);
            std::string argb(static_cast<size_t>(w) * h * 4, '\0');
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const guchar* p = px + y * stride + x * 4;  // RGBA
                    char* q = &argb[(static_cast<size_t>(y) * w + x) * 4];
                    q[0] = static_cast<char>(p[3]);
                    q[1] = static_cast<char>(p[0]);
                    q[2] = static_cast<char>(p[1]);
                    q[3] = static_cast<char>(p[2]);
                }
            g_variant_builder_add(&icons, "(ii@ay)", w, h,
                                  g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, argb.data(), argb.size(), 1));
            g_object_unref(pix);
        }
        g_object_unref(loader);
        return g_variant_builder_end(&icons);
    }

    static GVariant* item_props(int id) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
        if (id == kRoot) {
            g_variant_builder_add(&b, "{sv}", "children-display", g_variant_new_string("submenu"));
        } else {
            g_variant_builder_add(&b, "{sv}", "label", g_variant_new_string(id == kShow ? "Show STAR FORGE Launcher" : "Exit"));
            g_variant_builder_add(&b, "{sv}", "enabled", g_variant_new_boolean(TRUE));
            g_variant_builder_add(&b, "{sv}", "visible", g_variant_new_boolean(TRUE));
        }
        return g_variant_builder_end(&b);
    }

    static GVariant* layout(int id, bool children) {
        GVariantBuilder kids;
        g_variant_builder_init(&kids, G_VARIANT_TYPE("av"));
        if (id == kRoot && children)
            for (int child : {kShow, kExit}) g_variant_builder_add(&kids, "v", layout(child, false));
        return g_variant_new("(i@a{sv}@av)", id, item_props(id), g_variant_builder_end(&kids));
    }

    void clicked(int id) {
        if (id == kShow && on_show_) on_show_();
        if (id == kExit && on_exit_) on_exit_();
    }

    static void call(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* method, GVariant* params,
                     GDBusMethodInvocation* invocation, gpointer data) {
        auto* self = static_cast<SniTray*>(data);
        const std::string m = method;
        if (m == "GetLayout") {
            gint parent = 0, depth = 0;
            g_variant_get(params, "(ii^a&s)", &parent, &depth, nullptr);
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(u@(ia{sv}av))", 1u, layout(parent, depth != 0)));
        } else if (m == "GetGroupProperties") {
            GVariantIter* ids = nullptr;
            g_variant_get(params, "(ai^a&s)", &ids, nullptr);
            GVariantBuilder b;
            g_variant_builder_init(&b, G_VARIANT_TYPE("a(ia{sv})"));
            gint id;
            while (g_variant_iter_next(ids, "i", &id))
                if (id >= kRoot && id <= kExit) g_variant_builder_add(&b, "(i@a{sv})", id, item_props(id));
            g_variant_iter_free(ids);
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(@a(ia{sv}))", g_variant_builder_end(&b)));
        } else if (m == "GetProperty") {
            gint id = 0;
            const gchar* name = nullptr;
            g_variant_get(params, "(i&s)", &id, &name);
            GVariant* props = item_props(id);
            GVariant* v = g_variant_lookup_value(props, name, nullptr);
            g_variant_unref(g_variant_ref_sink(props));
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(v)", v ? v : g_variant_new_string("")));
            if (v) g_variant_unref(v);
        } else if (m == "Event") {
            gint id = 0;
            const gchar* event = nullptr;
            g_variant_get(params, "(i&svu)", &id, &event, nullptr, nullptr);
            const bool click = std::string(event) == "clicked";
            g_dbus_method_invocation_return_value(invocation, nullptr);
            if (click) self->clicked(id);
        } else if (m == "EventGroup") {
            GVariantIter* events = nullptr;
            g_variant_get(params, "(a(isvu))", &events);
            std::vector<int> clicks;
            gint id;
            const gchar* event;
            GVariant* value;
            guint32 time;
            while (g_variant_iter_next(events, "(i&svu)", &id, &event, &value, &time)) {
                if (std::string(event) == "clicked") clicks.push_back(id);
                g_variant_unref(value);
            }
            g_variant_iter_free(events);
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(@ai)", g_variant_new_array(G_VARIANT_TYPE_INT32, nullptr, 0)));
            for (int c : clicks) self->clicked(c);
        } else if (m == "AboutToShow") {
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", FALSE));
        } else if (m == "AboutToShowGroup") {
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(@ai@ai)", g_variant_new_array(G_VARIANT_TYPE_INT32, nullptr, 0),
                                                                            g_variant_new_array(G_VARIANT_TYPE_INT32, nullptr, 0)));
        } else {
            // Activate, SecondaryActivate, ContextMenu, Scroll: the menu does everything.
            g_dbus_method_invocation_return_value(invocation, nullptr);
        }
    }

    static GVariant* property(GDBusConnection*, const gchar*, const gchar*, const gchar* iface, const gchar* name, GError**,
                              gpointer data) {
        auto* self = static_cast<SniTray*>(data);
        const std::string n = name;
        if (std::string(iface) == "com.canonical.dbusmenu") {
            if (n == "Version") return g_variant_new_uint32(3);
            if (n == "TextDirection") return g_variant_new_string("ltr");
            if (n == "Status") return g_variant_new_string("normal");
            return g_variant_new_strv(nullptr, 0);  // IconThemePath
        }
        if (n == "Category") return g_variant_new_string("ApplicationStatus");
        if (n == "Id") return g_variant_new_string("star-forge");
        if (n == "Title") return g_variant_new_string("STAR FORGE Launcher");
        if (n == "Status") return g_variant_new_string("Active");
        if (n == "WindowId") return g_variant_new_int32(0);
        if (n == "IconName") return g_variant_new_string("");
        if (n == "IconPixmap") return g_variant_ref(self->pixmap_);
        if (n == "ItemIsMenu") return g_variant_new_boolean(TRUE);
        if (n == "Menu") return g_variant_new_object_path(kMenuPath);
        // ToolTip: icon name, icon pixmaps, title, text.
        return g_variant_new("(s@a(iiay)ss)", "", g_variant_new_array(G_VARIANT_TYPE("(iiay)"), nullptr, 0), self->tooltip_.c_str(), "");
    }

    GDBusConnection* conn_;
    std::string tooltip_, name_;
    std::function<void()> on_show_, on_exit_;
    GVariant* pixmap_ = nullptr;
    std::vector<guint> registrations_;
    guint watch_ = 0;
};

}  // namespace

std::unique_ptr<Tray> Tray::create(const std::string& icon_png, const std::string& tooltip, std::function<void()> on_show,
                                   std::function<void()> on_exit) {
    GDBusConnection* conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!conn) return nullptr;
    // A tray needs a host listening for items.
    GVariant* r = g_dbus_connection_call_sync(conn, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                              "NameHasOwner", g_variant_new("(s)", kWatcher), G_VARIANT_TYPE("(b)"),
                                              G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);
    gboolean has = FALSE;
    if (r) {
        g_variant_get(r, "(b)", &has);
        g_variant_unref(r);
    }
    if (!has) {
        g_object_unref(conn);
        return nullptr;
    }
    auto tray = std::make_unique<SniTray>(conn, tooltip, std::move(on_show), std::move(on_exit));
    if (!tray->start(icon_png)) return nullptr;
    return tray;
}

}  // namespace sf::app
