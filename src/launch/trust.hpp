#pragma once

// Port of sc-launch's trust.rs: which key may sign which pack.
//
// The first signed version of a pack the player installs pins its key to the pack id (trust on
// first use, kept in <data dir>/trust.json). Every later version must be signed by that same key,
// so a hijacked update address or a look-alike pack can't take over an installed profile.
// Removing the profile removes the pin.
//
// kKnown lists publishers the launcher already knows; a pack whose id falls under one of their
// prefixes must carry their key even on first install. Portable.

#include <optional>
#include <string>
#include <vector>

#include "launch/signing.hpp"

namespace sf::launch::trust {

// A publisher known to the launcher. Add one here (by pull request) once a server team publishes
// its key.
struct KnownPublisher {
    const char* name;
    const char* id_prefix;
    const char* key;
};
extern const std::vector<KnownPublisher> kKnown;

// The key pinned to a pack id, if any. Throws std::runtime_error (an unreadable trust.json).
std::optional<signing::PublicKey> pinned(const std::string& id);
void pin(const std::string& id, const signing::PublicKey& key);
void unpin(const std::string& id);

// How far a pack can be trusted, for the install review.
struct Trust {
    enum Kind {
        Unsigned,  // no signature: allowed, but the player is told
        NewKey,    // signed by a key seen for the first time; it gets pinned on install
        Pinned,    // signed by the key already pinned to this pack
        Known,     // signed by a publisher the launcher knows (`name`)
    } kind = Unsigned;
    std::optional<signing::PublicKey> key;  // set unless Unsigned
    std::string name;                       // Known: the publisher

    bool operator==(const Trust& o) const { return kind == o.kind && key == o.key && name == o.name; }
    bool operator!=(const Trust& o) const { return !(*this == o); }
};

// Decides whether a pack with this id and signature may be installed (or launched). Throws
// std::runtime_error saying why not.
Trust check(const std::string& id, const signing::Signature& signature);

// For tests: the publisher list check() uses (kKnown unless replaced).
void set_known_for_test(const std::vector<KnownPublisher>* known);

}  // namespace sf::launch::trust
