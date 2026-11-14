#include "patch/server_config.hpp"

#include <sstream>
#include <stdexcept>

#include <toml.hpp>

namespace sf::patch {
namespace {

std::string quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

}  // namespace

ServerConfig ServerConfig::parse(const std::string& text, const std::string& name) {
    toml::table root;
    try {
        root = toml::parse(text, name);
    } catch (const toml::parse_error& e) {
        std::ostringstream os;
        os << e;
        throw std::runtime_error(os.str());
    }
    ServerConfig out;
    if (auto node = root["on_error"]) {
        auto v = node.value<std::string>();
        if (!v || (*v != "exit" && *v != "continue")) throw std::runtime_error(name + ": on_error must be \"exit\" or \"continue\"");
        out.exit_on_error = *v == "exit";
    }
    if (auto node = root["patches"]) {
        const toml::table* t = node.as_table();
        if (!t) throw std::runtime_error(name + ": patches must be a table of per-exe tables");
        for (const auto& [exe, sub] : *t) {
            const toml::table* st = sub.as_table();
            if (!st)
                throw std::runtime_error(name + ": [patches] entries must be per-exe tables, e.g. [patches.\"Shard.exe\"]");
            auto& c = out.patches[std::string(exe.str())];
            for (const auto& [k, v] : *st) {
                auto on = v.value_exact<bool>();  // not value<bool>(), which takes 1 as true
                if (!on)
                    throw std::runtime_error(name + ": [patches.\"" + std::string(exe.str()) + "\"] " + std::string(k.str()) +
                                             " must be true or false");
                c[std::string(k.str())] = *on;
            }
        }
    }
    if (auto node = root["files"]) {
        const toml::table* t = node.as_table();
        if (!t) throw std::runtime_error(name + ": files must be a table");
        for (const char* key : {"overlay", "state"}) {
            if (auto v = (*t)[key]) {
                auto s = v.value_exact<std::string>();
                if (!s || s->empty()) throw std::runtime_error(name + ": [files] " + key + " must be a folder name or path");
                (std::string(key) == "overlay" ? out.overlay : out.state) = *s;
            }
        }
    }
    if (auto node = root["stack"]) {
        const toml::table* t = node.as_table();
        if (!t) throw std::runtime_error(name + ": stack must be a table of exe = megabytes");
        for (const auto& [exe, v] : *t) {
            auto mb = v.value_exact<int64_t>();
            if (!mb || *mb < 1 || *mb > 1024)
                throw std::runtime_error(name + ": [stack] " + std::string(exe.str()) + " must be a whole number of MB, 1 to 1024");
            out.stack_mb[std::string(exe.str())] = int(*mb);
        }
    }
    return out;
}

std::string ServerConfig::to_toml() const {
    std::string out =
        "# STAR FORGE patcher runtime config. Read by every server process at startup.\n"
        "# What to do when a patchset doesn't match the exe or a patch fails to verify:\n"
        "#   \"exit\"     -> the process refuses to start (recommended; you never run unpatched by accident)\n"
        "#   \"continue\" -> log the problem and run with whatever patches did verify\n";
    out += std::string("on_error = \"") + (exit_on_error ? "exit" : "continue") + "\"\n";
    const ServerConfig defaults;
    if (overlay != defaults.overlay || state != defaults.state) {
        out += "\n# Where changed game files are served from (relative to starforge/, or absolute).\n";
        out += "[files]\n";
        out += "overlay = " + quote(overlay) + "\n";
        out += "state = " + quote(state) + "\n";
    }
    if (!stack_mb.empty()) {
        out += "\n# Main thread stack, in MB, for exes whose stock 1 MB isn't enough.\n";
        out += "[stack]\n";
        for (const auto& [exe, mb] : stack_mb) out += quote(exe) + " = " + std::to_string(mb) + "\n";
    }
    out += "\n# Per-exe, per-patch overrides of each patch's `default`.\n";
    for (const auto& [module, c] : patches) {
        if (c.empty()) continue;
        out += "\n[patches." + quote(module) + "]\n";
        for (const auto& [id, on] : c) out += quote(id) + " = " + (on ? "true" : "false") + "\n";
    }
    return out;
}

}  // namespace sf::patch
