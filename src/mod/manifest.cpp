#include "mod/manifest.hpp"

#include <algorithm>

#include "pack/pack.hpp"
#include "pack/serde.hpp"

namespace sf::mod {
namespace {

using namespace pack::serde;
using pack::Error;

std::map<std::string, std::string> string_map(const json& v) {
    if (!v.is_object()) invalid(v, "a map");
    std::map<std::string, std::string> out;
    for (auto it = v.begin(); it != v.end(); ++it) out[it.key()] = str(it.value());
    return out;
}

Manifest read(const json& j) {
    Obj o(j, "Mod", {"$schema", "format", "id", "name", "version", "description", "authors", "links", "game", "needs",
                     "requires", "after", "before", "scripts", "lua", "server"});
    Manifest m;
    m.format = u32(o.req("format"));
    if (m.format == 0 || m.format > kModFormat)
        throw Error("format " + std::to_string(m.format) + " is not supported by this launcher (it reads format " +
                    std::to_string(kModFormat) + "); update the launcher");
    m.id = str(o.req("id"));
    if (!valid_id(m.id)) throw Error("id " + debug_str(m.id) + ": use lowercase letters, digits and '-'");
    if (m.id == kSfmodId) throw Error("id \"sfmod\" is the launcher's own; pick another");
    if (const json* v = o.opt("name")) m.name = str(*v);
    if (m.name.empty()) m.name = m.id;
    if (const json* v = o.opt("version")) m.version = str(*v);
    if (!m.version.empty() && !pack::parse_version(m.version))
        throw Error("version " + debug_str(m.version) + ": expected x.y.z");
    if (const json* v = o.opt("description")) m.description = str(*v);
    if (const json* v = o.opt("authors")) m.authors = strings(*v);
    if (const json* v = o.opt("links")) m.links = string_map(*v);
    if (const json* v = o.opt("game")) m.game = strings(*v);
    for (auto& h : m.game) {
        if (!is_hex(h, 64)) throw Error("game: " + debug_str(h) + " is not a SHA-256");
        for (char& c : h)
            if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
    }
    if (const json* v = o.opt("needs")) m.needs = strings(*v);
    if (const json* v = o.opt("requires")) m.requires_ = string_map(*v);
    if (const json* v = o.opt("after")) m.after = strings(*v);
    if (const json* v = o.opt("before")) m.before = strings(*v);
    if (const json* v = o.opt("scripts")) {
        if (!v->is_array()) invalid(*v, "a sequence");
        for (const json& e : *v) {
            Obj s(e, "Script", {"file", "after", "type", "groups"});
            Script sc;
            sc.file = str(s.req("file"));
            if (const json* a = s.opt("after")) sc.after = str(*a);
            if (const json* t = s.opt("type")) sc.type = str(*t);
            if (const json* g = s.opt("groups")) sc.groups = str(*g);
            if (sc.type != "SCRIPT" && sc.type != "TEXTURE")
                throw Error("scripts: type " + debug_str(sc.type) + ", expected \"SCRIPT\" or \"TEXTURE\"");
            for (const std::string* f : {&sc.file, sc.after ? &*sc.after : nullptr, sc.groups ? &*sc.groups : nullptr})
                if (f && f->find_first_of("\"[]") != std::string::npos)
                    throw Error("scripts: " + debug_str(*f) + " may not contain \" [ or ]");
            m.scripts.push_back(std::move(sc));
        }
    }
    if (const json* v = o.opt("lua")) {
        Obj l(*v, "Lua", {"replaces"});
        if (const json* r = l.opt("replaces")) m.lua_replaces = strings(*r);
    }
    if (const json* v = o.opt("server")) m.server = boolean(*v);
    return m;
}

}  // namespace

Manifest Manifest::from_json(const std::string& text) { return parse<Manifest>(text, kManifestFile, read); }

bool valid_id(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    auto alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    if (!alnum(id.front()) || !alnum(id.back())) return false;
    for (std::size_t i = 0; i < id.size(); ++i) {
        char c = id[i];
        if (alnum(c)) continue;
        if ((c == '-' || c == '.' || c == '_') && alnum(id[i - 1])) continue;
        return false;
    }
    return true;
}

bool version_matches(const std::string& version, const std::string& range) {
    auto have = pack::parse_version(version);
    std::size_t p = 0;
    bool any = false;
    while (p < range.size()) {
        while (p < range.size() && range[p] == ' ') ++p;
        if (p >= range.size()) break;
        std::size_t e = range.find(' ', p);
        std::string part = range.substr(p, e == std::string::npos ? std::string::npos : e - p);
        p = e == std::string::npos ? range.size() : e;
        any = true;
        if (part == "*") continue;
        if (!have) return false;
        std::string op;
        while (!part.empty() && (part[0] == '>' || part[0] == '<' || part[0] == '=')) {
            op += part[0];
            part.erase(0, 1);
        }
        // "1" and "1.0" mean 1.0.0.
        std::size_t dots = static_cast<std::size_t>(std::count(part.begin(), part.end(), '.'));
        for (std::size_t i = dots; i < 2 && !part.empty() && part.find_first_of("-+") == std::string::npos; ++i) part += ".0";
        auto want = pack::parse_version(part);
        if (!want) return false;
        bool ok = op == ">=" ? *have >= *want
                  : op == ">"  ? *have > *want
                  : op == "<=" ? *have <= *want
                  : op == "<"  ? *have < *want
                  : (op.empty() || op == "=") ? *have == *want
                                              : false;
        if (!ok) return false;
    }
    return any;
}

}  // namespace sf::mod
