#include "launch/paths.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace sf::launch {

std::optional<std::string> get_env(const char* name) {
#ifdef _WIN32
    // The wide API, so the value isn't squeezed through the ANSI code page.
    const std::wstring wname = fs::path(name).wstring();
    DWORD n = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (n == 0) return std::nullopt;  // unset (an empty value also reads as 0: treated alike)
    std::wstring value(n, L'\0');
    value.resize(GetEnvironmentVariableW(wname.c_str(), value.data(), n));
    return ustr(fs::path(value));
#else
    const char* v = std::getenv(name);
    if (!v) return std::nullopt;
    return std::string(v);
#endif
}

void set_env(const char* name, const std::optional<std::string>& value) {
#ifdef _WIN32
    const std::wstring wname = fs::path(name).wstring();
    SetEnvironmentVariableW(wname.c_str(), value ? upath(*value).wstring().c_str() : nullptr);
#else
    if (value) setenv(name, value->c_str(), 1);
    else unsetenv(name);
#endif
}

std::optional<std::string> read_text(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool plain_part(const std::string& part) {
    if (part.empty() || part == "." || part == "..") return false;
    if (part.back() == '.' || part.back() == ' ') return false;
    for (size_t i = 0; i < part.size(); ++i) {
        unsigned char c = part[i];
        if (c < 0x20 || c == 0x7f) return false;
        // U+0080..U+009F (C1 controls) are UTF-8 C2 80..C2 9F.
        if (c == 0xc2 && i + 1 < part.size() && static_cast<unsigned char>(part[i + 1]) <= 0x9f) return false;
        if (std::string("<>:\"|?*").find(static_cast<char>(c)) != std::string::npos) return false;
    }
    // Device names: the part before the first dot, trailing spaces dropped, any case.
    std::string stem = part.substr(0, part.find('.'));
    while (!stem.empty() && (stem.back() == ' ' || stem.back() == '\t')) stem.pop_back();
    for (auto& c : stem)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul") return false;
    if (stem.size() == 4 && (stem.rfind("com", 0) == 0 || stem.rfind("lpt", 0) == 0) && stem[3] >= '0' && stem[3] <= '9')
        return false;
    return true;
}

}  // namespace sf::launch
