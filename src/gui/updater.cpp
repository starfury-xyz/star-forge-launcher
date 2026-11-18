#include "gui/updater.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include <monocypher-ed25519.h>
#include <monocypher.h>

#include "crypto/base64.hpp"
#include "gui/internal.hpp"
#include "launch/net.hpp"
#include "launch/packs.hpp"
#include "pack/pack.hpp"

namespace sf::gui::updater {

namespace {

using nlohmann::json;

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

std::vector<uint8_t> unbase64(const std::string& text, const char* what) {
    auto bytes = crypto::base64_decode(text);
    if (!bytes) fail(std::string(what) + " is not valid base64");
    return *bytes;
}

// The lines of a minisign file, without line ends.
std::vector<std::string> lines_of(const std::vector<uint8_t>& bytes) {
    std::vector<std::string> lines;
    std::string text(bytes.begin(), bytes.end()), line;
    for (size_t at = 0; at <= text.size();) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        line = text.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        at = end + 1;
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    return lines;
}

bool starts_with(const std::string& s, const std::string& prefix) { return s.rfind(prefix, 0) == 0; }

// The parts of "1.2.3", "v1.2.3" or either with a -pre suffix (tauri-plugin-updater takes a v).
struct Version {
    std::tuple<uint64_t, uint64_t, uint64_t> core;
    std::vector<std::string> pre;  // dot-separated pre-release identifiers; empty for a release
};

std::optional<Version> parse(const std::string& text) {
    std::string v = !text.empty() && text[0] == 'v' ? text.substr(1) : text;
    v = v.substr(0, v.find('+'));
    auto core = pack::parse_version(v);
    if (!core) return std::nullopt;
    Version out{*core, {}};
    const size_t dash = v.find('-');
    if (dash == std::string::npos) return out;
    const std::string pre = v.substr(dash + 1);
    for (size_t at = 0; at <= pre.size();) {
        size_t end = pre.find('.', at);
        if (end == std::string::npos) end = pre.size();
        std::string id = pre.substr(at, end - at);
        if (id.empty()) return std::nullopt;
        out.pre.push_back(id);
        at = end + 1;
    }
    return out;
}

bool numeric(const std::string& s) { return !s.empty() && s.size() <= 18 && s.find_first_not_of("0123456789") == std::string::npos; }

// semver.org rule 11 for two pre-release identifiers.
int compare_ids(const std::string& a, const std::string& b) {
    const bool na = numeric(a), nb = numeric(b);
    if (na && nb) {
        const auto x = std::stoull(a), y = std::stoull(b);
        return x < y ? -1 : x > y;
    }
    if (na != nb) return na ? -1 : 1;
    return a.compare(b) < 0 ? -1 : a.compare(b) > 0;
}

}  // namespace

int compare_versions(const std::string& a, const std::string& b) {
    const auto x = parse(a), y = parse(b);
    if (!x) fail("version `" + a + "` is not x.y.z");
    if (!y) fail("version `" + b + "` is not x.y.z");
    if (x->core != y->core) return x->core < y->core ? -1 : 1;
    if (x->pre.empty() || y->pre.empty()) return x->pre.empty() == y->pre.empty() ? 0 : (x->pre.empty() ? 1 : -1);
    for (size_t i = 0; i < x->pre.size() && i < y->pre.size(); ++i)
        if (int c = compare_ids(x->pre[i], y->pre[i])) return c;
    return x->pre.size() < y->pre.size() ? -1 : x->pre.size() > y->pre.size();
}

std::optional<std::string> latest_in(const json& releases) {
    if (!releases.is_array()) fail("the release list is not a list");
    std::optional<std::string> best_version, best_url;
    for (const json& r : releases) {
        if (!r.is_object() || r.value("draft", false)) continue;
        const json tag = r.value("tag_name", json());
        const json assets = r.value("assets", json());
        if (!tag.is_string() || !assets.is_array() || !parse(tag.get<std::string>())) continue;
        for (const json& a : assets) {
            if (!a.is_object() || a.value("name", json()) != "latest.json") continue;
            const json url = a.value("browser_download_url", json());
            if (!url.is_string() || !starts_with(url.get<std::string>(), "https://")) continue;
            if (!best_version || compare_versions(tag.get<std::string>(), *best_version) > 0) {
                best_version = tag.get<std::string>();
                best_url = url.get<std::string>();
            }
        }
    }
    return best_url;
}

std::optional<Release> newer(const json& latest, const std::string& current, const std::vector<std::string>& targets) {
    if (!latest.is_object() || !latest.contains("version") || !latest["version"].is_string())
        fail("latest.json has no version");
    Release r;
    r.version = latest["version"].get<std::string>();
    if (!parse(r.version)) fail("latest.json's version `" + r.version + "` is not x.y.z");
    if (!parse(current)) fail("this launcher's version `" + current + "` is not x.y.z");
    // In semver order, as this launcher may be a pre-release itself (0.3.1-rc.1 < 0.3.1).
    if (compare_versions(r.version, current) <= 0) return std::nullopt;
    if (!r.version.empty() && r.version[0] == 'v') r.version.erase(0, 1);
    const json platforms = latest.value("platforms", json::object());
    if (!platforms.is_object()) fail("latest.json has no platforms");
    for (const auto& t : targets) {
        if (!platforms.contains(t)) continue;
        const json& p = platforms[t];
        if (!p.is_object() || !p.contains("url") || !p["url"].is_string() || !p.contains("signature") ||
            !p["signature"].is_string())
            fail("latest.json's `" + t + "` needs a url and a signature");
        r.url = p["url"];
        r.signature = p["signature"];
        if (latest.contains("page") && latest["page"].is_string() && starts_with(latest["page"].get<std::string>(), "https://"))
            r.page = latest["page"].get<std::string>();
        return r;
    }
    fail("this platform (" + (targets.empty() ? std::string("?") : targets.back()) + ") is not in latest.json");
}

void verify(const std::string& pubkey, const std::string& signature, const fs::path& file) {
    // The public key: "untrusted comment: ..." then base64 of "Ed", the key id and the key.
    const auto key_lines = lines_of(unbase64(pubkey, "the update public key"));
    if (key_lines.size() < 2 || !starts_with(key_lines[0], "untrusted comment: ")) fail("the update public key is malformed");
    const auto key = unbase64(key_lines[1], "the update public key");
    if (key.size() != 42 || key[0] != 'E' || key[1] != 'd') fail("the update public key is malformed");

    // The signature: untrusted comment, base64 of the algorithm, key id and signature, the
    // trusted comment, and base64 of the global signature.
    const auto sig_lines = lines_of(unbase64(signature, "the update signature"));
    if (sig_lines.size() < 4 || !starts_with(sig_lines[0], "untrusted comment: ") ||
        !starts_with(sig_lines[2], "trusted comment: "))
        fail("the update signature is malformed");
    const auto sig = unbase64(sig_lines[1], "the update signature");
    const auto global = unbase64(sig_lines[3], "the update signature");
    if (sig.size() != 74 || global.size() != 64 || sig[0] != 'E' || (sig[1] != 'd' && sig[1] != 'D'))
        fail("the update signature is malformed");
    if (std::memcmp(sig.data() + 2, key.data() + 2, 8) != 0) fail("the update was signed with a different key");
    const uint8_t* pk = key.data() + 10;
    const uint8_t* sig64 = sig.data() + 10;

    std::ifstream in(file, std::ios::binary);
    if (!in) fail("cannot read the downloaded update");
    std::array<char, 1 << 16> buf;
    bool ok;
    if (sig[1] == 'D') {
        // Prehashed: the signature is of the file's BLAKE2b-512 hash.
        crypto_blake2b_ctx ctx;
        crypto_blake2b_init(&ctx, 64);
        while (in.read(buf.data(), buf.size()) || in.gcount() > 0)
            crypto_blake2b_update(&ctx, reinterpret_cast<const uint8_t*>(buf.data()), static_cast<size_t>(in.gcount()));
        uint8_t hash[64];
        crypto_blake2b_final(&ctx, hash);
        ok = crypto_ed25519_check(sig64, pk, hash, sizeof hash) == 0;
    } else {
        std::string data;
        while (in.read(buf.data(), buf.size()) || in.gcount() > 0) data.append(buf.data(), static_cast<size_t>(in.gcount()));
        ok = crypto_ed25519_check(sig64, pk, reinterpret_cast<const uint8_t*>(data.data()), data.size()) == 0;
    }
    if (in.bad()) fail("cannot read the downloaded update");
    if (!ok) fail("the update's signature does not match: it may be damaged or not from STAR FORGE");

    // The global signature covers the signature and the trusted comment, so neither can be swapped.
    std::string signed_part(reinterpret_cast<const char*>(sig64), 64);
    signed_part += sig_lines[2].substr(std::strlen("trusted comment: "));
    if (crypto_ed25519_check(global.data(), pk, reinterpret_cast<const uint8_t*>(signed_part.data()), signed_part.size()) != 0)
        fail("the update's trusted comment does not match its signature");
}

void replace_file(const fs::path& target, const fs::path& package) {
    std::error_code ec;
    const auto status = fs::status(target, ec);
    if (ec || !fs::is_regular_file(status)) fail("cannot update " + target.u8string() + ": it is not a file");
    fs::path next = target;
    next += ".update";
    fs::copy_file(package, next, fs::copy_options::overwrite_existing, ec);
    if (!ec) fs::permissions(next, status.permissions(), fs::perm_options::replace, ec);
    if (!ec) fs::rename(next, target, ec);
    if (ec) {
        std::error_code ignored;
        fs::remove(next, ignored);
        fail("cannot update " + target.u8string() + ": " + launch::io_msg(ec));
    }
}

}  // namespace sf::gui::updater

namespace sf::gui {

namespace {

constexpr uint64_t kLatestLimit = 1 << 20;            // latest.json
constexpr uint64_t kReleaseListLimit = 8 << 20;       // GitHub's release list (pre-release updates)
constexpr uint64_t kPackageLimit = uint64_t(1) << 30;  // the package itself

}  // namespace

json Backend::check_launcher_update() {
    if (updater.unsupported) throw std::runtime_error(*updater.unsupported);
    if (updater.endpoint.empty()) throw std::runtime_error("Development builds do not update themselves.");
    std::optional<updater::Release> found;
    try {
        const fs::path dir = launch::packs::new_staging_dir();
        auto get = [&](const std::string& url, const char* name, uint64_t limit) {
            const fs::path file = dir / name;
            launch::net::fetch(url, file, limit);
            std::ifstream in(file, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        };
        // Testers get the newest release, pre-release or not; everyone else the newest release.
        std::string endpoint = updater.endpoint;
        if (prefs().prerelease_updates && !updater.prerelease_endpoint.empty()) {
            const json list = json::parse(get(updater.prerelease_endpoint, "releases.json", kReleaseListLimit), nullptr, false);
            if (list.is_discarded()) throw std::runtime_error("the release list is not valid JSON");
            if (auto url = updater::latest_in(list)) endpoint = *url;
        }
        const std::string text = get(endpoint, "latest.json", kLatestLimit);
        std::error_code ec;
        fs::remove_all(dir, ec);
        // An empty answer (HTTP 204) means there is nothing newer, as with tauri-plugin-updater.
        if (text.find_first_not_of(" \t\r\n") != std::string::npos) {
            const json latest = json::parse(text, nullptr, false);
            if (latest.is_discarded()) throw std::runtime_error("latest.json is not valid JSON");
            found = updater::newer(latest, updater.current, updater.targets);
        }
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("could not check for launcher updates: ") + e.what());
    }
    json out = nullptr;
    if (found)
        out = {{"version", found->version}, {"current", updater.current}, {"page", found->page ? json(*found->page) : json(nullptr)}};
    std::lock_guard<std::mutex> lock(updates_->mu);
    updates_->found = std::move(found);
    return out;
}

// Downloads and installs the update found by the last check, then restarts. Progress goes to the
// interface as `launcher-update-progress` events [downloaded, total], then
// `launcher-update-downloaded` before the package's signature is checked and it installs.
void Backend::install_launcher_update() {
    if (busy()) throw std::runtime_error("Close the game before updating the launcher.");
    std::optional<updater::Release> release;
    {
        std::lock_guard<std::mutex> lock(updates_->mu);
        release.swap(updates_->found);
    }
    if (!release) throw std::runtime_error("Check for updates first.");
    try {
        const fs::path package = launch::packs::new_staging_dir() / launch::net::file_name(release->url);
        auto last = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        launch::net::fetch(release->url, package, kPackageLimit, nullptr,
                           [&](uint64_t got, std::optional<uint64_t> total) -> std::optional<std::string> {
                               // A few a second is plenty for the progress bar.
                               const auto now = std::chrono::steady_clock::now();
                               if (now - last >= std::chrono::milliseconds(100) || (total && got == *total)) {
                                   last = now;
                                   host_.emit("launcher-update-progress", json::array({got, total ? json(*total) : json(nullptr)}));
                               }
                               return std::nullopt;
                           });
        host_.emit("launcher-update-downloaded", nullptr);
        updater::verify(updater.pubkey, release->signature, package);
        host_.install_update(package);
    } catch (const std::exception& e) {
        // Kept so the player can retry without checking again.
        {
            std::lock_guard<std::mutex> lock(updates_->mu);
            updates_->found = release;
        }
        throw std::runtime_error(std::string("could not install the launcher update: ") + e.what());
    }
    notify("Updated to " + release->version + ", restarting…", NotificationLevel::Info);
    host_.restart();
}

void Backend::open_launcher_release() {
    std::optional<std::string> page;
    {
        std::lock_guard<std::mutex> lock(updates_->mu);
        if (updates_->found) page = updates_->found->page;
    }
    if (!page) throw std::runtime_error("No release page for this update.");
    host_.open(*page);
}

}  // namespace sf::gui
