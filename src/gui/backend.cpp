// The command table (main.rs's generate_handler!), shared helpers, and downloads.rs.

#include <cstdio>
#include <stdexcept>
#include <thread>

#include "crypto/base64.hpp"
#include "crypto/random.hpp"
#include "gui/internal.hpp"
#include "launch/process.hpp"
#include "launch/profiles.hpp"
#include "launch/steam.hpp"
#include "pack/serde.hpp"

namespace sf::gui {

namespace serde = pack::serde;
using launch::remote::SyncEvent;

// --- helpers ---

std::string trim(const std::string& s) {
    const char* ws = " \t\r\n\f\v";
    size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(ws) - a + 1);
}

std::optional<std::string> data_url(const fs::path& path) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) return std::nullopt;
    if (size > (8u << 20)) {
        std::fprintf(stderr, "%s: too large for artwork (8 MB at most)\n", launch::ustr(path).c_str());
        return std::nullopt;
    }
    auto bytes = launch::read_text(path);
    if (!bytes) return std::nullopt;
    const std::string& b = *bytes;
    const char* mime = nullptr;
    if (b.size() >= 4 && b.compare(0, 4, "\x89PNG") == 0) mime = "image/png";
    else if (b.size() >= 3 && b.compare(0, 3, "\xff\xd8\xff") == 0) mime = "image/jpeg";
    else if (b.size() >= 12 && b.compare(0, 4, "RIFF") == 0 && b.compare(8, 4, "WEBP") == 0) mime = "image/webp";
    if (!mime) {
        std::fprintf(stderr, "%s: not a PNG, JPEG or WebP image\n", launch::ustr(path).c_str());
        return std::nullopt;
    }
    return std::string("data:") + mime + ";base64," +
           crypto::base64_encode(reinterpret_cast<const uint8_t*>(b.data()), b.size());
}

size_t random_below(size_t n) {
    uint64_t r = 0;
    crypto::random_bytes(reinterpret_cast<uint8_t*>(&r), sizeof r);
    return static_cast<size_t>(r % n);
}

bool path_starts_with(const fs::path& path, const fs::path& prefix) {
    auto p = path.begin();
    for (auto q = prefix.begin(); q != prefix.end(); ++q, ++p) {
        if (q->empty()) continue;  // a trailing separator
        if (p == path.end() || *p != *q) return false;
    }
    return true;
}

void create_dirs(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) throw std::runtime_error(launch::io_error(dir, ec));
}

fs::path profiles_dir() {
    auto dir = launch::profiles::default_profiles_dir();
    if (!dir) throw std::runtime_error("cannot work out the profiles folder");
    return *dir;
}

fs::path profile_dir(const std::string& id) {
    const fs::path dir = profiles_dir() / launch::upath(id);
    if (!launch::profiles::valid_name(id) || !launch::profiles::is_profile(dir))
        throw std::runtime_error("profile " + serde::debug_str(id) + " does not exist");
    return dir;
}

// --- arguments ---

namespace {

[[noreturn]] void bad_arg(const char* command, const char* key, const std::string& why) {
    throw std::runtime_error(std::string("invalid args `") + key + "` for command `" + command + "`: " + why);
}

template <class F>
auto read_arg(const char* command, const char* key, F f) -> decltype(f()) {
    try {
        return f();
    } catch (const pack::Error& e) {
        bad_arg(command, key, e.what());
    }
}

}  // namespace

const json& arg(const char* command, const json& args, const char* key) {
    auto it = args.is_object() ? args.find(key) : args.end();
    if (it == args.end())
        bad_arg(command, key, std::string("command ") + command + " missing required key " + key);
    return *it;
}

std::string arg_str(const char* command, const json& args, const char* key) {
    const json& v = arg(command, args, key);
    return read_arg(command, key, [&] { return serde::str(v); });
}

std::optional<std::string> arg_opt_str(const char* command, const json& args, const char* key) {
    if (!args.is_object() || !args.contains(key) || args[key].is_null()) return std::nullopt;
    return arg_str(command, args, key);
}

uint64_t arg_u64(const char* command, const json& args, const char* key) {
    const json& v = arg(command, args, key);
    return read_arg(command, key, [&] { return serde::u64(v); });
}

bool arg_bool(const char* command, const json& args, const char* key) {
    const json& v = arg(command, args, key);
    return read_arg(command, key, [&] { return serde::boolean(v); });
}

std::vector<fs::path> default_tool_folders(const fs::path& exe_dir) {
    std::vector<fs::path> out;
#ifndef _WIN32
    if (exe_dir.filename() == "bin") out.push_back(exe_dir.parent_path() / "lib" / "STAR FORGE Launcher" / "tools");
#endif
    out.push_back(exe_dir / "tools");
    out.push_back(exe_dir);
    return out;
}

// --- the backend ---

Backend::Backend(Host& host)
    : host_(host), game_(std::make_unique<Game>()), reviews_(std::make_unique<Reviews>()),
      downloads_(std::make_unique<DownloadState>()), updates_(std::make_unique<Updates>()) {
    const fs::path exe_dir = launch::current_exe().parent_path();
    tool_folders = default_tool_folders(exe_dir);
    resources = exe_dir;
}

Backend::~Backend() { wait_idle(); }

void Backend::background(std::function<void()> f, bool idle) {
    if (!idle) {
        std::lock_guard<std::mutex> lock(idle_mu_);
        ++working_;
    }
    std::thread([this, f = std::move(f), idle] {
        try {
            f();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "background task failed: %s\n", e.what());
        }
        if (!idle) {
            std::lock_guard<std::mutex> lock(idle_mu_);
            if (--working_ == 0) idle_cv_.notify_all();
        }
    }).detach();
}

void Backend::wait_idle() {
    std::unique_lock<std::mutex> lock(idle_mu_);
    idle_cv_.wait(lock, [&] { return working_ == 0; });
}

void Backend::notify(const std::string& message, NotificationLevel level) {
    host_.emit("notification", json::array({message, static_cast<int>(level)}));
}

void Backend::receive_link(const std::string& url) {
    bool listening;
    {
        std::lock_guard<std::mutex> lock(reviews_->mu);
        listening = reviews_->listening;
        if (!listening) reviews_->pending = url;
    }
    if (listening) {
        host_.restore();
        host_.emit("pack-link", url);
    }
}

json Backend::take_pending_link() {
    std::lock_guard<std::mutex> lock(reviews_->mu);
    reviews_->listening = true;
    auto link = std::move(reviews_->pending);
    reviews_->pending.reset();
    return link ? json(*link) : json(nullptr);
}

json Backend::invoke(const std::string& command, const json& args) {
    const char* c = command.c_str();
    // prefs.rs
    if (command == "get_launcher_preferences") return prefs().to_json();
    if (command == "update_launcher_preferences") return update_preferences(arg(c, args, "update"));
    // game.rs
    if (command == "get_launcher_snapshot") return snapshot();
    if (command == "initialize_launcher") return refresh(), nullptr;
    if (command == "perform_launcher_action") return perform_action(arg_str(c, args, "action"));
    if (command == "detect_compat_tools") return detect_compat_tools();
    if (command == "get_artwork") return get_artwork();
    if (command == "pick_background_folder") {
        auto dir = host_.pick_folder("Background folder", std::nullopt);
        return dir ? json(*dir) : json(nullptr);
    }
    if (command == "view_game_install") return view_game_install();
    if (command == "view_launch_log") return view_launch_log();
    // downloads.rs
    if (command == "cancel_download") return cancel_download(arg_u64(c, args, "id")), nullptr;
    // profiles.rs
    if (command == "list_profiles") return list_profiles();
    if (command == "save_profile") return save_profile(arg(c, args, "profile"));
    if (command == "delete_profile") return delete_profile(arg_str(c, args, "id")), nullptr;
    if (command == "open_profile_folder") return open_profile_folder(arg_opt_str(c, args, "id")), nullptr;
    if (command == "open_pack_link") return open_pack_link(arg_str(c, args, "id"), arg_str(c, args, "key")), nullptr;
    if (command == "inspect_pack") return inspect_pack(arg_str(c, args, "source"));
    if (command == "install_pack") return install_pack(arg_u64(c, args, "token"));
    if (command == "discard_pack") return discard_pack(arg_u64(c, args, "token")), nullptr;
    if (command == "pick_pack_file") {
        auto file = host_.pick_file("Import server pack", "Server pack", {"zip"});
        return file ? json(*file) : json(nullptr);
    }
    if (command == "list_sample_packs") return list_sample_packs();
    if (command == "sample_pack_source") return sample_pack_source(arg_str(c, args, "key"));
    if (command == "check_pack_updates") return check_pack_updates();
    if (command == "check_pack_update") return check_pack_update(arg_str(c, args, "id"));
    if (command == "take_pending_link") return take_pending_link();
    if (command == "get_profile_branding") return get_profile_branding(arg_str(c, args, "id"));
    // setup.rs
    if (command == "get_setup_info") return get_setup_info();
    if (command == "check_game_folder") {
        if (!launch::steam::is_game_dir(launch::upath(trim(arg_str(c, args, "path")))))
            throw std::runtime_error(
                "That folder doesn't hold Star Conflict (no win64\\game.exe). Pick the folder Steam installed it in.");
        return nullptr;
    }
    if (command == "pick_folder") {
        auto start = arg_opt_str(c, args, "start");
        std::error_code ec;
        if (start && !fs::is_directory(launch::upath(*start), ec)) start.reset();
        auto dir = host_.pick_folder(arg_str(c, args, "title"), start);
        return dir ? json(*dir) : json(nullptr);
    }
    if (command == "finish_setup")
        return finish_setup(arg_str(c, args, "gameDir"), arg_str(c, args, "profilesDir"), arg_bool(c, args, "moveProfiles")),
               nullptr;
    if (command == "install_appimage") return install_appimage(arg_str(c, args, "dir")), nullptr;
    // mods.cpp (no Rust counterpart). `on` is true, false, or null for the patch's default.
    if (command == "list_mods") return list_mods(arg_str(c, args, "kind"), arg_str(c, args, "id"));
    if (command == "set_mod") {
        const json& on = arg(c, args, "on");
        return set_mod(arg_str(c, args, "kind"), arg_str(c, args, "id"), arg_str(c, args, "module"),
                       arg_str(c, args, "patch"), on.is_null() ? std::nullopt : std::optional<bool>(arg_bool(c, args, "on")));
    }
    // servers.cpp (no Rust counterpart)
    if (command == "list_servers") return list_servers();
    if (command == "add_server") return add_server(arg_str(c, args, "name"), arg_str(c, args, "cloud"));
    if (command == "remove_server") return remove_server(arg_str(c, args, "id")), nullptr;
    if (command == "server_status") return server_status(arg_str(c, args, "id"));
    if (command == "open_server_folder") return open_server_folder(arg_str(c, args, "id"), arg_str(c, args, "which")), nullptr;
    if (command == "suggest_server_cloud") return suggest_server_cloud();
    if (command == "check_launcher_update") return check_launcher_update();
    if (command == "install_launcher_update") return install_launcher_update(), nullptr;
    if (command == "open_launcher_release") return open_launcher_release(), nullptr;
    throw std::runtime_error("command " + command + " not found");
}

// --- downloads.rs ---

namespace {
constexpr auto kTick = std::chrono::milliseconds(100);  // byte counts go out at most this often

json opt(const std::optional<uint64_t>& v) { return v ? json(*v) : json(nullptr); }
}  // namespace

Backend::Session::Session(Backend& backend, const char* action, std::string title)
    : backend_(backend), id_(backend.downloads_->next.fetch_add(1) + 1), action_(action), title_(std::move(title)),
      cancel_(std::make_shared<std::atomic<bool>>(false)) {
    std::lock_guard<std::mutex> lock(backend_.downloads_->mu);
    backend_.downloads_->running.emplace(id_, cancel_);
}

Backend::Session::~Session() {
    std::lock_guard<std::mutex> lock(backend_.downloads_->mu);
    backend_.downloads_->running.erase(id_);
}

void Backend::Session::emit(json message) {
    message["id"] = id_;
    backend_.host_.emit("download-progress", message);
}

std::optional<std::string> Backend::Session::report(const SyncEvent& e) {
    if (cancel_->load()) return std::string(kCancelled);
    if (e.kind == SyncEvent::Plan) {
        shown_ = e.files > 0;
        if (shown_)
            emit({{"kind", "start"},
                  {"action", action_},
                  {"title", title_},
                  {"source", e.list->source},
                  {"signed", e.list->is_signed},
                  {"listed", e.list->entries.size()},
                  {"files", e.files},
                  {"bytes", opt(e.bytes)},
                  {"kept", e.kept},
                  {"remove", e.remove}});
        return std::nullopt;
    }
    if (!shown_) return std::nullopt;
    switch (e.kind) {
    case SyncEvent::File:
        last_tick_.reset();
        emit({{"kind", "file"}, {"path", e.path}, {"size", opt(e.size)}});
        break;
    case SyncEvent::Bytes: {
        const auto now = std::chrono::steady_clock::now();
        if (e.total == e.done || !last_tick_ || now - *last_tick_ >= kTick) {
            last_tick_ = now;
            emit({{"kind", "bytes"}, {"done", e.done}, {"total", opt(e.total)}});
        }
        break;
    }
    case SyncEvent::Verified: emit({{"kind", "verified"}, {"path", e.path}, {"sha256", e.sha256}, {"size", e.done}}); break;
    case SyncEvent::Removed: emit({{"kind", "removed"}, {"path", e.path}}); break;
    case SyncEvent::Plan: break;
    }
    return std::nullopt;
}

bool Backend::Session::cancelled() const { return cancel_->load(); }

void Backend::Session::finish(const std::optional<std::string>& error) {
    if (shown_)
        emit({{"kind", "finish"}, {"error", error ? json(*error) : json(nullptr)}, {"cancelled", cancel_->load()}});
    shown_ = false;
}

void Backend::cancel_download(uint64_t id) {
    std::lock_guard<std::mutex> lock(downloads_->mu);
    auto it = downloads_->running.find(id);
    if (it != downloads_->running.end()) it->second->store(true);
}

}  // namespace sf::gui
