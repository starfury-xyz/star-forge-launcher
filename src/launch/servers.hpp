#pragma once

// Server installs: the game's cloud/ folders the player hosts a server from, kept in
// <data dir>/servers.json so the launcher can list them, check them and switch their patches
// (see launch::mods for what's in one). Only the list lives here; nothing is copied. Portable.
//
// servers.json: {"servers": [{"id": "testbed", "name": "Testbed", "cloud": "/path/to/cloud"}]}

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "launch/paths.hpp"

namespace sf::launch::servers {

struct Server {
    std::string id;    // a slug, unique in the list
    std::string name;  // shown to the player
    fs::path cloud;    // the game's cloud/ folder
};

inline constexpr const char* kFile = "servers.json";

// The registry under `data_dir` (profiles::data_dir() unless a test passes its own). A missing
// file is an empty list. Throws std::runtime_error for an unreadable or malformed file.
std::vector<Server> list(const fs::path& data_dir);
// Adds a server install; its id comes from the name (slug, made unique). Throws
// std::runtime_error if `cloud` isn't a folder or is already in the list.
Server add(const fs::path& data_dir, const std::string& name, const fs::path& cloud);
// Removes one from the list (its folder is left alone). Throws for an unknown id.
void remove(const fs::path& data_dir, const std::string& id);
// The one with this id. Throws std::runtime_error for an unknown id.
Server get(const fs::path& data_dir, const std::string& id);

// The cloud/ folder of a game install, if it has one.
std::optional<fs::path> game_cloud(const fs::path& game_dir);

// Whether a server install's stack is up, from the starforge/server.json that sc-server.exe keeps
// while it runs (rewritten every few seconds).
struct RunState {
    enum State {
        Stopped,  // no server.json
        Running,  // a fresh heartbeat
        Stale,    // server.json left behind: its supervisor was killed (or hangs)
    } state = Stopped;
    int64_t started = 0;      // unix time
    unsigned mongo_port = 0;  // the port its MongoDB is on
    bool own_mongo = false;   // sc-server started that MongoDB (and stops it)
};
inline constexpr int64_t kStaleAfter = 15;  // seconds without a heartbeat
RunState run_state(const fs::path& cloud, int64_t now);

// A server's logs: STAR FORGE's own in starforge/logs (the proxy's, one per exe; sc-server's;
// MongoDB's) and the game's in Hub's output folder, <LocalAppData>/Targem/StarConflict/cloud/
// Hub/logs. The game keeps a file a day per service there, for every server the machine (or
// Proton prefix) ever ran; only the newest per service is listed.
struct LogFile {
    std::string group;  // "starforge" or "game"
    std::string label;  // "Shard", "sc-server", "shard (console)", ...
    std::string name;   // the file's name, which picks it in log_tail
    fs::path path;
    uint64_t size = 0;
    int64_t modified = 0;  // unix time
};
inline constexpr const char* kGameLogs = "Targem/StarConflict/cloud/Hub/logs";  // under LocalAppData
// Game logs first, by label; `local_appdata` unset leaves them out.
std::vector<LogFile> logs(const fs::path& cloud, const std::optional<fs::path>& local_appdata);
// "1010000000000002_shard_2026_11_14.log" -> "shard"; ".conlog" -> "shard (console)".
std::string game_log_label(const std::string& file_name);
// The last `max_bytes` of a log, from the start of a line. Throws std::runtime_error.
std::string log_tail(const fs::path& file, size_t max_bytes, bool* cut = nullptr);

}  // namespace sf::launch::servers
