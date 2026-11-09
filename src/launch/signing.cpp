#include "launch/signing.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "crypto/base64.hpp"
#include "crypto/random.hpp"
#include "crypto/sha256.hpp"
#include "launch/profiles.hpp"
#include "pack/serde.hpp"

#include <monocypher-ed25519.h>
#include <monocypher.h>

namespace sf::launch::signing {
namespace {

using namespace pack::serde;

const std::string kKeyPrefix = "ed25519:";
const std::string kSecretPrefix = "starforge-pack-secret-v1:";

std::string trim(const std::string& s) {
    const char* ws = " \t\r\n\v\f";
    size_t a = s.find_first_not_of(ws);
    return a == std::string::npos ? "" : s.substr(a, s.find_last_not_of(ws) - a + 1);
}

bool starts_with(const std::string& s, const std::string& prefix) { return s.compare(0, prefix.size(), prefix) == 0; }

// Whether 32 bytes decode to a point on the curve, as ed25519-dalek's VerifyingKey::from_bytes
// checks. Monocypher has no call for just that, but its signature equation [S]B = R + [h]A with
// R = the neutral point and S = h = 0 holds for every A it can decode, and it refuses to go on
// with one it can't.
bool on_curve(const std::array<uint8_t, 32>& key) {
    uint8_t sig[64] = {1};  // R: the neutral point (y = 1); S: 0
    const uint8_t h[32] = {};
    return crypto_eddsa_check_equation(sig, key.data(), h) == 0;
}

// Rust's Entry/Manifest field order, for writing.
ojson entry_json(const Entry& e) {
    ojson j;
    j["size"] = e.size;
    j["sha256"] = e.sha256;
    return j;
}

Manifest read_manifest(const json& j) {
    Obj o(j, "Manifest", {"format", "id", "version", "key", "files"});
    Manifest m;
    m.format = u32(o.req("format"));
    m.id = str(o.req("id"));
    m.version = str(o.req("version"));
    m.key = str(o.req("key"));
    const json& files = o.req("files");
    if (!files.is_object()) invalid(files, "a map");
    for (auto it = files.begin(); it != files.end(); ++it) {
        Obj e(*it, "Entry", {"size", "sha256"});
        m.files[it.key()] = Entry{u64(e.req("size")), str(e.req("sha256"))};
    }
    return m;
}

void walk(const fs::path& dir, const std::string& rel, std::vector<std::string>& out) {
    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    if (ec) {
        if (not_found(ec)) return;
        throw std::runtime_error(io_error(dir, ec));
    }
    for (; it != fs::directory_iterator(); it.increment(ec)) {
        const std::string name = ustr(it->path().filename());
        if (name[0] == '.') continue;
        const std::string child = rel + "/" + name;
        // Not following links, like Rust's DirEntry::file_type: a link is neither.
        const fs::file_status st = it->symlink_status(ec);
        if (fs::is_directory(st)) walk(it->path(), child, out);
        else if (fs::is_regular_file(st)) out.push_back(child);
        else throw std::runtime_error(child + ": not a regular file");
    }
}

}  // namespace

// --- keys ---

PublicKey PublicKey::parse(const std::string& text) {
    const std::string t = trim(text);
    if (!starts_with(t, kKeyPrefix)) throw std::runtime_error(debug_str(text) + ": expected " + kKeyPrefix + "<base64>");
    auto bytes = crypto::base64_decode(t.substr(kKeyPrefix.size()));
    if (!bytes || bytes->size() != 32) throw std::runtime_error(debug_str(text) + ": not a 32-byte ed25519 key");
    PublicKey k;
    std::copy(bytes->begin(), bytes->end(), k.bytes_.begin());
    if (!on_curve(k.bytes_)) throw std::runtime_error(debug_str(text) + ": signature error: Cannot decompress Edwards point");
    return k;
}

std::string PublicKey::fingerprint() const {
    crypto::Sha256 h;
    h.update(bytes_.data(), bytes_.size());
    const std::string hex = crypto::to_hex(h.digest().data(), 8);
    return hex.substr(0, 4) + "-" + hex.substr(4, 4) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4);
}

std::string PublicKey::to_string() const { return kKeyPrefix + crypto::base64_encode(bytes_.data(), bytes_.size()); }

SecretKey::SecretKey(const std::array<uint8_t, 32>& seed) : seed_(seed) {
    uint8_t scratch[32];
    std::memcpy(scratch, seed.data(), 32);  // Monocypher wipes the seed it's given
    crypto_ed25519_key_pair(secret_.data(), public_.bytes_.data(), scratch);
}

SecretKey::SecretKey(SecretKey&& o) noexcept : seed_(o.seed_), secret_(o.secret_), public_(o.public_) {
    crypto_wipe(o.seed_.data(), o.seed_.size());
    crypto_wipe(o.secret_.data(), o.secret_.size());
}

SecretKey::~SecretKey() {
    crypto_wipe(seed_.data(), seed_.size());
    crypto_wipe(secret_.data(), secret_.size());
}

SecretKey SecretKey::generate() {
    std::array<uint8_t, 32> seed;
    crypto::random_bytes(seed.data(), seed.size());
    SecretKey key(seed);
    crypto_wipe(seed.data(), seed.size());
    return key;
}

SecretKey SecretKey::parse(const std::string& text) {
    const std::string t = trim(text);
    if (!starts_with(t, kSecretPrefix)) throw std::runtime_error("not a STAR FORGE pack signing key");
    auto bytes = crypto::base64_decode(t.substr(kSecretPrefix.size()));
    if (!bytes || bytes->size() != 32) throw std::runtime_error("damaged pack signing key");
    std::array<uint8_t, 32> seed;
    std::copy(bytes->begin(), bytes->end(), seed.begin());
    crypto_wipe(bytes->data(), bytes->size());
    SecretKey key(seed);
    crypto_wipe(seed.data(), seed.size());
    return key;
}

std::string SecretKey::to_text() const { return kSecretPrefix + crypto::base64_encode(seed_.data(), seed_.size()) + "\n"; }

std::array<uint8_t, 64> SecretKey::sign(const uint8_t* data, size_t len) const {
    std::array<uint8_t, 64> sig;
    crypto_ed25519_sign(sig.data(), secret_.data(), data, len);
    return sig;
}

// --- manifest ---

Manifest Manifest::from_json(const std::string& text) {
    try {
        return read_manifest(json::parse(text));
    } catch (const json::parse_error& e) {
        std::string m = e.what();
        size_t at = m.find("] ");
        throw pack::Error(at == std::string::npos ? m : m.substr(at + 2));
    }
}

std::string Manifest::to_json() const {
    ojson j;
    j["format"] = format;
    j["id"] = id;
    j["version"] = version;
    j["key"] = key;
    j["files"] = ojson::object();
    for (const auto& [path, e] : files) j["files"][path] = entry_json(e);
    return j.dump(2) + "\n";
}

// --- files ---

std::vector<std::string> pack_files(const fs::path& root, const pack::Pack& pack) {
    std::vector<std::string> out;
    std::error_code ec;
    if (fs::is_regular_file(root / pack::kPackFile, ec)) out.push_back(pack::kPackFile);
    for (const char* dir : {pack::kResourcesDir, pack::kFilesDir, pack::kPatchesDir}) {
        if (std::strcmp(dir, pack::kFilesDir) == 0 && pack.files_from) continue;
        walk(root / dir, dir, out);
    }
    const std::string files_prefix = std::string(pack::kFilesDir) + "/";
    out.erase(std::remove_if(out.begin(), out.end(),
                             [&](const std::string& p) {
                                 return starts_with(p, files_prefix) &&
                                        std::find(pack::kGeneratedFiles.begin(), pack::kGeneratedFiles.end(),
                                                  p.substr(files_prefix.size())) != pack::kGeneratedFiles.end();
                             }),
              out.end());
    std::sort(out.begin(), out.end());
    return out;
}

std::optional<std::string> sha256_file(const fs::path& path, uint64_t* size, std::error_code& ec) {
#ifdef _WIN32
    FILE* f = _wfopen(path.wstring().c_str(), L"rb");
#else
    FILE* f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) {
        ec = std::error_code(errno, std::generic_category());
        return std::nullopt;
    }
    crypto::Sha256 h;
    uint64_t total = 0;
    static thread_local char buf[64 * 1024];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
        h.update(buf, n);
        total += n;
    }
    const bool failed = std::ferror(f);
    const int err = errno;
    std::fclose(f);
    if (failed) {
        ec = std::error_code(err, std::generic_category());
        return std::nullopt;
    }
    ec.clear();
    if (size) *size = total;
    return h.hex();
}

std::string sha256_file(const fs::path& path, uint64_t* size) {
    std::error_code ec;
    auto hex = sha256_file(path, size, ec);
    if (!hex) throw std::runtime_error(io_error(path, ec));
    return *hex;
}

// --- signatures ---

std::string sign_bytes(const SecretKey& key, const std::string& bytes) {
    auto sig = key.sign(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    return crypto::base64_encode(sig.data(), sig.size()) + "\n";
}

void verify_bytes(const PublicKey& key, const std::string& bytes, const std::string& sig_text) {
    auto sig = crypto::base64_decode(trim(sig_text));
    if (!sig || sig->size() != 64) throw std::runtime_error("the signature is damaged");
    if (crypto_ed25519_check(sig->data(), key.bytes().data(), reinterpret_cast<const uint8_t*>(bytes.data()),
                             bytes.size()) != 0)
        throw std::runtime_error("the signature does not match");
}

Manifest sign(const fs::path& root, const SecretKey& key) {
    pack::Pack pack = profiles::read_pack(root);
    pack.validate();
    Manifest m;
    m.id = pack.id;
    m.version = pack.version;
    m.key = key.public_key().to_string();
    for (const std::string& rel : pack_files(root, pack)) {
        Entry e;
        e.sha256 = sha256_file(root / upath(rel), &e.size);
        m.files[rel] = e;
    }
    const std::string bytes = m.to_json();
    profiles::write_atomic(root / pack::kManifestFile, bytes);
    profiles::write_atomic(root / pack::kSignatureFile, sign_bytes(key, bytes));
    return m;
}

Signature verify(const fs::path& root, Depth depth) {
    const std::string M = pack::kManifestFile, S = pack::kSignatureFile;
    const fs::path manifest_path = root / M, sig_path = root / S;
    std::error_code ec;
    const bool has_manifest = fs::is_regular_file(manifest_path, ec), has_sig = fs::is_regular_file(sig_path, ec);
    if (!has_manifest && !has_sig) return std::nullopt;
    if (has_manifest != has_sig) throw std::runtime_error("the pack has only one of " + M + " and " + S);

    auto bytes = read_text(manifest_path, &ec);
    if (!bytes) throw std::runtime_error(io_error(M, ec));
    Manifest manifest;
    try {
        manifest = Manifest::from_json(*bytes);
    } catch (const pack::Error& e) {
        throw std::runtime_error(M + ": " + e.what());
    }
    if (manifest.format == 0 || manifest.format > kManifestFormat)
        throw std::runtime_error(M + ": format " + std::to_string(manifest.format) +
                                 " is newer than this launcher understands; update it");
    PublicKey key;
    try {
        key = PublicKey::parse(manifest.key);
    } catch (const std::runtime_error& e) {
        throw std::runtime_error(M + ": key " + e.what());
    }
    auto sig_text = read_text(sig_path, &ec);
    if (!sig_text) throw std::runtime_error(io_error(S, ec));
    try {
        verify_bytes(key, *bytes, *sig_text);
    } catch (const std::runtime_error&) {
        throw std::runtime_error(S + " does not match " + M + ": the pack was changed after it was signed");
    }

    pack::Pack pack = profiles::read_pack(root);
    if (pack.id != manifest.id || pack.version != manifest.version)
        throw std::runtime_error(M + " was signed for " + manifest.id + " " + manifest.version + ", not " + pack.id + " " +
                                 pack.version);
    const std::vector<std::string> present = pack_files(root, pack);
    for (const std::string& p : present)
        if (!manifest.files.count(p)) throw std::runtime_error(p + " is not part of the signed pack");
    const std::string files_prefix = std::string(pack::kFilesDir) + "/";
    for (const auto& [rel, want] : manifest.files) {
        if (!std::binary_search(present.begin(), present.end(), rel))
            throw std::runtime_error(rel + " is missing from the signed pack");
        const fs::path path = root / upath(rel);
        bool ok;
        if (depth == Depth::Launch && starts_with(rel, files_prefix)) {
            uint64_t size = fs::file_size(path, ec);
            if (ec) throw std::runtime_error(io_error(rel, ec));
            ok = size == want.size;
        } else {
            Entry got;
            got.sha256 = sha256_file(path, &got.size);
            ok = got == want;
        }
        if (!ok) throw std::runtime_error(rel + " does not match the signed pack");
    }
    return key;
}

}  // namespace sf::launch::signing
