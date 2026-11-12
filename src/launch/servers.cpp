#include "launch/servers.hpp"

#include <algorithm>
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

}  // namespace sf::launch::servers
