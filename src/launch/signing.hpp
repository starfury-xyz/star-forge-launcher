#pragma once

// Port of sc-launch's signing.rs: pack signatures.
//
// A signed pack carries manifest.json, listing every file of the pack (pack.json, resources/,
// files/, patches/) with its size and SHA-256, and manifest.sig, an ed25519 signature over the
// manifest's exact bytes. The launcher checks both when a pack is installed, pins the key to the
// pack id (see trust.hpp), and checks the code-bearing files again before every launch, so a
// pack can't be swapped or edited behind the player's back.
//
// ed25519 is RFC 8032's, from Monocypher (third_party/monocypher). Portable.

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "launch/paths.hpp"
#include "pack/pack.hpp"

namespace sf::launch::signing {

inline constexpr uint32_t kManifestFormat = 1;  // newest manifest format this build understands

// A pack publisher's public key, written ed25519:<base64>.
class PublicKey {
public:
    // Throws std::runtime_error if `text` isn't one (or isn't a point on the curve).
    static PublicKey parse(const std::string& text);
    // Short form for people to compare, e.g. 3f2a-91c0-77de-0b14.
    std::string fingerprint() const;
    std::string to_string() const;  // ed25519:<base64>
    const std::array<uint8_t, 32>& bytes() const { return bytes_; }
    bool operator==(const PublicKey& o) const { return bytes_ == o.bytes_; }
    bool operator!=(const PublicKey& o) const { return !(*this == o); }

private:
    friend class SecretKey;
    std::array<uint8_t, 32> bytes_{};
};

// A publisher's signing key. Kept by the server team, never in a pack.
class SecretKey {
public:
    static SecretKey generate();
    // Throws std::runtime_error if `text` isn't one.
    static SecretKey parse(const std::string& text);
    ~SecretKey();
    SecretKey(const SecretKey&) = delete;
    SecretKey& operator=(const SecretKey&) = delete;
    SecretKey(SecretKey&& o) noexcept;

    std::string to_text() const;  // starforge-pack-secret-v1:<base64 seed>, plus a newline
    const PublicKey& public_key() const { return public_; }
    std::array<uint8_t, 64> sign(const uint8_t* data, size_t len) const;

private:
    explicit SecretKey(const std::array<uint8_t, 32>& seed);
    std::array<uint8_t, 32> seed_{};
    std::array<uint8_t, 64> secret_{};  // Monocypher's form: the seed, then the public key
    PublicKey public_;
};

// One file in the manifest.
struct Entry {
    uint64_t size = 0;
    std::string sha256;
    bool operator==(const Entry& o) const { return size == o.size && sha256 == o.sha256; }
};

// manifest.json.
struct Manifest {
    uint32_t format = kManifestFormat;
    std::string id, version;  // so a signature can't be moved to another pack
    std::string key;          // the key that signed it
    std::map<std::string, Entry> files;  // path (with / separators) -> entry, for every pack file

    // Strict, like serde; throws pack::Error with the reason (no file name in front).
    static Manifest from_json(const std::string& text);
    std::string to_json() const;  // pretty, newline at the end, the way the Rust writes it
};

// What a pack's signature says, once checked: nullopt is unsigned.
using Signature = std::optional<PublicKey>;

// How much of the pack to hash.
enum class Depth {
    Full,    // every file
    Launch,  // all but files/, which is only checked for size: what can change how the game runs
             // (pack.json, patches, plugins) is always hashed
};

// Every file that belongs to the pack, relative, with / separators, sorted. Dot files are left
// out, as when a pack is unpacked, and so are the files the launcher writes itself. A pack whose
// game files come from a file list (files_from) has none of its own: that list is signed
// separately. Throws std::runtime_error.
std::vector<std::string> pack_files(const fs::path& root, const pack::Pack& pack);

// SHA-256 of a file, lower-case hex; its size into `size` if given. Throws std::runtime_error
// ("path: message (os error N)").
std::string sha256_file(const fs::path& path, uint64_t* size = nullptr);
// The same, without throwing: nullopt and `ec` set if the file can't be read.
std::optional<std::string> sha256_file(const fs::path& path, uint64_t* size, std::error_code& ec);

// A detached signature over `bytes`, as written to a .sig file.
std::string sign_bytes(const SecretKey& key, const std::string& bytes);
// Checks a detached signature (the text of a .sig file) over `bytes`. Throws std::runtime_error
// ("the signature is damaged" / "the signature does not match").
void verify_bytes(const PublicKey& key, const std::string& bytes, const std::string& sig_text);

// Writes manifest.json and manifest.sig for the pack in `root`.
Manifest sign(const fs::path& root, const SecretKey& key);
// Checks the pack in `root` against its manifest. A pack without one is unsigned; a manifest that
// doesn't verify, or files that don't match it, throw std::runtime_error.
Signature verify(const fs::path& root, Depth depth);

}  // namespace sf::launch::signing
