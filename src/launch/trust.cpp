#include "launch/trust.hpp"

#include <map>
#include <stdexcept>

#include "launch/profiles.hpp"
#include "pack/serde.hpp"

namespace sf::launch::trust {

const std::vector<KnownPublisher> kKnown = {};

namespace {

using namespace pack::serde;

const std::vector<KnownPublisher>* g_known = &kKnown;

// trust.json: {"packs": {"<id>": {"key": "ed25519:..."}}}
using Store = std::map<std::string, std::string>;  // pack id -> key

std::optional<fs::path> file() {
    auto d = profiles::data_dir();
    if (!d) return std::nullopt;
    return *d / "trust.json";
}

Store load() {
    auto path = file();
    if (!path) return {};
    std::error_code ec;
    auto text = read_text(*path, &ec);
    if (!text) {
        if (not_found(ec)) return {};
        throw std::runtime_error(io_error(*path, ec));
    }
    return parse<Store>(*text, ustr(*path).c_str(), [](const json& j) {
        Obj o(j, "Store", {"packs"});
        Store s;
        if (const json* packs = o.get("packs")) {
            if (!packs->is_object()) invalid(*packs, "a map");
            for (auto it = packs->begin(); it != packs->end(); ++it) {
                Obj pin(*it, "Pin", {"key"});
                s[it.key()] = str(pin.req("key"));
            }
        }
        return s;
    });
}

void save(const Store& store) {
    auto path = file();
    if (!path) throw std::runtime_error("cannot work out the launcher data folder");
    std::error_code ec;
    fs::create_directories(path->parent_path(), ec);
    if (ec) throw std::runtime_error(io_error(path->parent_path(), ec));
    ojson j;
    j["packs"] = ojson::object();
    for (const auto& [id, key] : store) j["packs"][id]["key"] = key;
    profiles::write_atomic(*path, j.dump(2) + "\n");
}

std::optional<std::pair<std::string, signing::PublicKey>> known_for(const std::string& id) {
    for (const KnownPublisher& k : *g_known) {
        const std::string prefix = k.id_prefix;
        bool under = id == prefix ||
                     (id.size() > prefix.size() && id.compare(0, prefix.size(), prefix) == 0 &&
                      (id[prefix.size()] == '.' || id[prefix.size()] == '-'));
        if (under) return std::make_pair(std::string(k.name), signing::PublicKey::parse(k.key));
    }
    return std::nullopt;
}

}  // namespace

std::optional<signing::PublicKey> pinned(const std::string& id) {
    Store s = load();
    auto it = s.find(id);
    if (it == s.end()) return std::nullopt;
    return signing::PublicKey::parse(it->second);
}

void pin(const std::string& id, const signing::PublicKey& key) {
    Store s = load();
    s[id] = key.to_string();
    save(s);
}

void unpin(const std::string& id) {
    Store s = load();
    if (s.erase(id)) save(s);
}

Trust check(const std::string& id, const signing::Signature& signature) {
    auto pin = pinned(id);
    auto known = known_for(id);
    if (!signature) {
        if (pin || known)
            throw std::runtime_error(id + " is normally signed by its publisher, but this copy isn't; not trusting it");
        return {};
    }
    const signing::PublicKey& key = *signature;
    if (known) {
        if (key != known->second)
            throw std::runtime_error(id + " belongs to " + known->first + ", but this copy is signed by someone else (key " +
                                     key.fingerprint() + ")");
        return {Trust::Known, key, known->first};
    }
    if (pin) {
        if (*pin == key) return {Trust::Pinned, key, ""};
        throw std::runtime_error(id + " was signed with key " + pin->fingerprint() +
                                 " when you installed it, but this copy is signed with " + key.fingerprint() +
                                 ". If its server really changed keys, remove the profile and install the pack again.");
    }
    return {Trust::NewKey, key, ""};
}

void set_known_for_test(const std::vector<KnownPublisher>* known) { g_known = known ? known : &kKnown; }

}  // namespace sf::launch::trust
