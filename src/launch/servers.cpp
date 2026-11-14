#include "launch/servers.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "launch/profiles.hpp"

namespace sf::launch::servers {
namespace {

using nlohmann::json;

void save(const fs::path& data_dir, const std::vector<Server>& all) {
    json arr = json::array();
    for (const Server& s : all) arr.push_back({{"id", s.id}, {"name", s.name}, {"cloud", ustr(s.cloud)}});
    std::error_code ec;
    fs::create_directories(data_dir, ec);
    if (ec) throw std::runtime_error(io_error(data_dir, ec));
    profiles::write_atomic(data_dir / kFile, json{{"servers", arr}}.dump(2) + "\n");
}

bool same_folder(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (fs::equivalent(a, b, ec)) return true;
    return fs::weakly_canonical(a, ec) == fs::weakly_canonical(b, ec);
}

}  // namespace

std::vector<Server> list(const fs::path& data_dir) {
    const fs::path file = data_dir / kFile;
    std::error_code ec;
    auto text = read_text(file, &ec);
    if (!text) {
        if (not_found(ec)) return {};
        throw std::runtime_error(io_error(file, ec));
    }
    std::vector<Server> out;
    try {
        const json j = json::parse(*text);
        for (const json& s : j.at("servers"))
            out.push_back({s.at("id").get<std::string>(), s.at("name").get<std::string>(), upath(s.at("cloud").get<std::string>())});
    } catch (const json::exception& e) {
        throw std::runtime_error(std::string(kFile) + ": " + e.what());
    }
    return out;
}

Server add(const fs::path& data_dir, const std::string& name, const fs::path& cloud) {
    std::error_code ec;
    if (!fs::is_directory(cloud, ec)) throw std::runtime_error(ustr(cloud) + " is not a folder");
    std::vector<Server> all = list(data_dir);
    for (const Server& s : all)
        if (same_folder(s.cloud, cloud)) throw std::runtime_error(ustr(cloud) + " is already in the list, as " + s.name);
    const std::string base = profiles::slug(name.empty() ? ustr(cloud.parent_path().filename()) : name);
    std::string id = base;
    for (int n = 2; std::any_of(all.begin(), all.end(), [&](const Server& s) { return s.id == id; }); ++n)
        id = base + "-" + std::to_string(n);
    Server s{id, name.empty() ? ustr(cloud.parent_path().filename()) : name, fs::absolute(cloud, ec)};
    all.push_back(s);
    save(data_dir, all);
    return s;
}

void remove(const fs::path& data_dir, const std::string& id) {
    std::vector<Server> all = list(data_dir);
    auto it = std::find_if(all.begin(), all.end(), [&](const Server& s) { return s.id == id; });
    if (it == all.end()) throw std::runtime_error("no server install \"" + id + "\"");
    all.erase(it);
    save(data_dir, all);
}

Server get(const fs::path& data_dir, const std::string& id) {
    for (Server& s : list(data_dir))
        if (s.id == id) return s;
    throw std::runtime_error("no server install \"" + id + "\"");
}

std::optional<fs::path> game_cloud(const fs::path& game_dir) {
    std::error_code ec;
    const fs::path cloud = game_dir / "cloud";
    if (fs::is_directory(cloud, ec)) return cloud;
    return std::nullopt;
}

RunState run_state(const fs::path& cloud, int64_t now) {
    RunState st;
    auto text = read_text(cloud / "starforge" / "server.json");
    if (!text) return st;
    st.state = RunState::Stale;  // unless it reads as fresh
    try {
        const json j = json::parse(*text);
        st.started = j.at("started").get<int64_t>();
        st.mongo_port = j.at("mongoPort").get<unsigned>();
        st.own_mongo = j.at("mongod").get<int64_t>() != 0;
        if (now - j.at("heartbeat").get<int64_t>() <= kStaleAfter) st.state = RunState::Running;
    } catch (const std::exception&) {
        // half-written or foreign: stale
    }
    return st;
}

namespace {

int64_t mtime(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    if (ec) return 0;
    // file_clock -> system_clock, portably in C++17: shift by the two clocks' "now".
    const auto sys = std::chrono::system_clock::now() + (t - fs::file_time_type::clock::now());
    return std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
}

LogFile log_file(const std::string& group, std::string label, const fs::path& p) {
    std::error_code ec;
    const auto size = fs::file_size(p, ec);
    return LogFile{group, std::move(label), ustr(p.filename()), p, ec ? 0 : uint64_t(size), mtime(p)};
}

}  // namespace

std::string game_log_label(const std::string& file_name) {
    std::string s = file_name;
    bool console = false;
    for (const char* ext : {".conlog", ".log"}) {
        const std::string e = ext;
        if (s.size() > e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0) {
            s.resize(s.size() - e.size());
            console = e == ".conlog";
            break;
        }
    }
    // _YYYY_MM_DD at the end
    auto digits = [&](size_t at, size_t n) {
        if (at + n > s.size()) return false;
        for (size_t i = at; i < at + n; ++i)
            if (s[i] < '0' || s[i] > '9') return false;
        return true;
    };
    if (s.size() > 11 && s[s.size() - 11] == '_' && digits(s.size() - 10, 4) && s[s.size() - 6] == '_' &&
        digits(s.size() - 5, 2) && s[s.size() - 3] == '_' && digits(s.size() - 2, 2))
        s.resize(s.size() - 11);
    // the service's cloud address in hex at the front
    const size_t us = s.find('_');
    if (us != std::string::npos && us >= 8 &&
        std::all_of(s.begin(), s.begin() + long(us), [](char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'); }))
        s.erase(0, us + 1);
    return console ? s + " (console)" : s;
}

std::vector<LogFile> logs(const fs::path& cloud, const std::optional<fs::path>& local_appdata) {
    std::vector<LogFile> game, ours;
    std::error_code ec;
    if (local_appdata) {
        std::map<std::string, LogFile> newest;  // by label
        for (fs::directory_iterator it(*local_appdata / kGameLogs, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const std::string name = ustr(it->path().filename());
            if (name.size() < 5 || (name.compare(name.size() - 4, 4, ".log") != 0 && name.find(".conlog") == std::string::npos))
                continue;
            LogFile f = log_file("game", game_log_label(name), it->path());
            if (f.size == 0 && f.label.find("(console)") != std::string::npos) continue;  // usually empty
            auto [at, fresh] = newest.emplace(f.label, f);
            if (!fresh && (f.modified > at->second.modified || (f.modified == at->second.modified && f.name > at->second.name)))
                at->second = f;
        }
        for (auto& [label, f] : newest) game.push_back(std::move(f));
    }
    ec.clear();
    for (fs::directory_iterator it(cloud / "starforge" / "logs", ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const fs::path p = it->path();
        std::string label = ustr(p.stem());
        if (p.extension() == ".stdout") label += " (output)";
        else if (p.extension() != ".log") continue;
        ours.push_back(log_file("starforge", label, p));
    }
    std::sort(ours.begin(), ours.end(), [](const LogFile& a, const LogFile& b) { return a.label < b.label; });
    game.insert(game.end(), ours.begin(), ours.end());
    return game;
}

std::string log_tail(const fs::path& file, size_t max_bytes, bool* cut) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + ustr(file));
    in.seekg(0, std::ios::end);
    const auto size = size_t(in.tellg());
    const size_t start = size > max_bytes ? size - max_bytes : 0;
    in.seekg(std::streamoff(start));
    std::string text(size - start, '\0');
    in.read(text.data(), std::streamsize(text.size()));
    text.resize(size_t(in.gcount()));
    if (start > 0) {
        const size_t nl = text.find('\n');
        text.erase(0, nl == std::string::npos ? 0 : nl + 1);
    }
    if (cut) *cut = start > 0;
    return text;
}

}  // namespace sf::launch::servers
