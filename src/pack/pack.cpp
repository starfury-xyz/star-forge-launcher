#include "pack/pack.hpp"

#include <algorithm>

#include <nlohmann/json.hpp>

namespace sf::pack {
namespace {

using nlohmann::json;
using ojson = nlohmann::ordered_json;  // keeps keys in insertion order, for writing
using launch::ustr;

// Rust's {:?} for a string: in quotes, with ", \ and control characters escaped.
std::string debug_str(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20 || c == 0x7f) {
                char buf[16];
                std::snprintf(buf, sizeof buf, "\\u{%x}", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out + "\"";
}

bool is_hex(const std::string& s, size_t len) {
    return s.size() == len && std::all_of(s.begin(), s.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
           });
}

// --- strict reading, with serde-like messages ---

std::string kind(const json& v) {
    if (v.is_null()) return "null";
    if (v.is_boolean()) return std::string("boolean `") + (v.get<bool>() ? "true" : "false") + "`";
    if (v.is_number_integer()) return "integer `" + v.dump() + "`";
    if (v.is_number_float()) return "floating point `" + v.dump() + "`";
    if (v.is_string()) return "string " + debug_str(v.get<std::string>());
    if (v.is_array()) return "sequence";
    return "map";
}

[[noreturn]] void invalid(const json& v, const std::string& expected) {
    throw Error("invalid type: " + kind(v) + ", expected " + expected);
}

// One JSON object being read as a struct: unknown keys are refused up front.
class Obj {
public:
    Obj(const json& j, const char* type, std::vector<const char*> fields) : j_(j) {
        if (!j.is_object()) invalid(j, std::string("struct ") + type);
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (std::find_if(fields.begin(), fields.end(), [&](const char* f) { return it.key() == f; }) != fields.end())
                continue;
            std::string expected;
            for (size_t i = 0; i < fields.size(); ++i)
                expected += std::string(i == 0 ? "" : (i + 1 == fields.size() ? " or " : ", ")) + "`" + fields[i] + "`";
            throw Error("unknown field `" + it.key() + "`, expected " + (fields.size() > 1 ? "one of " : "") + expected);
        }
    }
    // The value, or nullptr when the key is missing.
    const json* get(const char* key) const {
        auto it = j_.find(key);
        return it == j_.end() ? nullptr : &*it;
    }
    // Optional<T> fields: missing or null both mean "not set".
    const json* opt(const char* key) const {
        const json* v = get(key);
        return v && !v->is_null() ? v : nullptr;
    }
    const json& req(const char* key) const {
        const json* v = get(key);
        if (!v) throw Error(std::string("missing field `") + key + "`");
        return *v;
    }

private:
    const json& j_;
};

std::string str(const json& v) {
    if (!v.is_string()) invalid(v, "a string");
    return v.get<std::string>();
}
bool boolean(const json& v) {
    if (!v.is_boolean()) invalid(v, "a boolean");
    return v.get<bool>();
}
uint32_t u32(const json& v) {
    if (!v.is_number_unsigned() || v.get<uint64_t>() > 0xffffffffull) {
        if (v.is_number_integer()) throw Error("invalid value: integer `" + v.dump() + "`, expected u32");
        invalid(v, "u32");
    }
    return static_cast<uint32_t>(v.get<uint64_t>());
}
std::vector<std::string> strings(const json& v) {
    if (!v.is_array()) invalid(v, "a sequence");
    std::vector<std::string> out;
    for (const auto& e : v) out.push_back(str(e));
    return out;
}
// Branding.background: a string or a list of strings.
std::vector<std::string> one_or_many(const json& v) {
    if (v.is_string()) return {v.get<std::string>()};
    if (v.is_array()) {
        std::vector<std::string> out;
        for (const auto& e : v) {
            if (!e.is_string()) throw Error("data did not match any variant of untagged enum Value");
            out.push_back(e.get<std::string>());
        }
        return out;
    }
    throw Error("data did not match any variant of untagged enum Value");
}

std::optional<std::string> opt_str(const Obj& o, const char* key) {
    if (const json* v = o.opt(key)) return str(*v);
    return std::nullopt;
}

Patches read_patches(const json& j) {
    Obj o(j, "Patches", {"method", "on_error", "plugins", "core"});
    Patches p;
    if (const json* v = o.get("method")) {
        std::string m = str(*v);
        if (m != "inject") throw Error("unknown variant `" + m + "`, expected `inject`");
    }
    if (const json* v = o.get("on_error")) {
        std::string e = str(*v);
        if (e == "exit") p.on_error = OnError::Exit;
        else if (e == "continue") p.on_error = OnError::Continue;
        else throw Error("unknown variant `" + e + "`, expected `exit` or `continue`");
    }
    if (const json* v = o.get("plugins")) p.plugins = strings(*v);
    if (const json* v = o.get("core")) p.core = strings(*v);
    return p;
}

Pack read_pack(const json& j) {
    Obj o(j, "Pack",
          {"$schema", "format", "id", "name", "version", "description", "authors", "links", "requires", "server", "game",
           "branding", "files_from", "patches", "update", "download"});
    Pack p;
    p.schema = opt_str(o, "$schema");
    p.format = u32(o.req("format"));
    p.id = str(o.req("id"));
    p.name = str(o.req("name"));
    p.version = str(o.req("version"));
    if (const json* v = o.get("description")) p.description = str(*v);
    if (const json* v = o.get("authors")) p.authors = strings(*v);
    if (const json* v = o.get("links")) {
        Obj l(*v, "Links", {"website", "discord", "news", "source"});
        p.links = {opt_str(l, "website"), opt_str(l, "discord"), opt_str(l, "news"), opt_str(l, "source")};
    }
    if (const json* v = o.get("requires")) {
        Obj r(*v, "Requires", {"launcher", "game"});
        p.requires_.launcher = opt_str(r, "launcher");
        if (const json* g = r.get("game")) p.requires_.game = strings(*g);
    }
    if (const json* v = o.get("server")) {
        Obj s(*v, "Server", {"address", "local"});
        p.server.address = opt_str(s, "address");
        if (const json* l = s.get("local")) p.server.local = boolean(*l);
    }
    if (const json* v = o.get("game")) {
        Obj g(*v, "Game", {"args"});
        if (const json* a = g.get("args")) p.game.args = strings(*a);
    }
    if (const json* v = o.get("branding")) {
        Obj b(*v, "Branding", {"logo", "icon", "background", "accent"});
        p.branding.logo = opt_str(b, "logo");
        p.branding.icon = opt_str(b, "icon");
        if (const json* bg = b.get("background")) p.branding.background = one_or_many(*bg);
        p.branding.accent = opt_str(b, "accent");
    }
    if (const json* v = o.opt("files_from")) {
        Obj f(*v, "FilesFrom", {"manifest", "scmod1"});
        p.files_from = FilesFrom{opt_str(f, "manifest"), opt_str(f, "scmod1")};
    }
    if (const json* v = o.get("patches")) p.patches = read_patches(*v);
    if (const json* v = o.opt("update")) {
        Obj u(*v, "Update", {"url", "min_version"});
        p.update = Update{str(u.req("url")), opt_str(u, "min_version")};
    }
    if (const json* v = o.opt("download")) {
        Obj d(*v, "Download", {"url", "sha256"});
        p.download = Download{str(d.req("url")), str(d.req("sha256"))};
    }
    return p;
}

Local read_local(const json& j) {
    Obj o(j, "Local", {"name", "server", "local_server", "args", "trace_files", "patches", "shared_game_data"});
    Local l;
    l.name = opt_str(o, "name");
    l.server = opt_str(o, "server");
    if (const json* v = o.opt("local_server")) l.local_server = boolean(*v);
    if (const json* v = o.get("args")) l.args = strings(*v);
    if (const json* v = o.get("trace_files")) l.trace_files = boolean(*v);
    if (const json* v = o.get("patches")) {
        if (!v->is_object()) invalid(*v, "a map");
        for (auto m = v->begin(); m != v->end(); ++m) {
            if (!m.value().is_object()) invalid(m.value(), "a map");
            for (auto p = m.value().begin(); p != m.value().end(); ++p) l.patches[m.key()][p.key()] = boolean(p.value());
        }
    }
    if (const json* v = o.get("shared_game_data")) l.shared_game_data = boolean(*v);
    return l;
}

// Parses `text`, then reads it with `read`; any error gets the file name in front.
template <class T, class F>
T parse(const std::string& text, const char* file, F read) {
    try {
        return read(json::parse(text));
    } catch (const json::parse_error& e) {
        // nlohmann's "[json.exception.parse_error.101] parse error at line 1, column 2: ..."
        std::string m = e.what();
        size_t at = m.find("] ");
        throw Error(std::string(file) + ": " + (at == std::string::npos ? m : m.substr(at + 2)));
    } catch (const Error& e) {
        throw Error(std::string(file) + ": " + e.what());
    }
}

std::optional<std::string> rel_inside(const char* dir, const std::string& path) {
    const std::string prefix = std::string(dir) + "/";
    if (path.rfind(prefix, 0) != 0) return std::nullopt;
    std::string rel = path.substr(prefix.size());
    if (rel.empty() || rel.find_first_of("\\:") != std::string::npos) return std::nullopt;
    size_t start = 0;
    while (start <= rel.size()) {
        size_t end = rel.find('/', start);
        if (end == std::string::npos) end = rel.size();
        std::string part = rel.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") return std::nullopt;
        start = end + 1;
    }
    return rel;
}

std::string lower(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

std::vector<std::pair<std::string, std::string>> Links::entries() const {
    std::vector<std::pair<std::string, std::string>> out;
    for (auto [k, v] : {std::pair{"website", &website}, {"discord", &discord}, {"news", &news}, {"source", &source}})
        if (*v) out.emplace_back(k, **v);
    return out;
}

const std::string& FilesFrom::url() const {
    static const std::string none;
    return manifest ? *manifest : scmod1 ? *scmod1 : none;
}

Pack Pack::from_json(const std::string& text) { return parse<Pack>(text, kPackFile, read_pack); }
Local Local::from_json(const std::string& text) { return parse<Local>(text, kLocalFile, read_local); }

std::string Pack::to_json() const {
    ojson j;
    if (schema) j["$schema"] = *schema;
    j["format"] = format;
    j["id"] = id;
    j["name"] = name;
    j["version"] = version;
    if (!description.empty()) j["description"] = description;
    if (!authors.empty()) j["authors"] = authors;
    if (!links.empty()) {
        ojson l = ojson::object();
        for (const auto& [k, v] : links.entries()) l[k] = v;
        j["links"] = l;
    }
    if (!requires_.empty()) {
        ojson r = ojson::object();
        if (requires_.launcher) r["launcher"] = *requires_.launcher;
        if (!requires_.game.empty()) r["game"] = requires_.game;
        j["requires"] = r;
    }
    ojson s = ojson::object();
    if (server.address) s["address"] = *server.address;
    s["local"] = server.local;
    j["server"] = s;
    if (!game.empty()) j["game"] = ojson{{"args", game.args}};
    if (!branding.empty()) {
        ojson b = ojson::object();
        if (branding.logo) b["logo"] = *branding.logo;
        if (branding.icon) b["icon"] = *branding.icon;
        if (branding.background.size() == 1) b["background"] = branding.background[0];  // one: written as a string
        else if (!branding.background.empty()) b["background"] = branding.background;
        if (branding.accent) b["accent"] = *branding.accent;
        j["branding"] = b;
    }
    if (files_from) {
        ojson f = ojson::object();
        if (files_from->manifest) f["manifest"] = *files_from->manifest;
        if (files_from->scmod1) f["scmod1"] = *files_from->scmod1;
        j["files_from"] = f;
    }
    if (!patches.is_default()) {
        ojson p = ojson::object();
        p["method"] = "inject";
        p["on_error"] = patches.on_error == OnError::Exit ? "exit" : "continue";
        if (!patches.plugins.empty()) p["plugins"] = patches.plugins;
        if (!patches.core.empty()) p["core"] = patches.core;
        j["patches"] = p;
    }
    if (update) {
        ojson u = ojson::object();
        u["url"] = update->url;
        if (update->min_version) u["min_version"] = *update->min_version;
        j["update"] = u;
    }
    if (download) j["download"] = ojson{{"url", download->url}, {"sha256", download->sha256}};
    return j.dump(2) + "\n";
}

std::string Local::to_json() const {
    ojson j = ojson::object();
    if (name) j["name"] = *name;
    if (server) j["server"] = *server;
    if (local_server) j["local_server"] = *local_server;
    if (!args.empty()) j["args"] = args;
    if (trace_files) j["trace_files"] = true;
    if (!patches.empty()) {
        ojson p = ojson::object();
        for (const auto& [module, choices] : patches) {
            ojson c = ojson::object();
            for (const auto& [id, on] : choices) c[id] = on;
            p[module] = c;
        }
        j["patches"] = p;
    }
    if (shared_game_data) j["shared_game_data"] = true;
    return j.dump(2) + "\n";
}

void Pack::validate() const {
    auto err = [](const std::string& m) { throw Error(std::string(kPackFile) + ": " + m); };
    if (format == 0 || format > kPackFormat)
        err("format " + std::to_string(format) + " is not supported by this launcher (it reads format " +
            std::to_string(kPackFormat) + "); update the launcher");
    if (!valid_pack_id(id)) err("id " + debug_str(id) + ": use lower-case letters, digits, '.' and '-' (e.g. org.example.server)");
    if (name.find_first_not_of(" \t\r\n") == std::string::npos) err("name is empty");
    if (!parse_version(version)) err("version " + debug_str(version) + ": expected x.y.z");
    if (server.address) check_address(*server.address);
    std::vector<std::pair<const char*, std::vector<std::string>>> images{{"logo", {}}, {"icon", {}}, {"background", branding.background}};
    if (branding.logo) images[0].second.push_back(*branding.logo);
    if (branding.icon) images[1].second.push_back(*branding.icon);
    for (const auto& [key, paths] : images)
        for (const auto& p : paths)
            if (!resource_rel(p)) err(std::string("branding.") + key + " " + debug_str(p) + ": must be a file inside resources/");
    if (branding.accent && !(branding.accent->size() == 7 && (*branding.accent)[0] == '#' && is_hex(branding.accent->substr(1), 6)))
        err("branding.accent " + debug_str(*branding.accent) + ": expected a colour like #e39a29");
    for (const auto& h : requires_.game)
        if (!is_hex(h, 64)) err("requires.game " + debug_str(h) + ": expected a SHA-256 in hex");
    if (update && update->min_version && !parse_version(*update->min_version))
        err("update.min_version " + debug_str(*update->min_version) + ": expected x.y.z");
    if (download && !is_hex(download->sha256, 64)) err("download.sha256: expected a SHA-256 in hex");
    if (files_from) {
        if (bool(files_from->manifest) == bool(files_from->scmod1)) err("files_from: set exactly one of manifest and scmod1");
        const std::string& url = files_from->url();
        if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) err("files_from: the list must be an http(s) link");
    }
    for (const auto& c : patches.core)
        if (c.empty() || c.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != std::string::npos)
            err("patches.core " + debug_str(c) + ": expected a core patch id such as \"loose-before-packed\"");
    for (const auto& p : patches.plugins) {
        auto rel = patches_rel(p);
        if (!rel || !ends_with(lower(*rel), ".dll")) err("patches.plugins " + debug_str(p) + ": must be a .dll inside patches/");
    }
}

Profile Profile::from(const Pack& pack, const Local& local) {
    Profile p;
    if (local.server) p.server = local.server->empty() ? std::nullopt : local.server;
    else p.server = pack.server.address;
    p.name = local.name && local.name->find_first_not_of(" \t\r\n") != std::string::npos ? *local.name : pack.name;
    p.local_server = local.local_server.value_or(pack.server.local);
    p.args = pack.game.args;
    p.args.insert(p.args.end(), local.args.begin(), local.args.end());
    p.trace_files = local.trace_files;
    p.patches = pack.patches;
    p.patch_choices = local.patches;
    p.own_game_data = !local.shared_game_data;
    return p;
}

Profile Profile::load(const fs::path& dir) {
    std::string text;
    try {
        text = launch::read_or_throw(dir / kPackFile);
    } catch (const std::runtime_error& e) {
        throw Error(e.what());
    }
    Pack pack = Pack::from_json(text);
    pack.validate();
    Local local;
    std::error_code ec;
    if (auto t = launch::read_text(dir / kLocalFile, &ec)) local = Local::from_json(*t);
    else if (!launch::not_found(ec)) throw Error(std::string(kLocalFile) + ": " + ec.message() + " (os error " + std::to_string(ec.value()) + ")");
    Profile profile = from(pack, local);
    profile.address_cfg();
    return profile;
}

std::vector<std::string> Profile::full_args() const {
    std::vector<std::string> out;
    if (local_server) out.push_back("-launchLocalServer");
    out.insert(out.end(), args.begin(), args.end());
    return out;
}

std::optional<std::string> Profile::address_cfg() const {
    if (!server) return std::nullopt;
    check_address(*server);
    return "set net_masterServerAddress \"" + *server + "\"\r\n";
}

bool valid_pack_id(const std::string& id) {
    if (id.empty() || id.size() > 128) return false;
    size_t start = 0;
    while (start <= id.size()) {
        size_t end = id.find_first_of(".-", start);
        if (end == std::string::npos) end = id.size();
        if (end == start) return false;
        for (size_t i = start; i < end; ++i)
            if (!((id[i] >= 'a' && id[i] <= 'z') || (id[i] >= '0' && id[i] <= '9'))) return false;
        start = end + 1;
    }
    return true;
}

std::optional<std::tuple<uint64_t, uint64_t, uint64_t>> parse_version(const std::string& v) {
    const std::string core = v.substr(0, v.find_first_of("-+"));
    uint64_t n[3];
    size_t start = 0;
    for (int i = 0; i < 3; ++i) {
        size_t end = core.find('.', start);
        if ((i < 2) == (end == std::string::npos)) return std::nullopt;  // exactly three parts
        if (end == std::string::npos) end = core.size();
        std::string part = core.substr(start, end - start);
        if (part.empty() || part.size() > 20 || part.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
        // Overflow check: 20 digits can exceed u64.
        unsigned long long x = 0;
        for (char c : part) {
            unsigned d = static_cast<unsigned>(c - '0');
            if (x > (UINT64_MAX - d) / 10) return std::nullopt;
            x = x * 10 + d;
        }
        n[i] = x;
        start = end + 1;
    }
    return std::tuple{n[0], n[1], n[2]};
}

std::optional<std::string> resource_rel(const std::string& path) { return rel_inside(kResourcesDir, path); }
std::optional<std::string> patches_rel(const std::string& path) { return rel_inside(kPatchesDir, path); }

void check_address(const std::string& s) {
    bool ok = !s.empty() && s.size() <= 255 && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
               c == ':' || c == '_';
    });
    if (!ok) throw Error("server " + debug_str(s) + ": expected host:port");
}

std::vector<fs::path> patchset_files(const fs::path& profile_dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it(profile_dir / kPatchesDir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2) && ends_with(lower(ustr(it->path().filename())), ".patchset.toml")) out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace sf::pack
