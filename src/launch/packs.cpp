#include "launch/packs.hpp"

#include <atomic>
#include <chrono>
#include <stdexcept>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include "launch/profiles.hpp"

namespace sf::launch::packs {
namespace {

// Folder for packs being reviewed. It sits inside the profiles folder so installing is a rename
// on the same drive (a rename can't cross drives).
fs::path pack_staging_root(const fs::path& profiles_dir) { return profiles_dir / ".staging"; }

fs::path new_staging_dir_in(const fs::path& root) {
    static std::atomic<uint32_t> counter{0};
    const auto stamp =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const fs::path dir = root / (std::to_string(getpid()) + "-" + std::to_string(stamp) + "-" + std::to_string(counter++));
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) throw std::runtime_error(io_error(dir, ec));
    return dir;
}

}  // namespace

void check_url(const std::string& url) {
    if (url.rfind("https://", 0) == 0) return;
    if (url.rfind("http://", 0) == 0) {
        const std::string rest = url.substr(7);
        std::string host = rest.substr(0, rest.find_first_of("/?#"));
        // Without a port, if what follows the last ':' is all digits.
        size_t colon = host.rfind(':');
        if (colon != std::string::npos &&
            host.find_first_not_of("0123456789", colon + 1) == std::string::npos)
            host = host.substr(0, colon);
        if (host == "localhost" || host == "127.0.0.1" || host == "[::1]") return;
        throw std::runtime_error(url + ": packs must be downloaded over https");
    }
    throw std::runtime_error(url + ": not an http(s) link");
}

std::optional<fs::path> staging_root() {
    auto d = profiles::data_dir();
    if (!d) return std::nullopt;
    return *d / "staging";
}

fs::path new_staging_dir() {
    auto root = staging_root();
    if (!root) throw std::runtime_error("cannot work out the launcher data folder");
    return new_staging_dir_in(*root);
}

void clear_staging(const std::optional<fs::path>& profiles_dir) {
    std::error_code ec;
    if (auto dir = staging_root()) fs::remove_all(*dir, ec);
    if (profiles_dir) fs::remove_all(pack_staging_root(*profiles_dir), ec);
}

}  // namespace sf::launch::packs
