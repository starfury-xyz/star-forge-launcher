#include "launch/env.hpp"

#ifndef _WIN32
extern char** environ;  // POSIX: this process's environment
#endif

namespace sf::launch::env {
namespace {

std::string trim(const std::string& s) {
    const char* ws = " \t\r\n\v\f";
    size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(ws) - a + 1);
}

}  // namespace

std::optional<fs::path> path(const char* name) {
    auto v = get_env(name);
    if (!v || v->empty()) return std::nullopt;
    return upath(*v);
}

void load_dotenv() {
#ifdef SF_DOTENV_FILE
    if (auto text = read_text(upath(SF_DOTENV_FILE)))
        for (const auto& [key, value] : parse_dotenv(*text))
            if (!get_env(key.c_str())) set_env(key.c_str(), value);
#endif
}

std::vector<std::pair<std::string, std::string>> parse_dotenv(const std::string& text) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = trim(text.substr(start, end - start));
        start = end + 1;
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("export ", 0) == 0) line = line.substr(7);
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));
        if (value.size() >= 2 && (value[0] == '"' || value[0] == '\'') && value.back() == value[0])
            value = value.substr(1, value.size() - 2);
        if (!key.empty()) out.emplace_back(key, value);
    }
    return out;
}

Changes host_env() {
    auto appimage = get_env("APPIMAGE");
    auto appdir = get_env("APPDIR");
    if (!appimage || !appdir) return {};
    std::vector<std::pair<std::string, std::string>> vars;
#ifndef _WIN32
    for (char** e = environ; *e; ++e) {
        std::string kv = *e;
        size_t eq = kv.find('=');
        if (eq != std::string::npos) vars.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
    }
#endif  // (AppImages are Linux-only)
    return host_env_changes(vars, *appdir);
}

Changes host_env_changes(const std::vector<std::pair<std::string, std::string>>& vars, std::string appdir) {
    while (!appdir.empty() && appdir.back() == '/') appdir.pop_back();
    Changes out;
    if (appdir.empty()) return out;
    for (const auto& [key, value] : vars) {
        if (key == "GTK_THEME" || key == "PYTHONDONTWRITEBYTECODE") {
            out.emplace_back(key, std::nullopt);
        } else if (value.find(appdir) != std::string::npos) {
            std::string kept;
            size_t start = 0;
            while (start <= value.size()) {
                size_t end = value.find(':', start);
                if (end == std::string::npos) end = value.size();
                std::string part = value.substr(start, end - start);
                if (!part.empty() && part.find(appdir) == std::string::npos) kept += (kept.empty() ? "" : ":") + part;
                start = end + 1;
            }
            out.emplace_back(key, kept.empty() ? std::nullopt : std::optional<std::string>(kept));
        }
    }
    return out;
}

}  // namespace sf::launch::env
