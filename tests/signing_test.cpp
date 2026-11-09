// Tests sf::crypto's base64 and sf::launch::signing (keys, detached signatures, signed packs).
// Groups marked "Rust:" port signing.rs's tests.
//
// Usage: signing_test [profiles folder]. With a folder of real installed profiles, also checks
// every signed one verifies (they were signed by the Rust tool), and that its manifest.json comes
// out byte for byte the same when read and written again. Only reads them.

#include "crypto/base64.hpp"
#include "launch/profiles.hpp"
#include "launch/signing.hpp"

#include "check.hpp"

namespace fs = std::filesystem;
using namespace sf::launch::signing;
using sf::crypto::base64_decode;
using sf::crypto::base64_encode;
using sf::launch::upath;
using test::check;
using test::contains;

namespace {

std::vector<uint8_t> unhex(const std::string& h) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < h.size(); i += 2) out.push_back(static_cast<uint8_t>(std::stoi(h.substr(i, 2), nullptr, 16)));
    return out;
}

std::string b64(const std::string& s) { return base64_encode(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }
std::string b64(const std::vector<uint8_t>& v) { return base64_encode(v.data(), v.size()); }

// What `f` threw, or "" if it didn't.
template <class F>
std::string error_of(F f) {
    try {
        f();
        return "";
    } catch (const std::exception& e) {
        return e.what();
    }
}

void base64() {
    const std::pair<const char*, const char*> rfc4648[] = {
        {"", ""}, {"f", "Zg=="}, {"fo", "Zm8="}, {"foo", "Zm9v"}, {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"}};
    bool enc = true, dec = true;
    for (auto [plain, coded] : rfc4648) {
        enc &= b64(std::string(plain)) == coded;
        auto d = base64_decode(coded);
        dec &= d && std::string(d->begin(), d->end()) == plain;
    }
    check(enc, "base64 encodes the RFC 4648 examples");
    check(dec, "base64 decodes them back");
    const std::string all = b64(unhex("00ff10fbef3e"));
    check(all == "AP8Q++8+", "base64 uses + and /: " + all);
    bool strict = true;
    for (const char* bad : {"Zg", "Zg=", "Zh==", "Zm9=", "Z===", "Zm9v\n", " Zm9v", "Zm=v", "Zg==Zg==", "Zm9-", "Zm9_"}) {
        if (base64_decode(bad)) {
            strict = false;
            check(false, std::string("base64 refuses ") + bad);
        }
    }
    check(strict, "base64 refuses missing padding, stray bits, whitespace, inner padding, url-safe letters");
}

// RFC 8032 section 7.1, tests 1 and 2.
void rfc8032() {
    struct Case {
        const char *seed, *pub, *msg, *sig;
    } cases[] = {
        {"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
         "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
         "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
        {"4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
         "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
         "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
    };
    int n = 0;
    for (const Case& c : cases) {
        ++n;
        const std::string tag = "RFC 8032 test " + std::to_string(n) + ": ";
        SecretKey key = SecretKey::parse("starforge-pack-secret-v1:" + b64(unhex(c.seed)));
        check(key.public_key().to_string() == "ed25519:" + b64(unhex(c.pub)), tag + "public key from the seed");
        auto m = unhex(c.msg);
        const std::string msg(m.begin(), m.end());
        const std::string sig = sign_bytes(key, msg);
        check(sig == b64(unhex(c.sig)) + "\n", tag + "signature");
        check(error_of([&] { verify_bytes(PublicKey::parse("ed25519:" + b64(unhex(c.pub))), msg, sig); }).empty(),
              tag + "verifies");
    }
}

// Rust: keys_round_trip.
void keys() {
    SecretKey key = SecretKey::generate();
    SecretKey again = SecretKey::parse(key.to_text());
    check(again.public_key() == key.public_key(), "a secret key reads back as the same key");
    check(PublicKey::parse(key.public_key().to_string()) == key.public_key(), "a public key reads back");
    check(key.public_key().fingerprint().size() == 19, "fingerprint is 19 characters");
    check(contains(error_of([] { PublicKey::parse("ed25519:abc"); }), "\"ed25519:abc\": not a 32-byte ed25519 key"),
          "a short key is refused: " + error_of([] { PublicKey::parse("ed25519:abc"); }));
    check(error_of([] { PublicKey::parse("hello"); }) == "\"hello\": expected ed25519:<base64>", "a key without the prefix");
    check(error_of([] { SecretKey::parse("hello"); }) == "not a STAR FORGE pack signing key", "not a secret key");
    check(error_of([] { SecretKey::parse("starforge-pack-secret-v1:abc"); }) == "damaged pack signing key", "a damaged secret key");
    check(SecretKey::generate().public_key() != key.public_key(), "two generated keys differ");
    check(key.to_text().back() == '\n' && key.to_text().rfind("starforge-pack-secret-v1:", 0) == 0, "secret key text");
    check(error_of([&] { PublicKey::parse("  " + key.public_key().to_string() + "\n"); }).empty(), "whitespace around a key is fine");

    // RFC 8032 test 1's key: fingerprint = first 8 bytes of its SHA-256, in fours.
    const std::string rfc = "ed25519:" + b64(unhex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"));
    check(PublicKey::parse(rfc).fingerprint() == "21fe-31df-a154-a261", "fingerprint: " + PublicKey::parse(rfc).fingerprint());

    // Not every 32 bytes is a point on the curve; those are refused as the Rust does.
    int refused = 0, accepted = 0;
    for (uint8_t y = 0; y < 32; ++y) {
        std::vector<uint8_t> b(32, 0);
        b[0] = y;
        std::string e = error_of([&] { PublicKey::parse("ed25519:" + b64(b)); });
        if (e.empty()) ++accepted;
        else if (contains(e, "Cannot decompress Edwards point")) ++refused;
    }
    check(refused > 0 && accepted > 0, "points off the curve are refused (" + std::to_string(refused) + " of 32 small ones)");
}

void detached() {
    SecretKey key = SecretKey::generate();
    const std::string sig = sign_bytes(key, "list bytes");
    check(sig.size() == 89 && sig.back() == '\n', "a signature is 88 base64 characters and a newline");
    check(error_of([&] { verify_bytes(key.public_key(), "list bytes", sig); }).empty(), "it verifies");
    check(error_of([&] { verify_bytes(key.public_key(), "list bytez", sig); }) == "the signature does not match", "other bytes don't");
    check(error_of([&] { verify_bytes(SecretKey::generate().public_key(), "list bytes", sig); }) == "the signature does not match",
          "another key doesn't");
    check(error_of([&] { verify_bytes(key.public_key(), "list bytes", "abc"); }) == "the signature is damaged", "a damaged signature");
    check(error_of([&] { verify_bytes(key.public_key(), "list bytes", sig.substr(0, 84) + "\n"); }) == "the signature is damaged",
          "a cut signature is damaged");
}

fs::path pack_dir(const std::string& name) {
    const fs::path dir = test::fresh_dir(fs::temp_directory_path() / ("sf-sign-" + name));
    test::write_file(dir / "pack.json", R"({"format":1,"id":"org.example.test","name":"Test","version":"1.0.0"})");
    test::write_file(dir / "files/data/a.cfg", "one");
    test::write_file(dir / "patches/game.exe.patchset.toml", "x");
    return dir;
}

// Rust: signs_and_detects_changes, then more.
void packs() {
    const fs::path dir = pack_dir("verify");
    check(!verify(dir, Depth::Full), "a pack without a manifest is unsigned");
    SecretKey key = SecretKey::generate();
    Manifest manifest = sign(dir, key);
    check(manifest.files.size() == 3, "the manifest lists 3 files");
    check(verify(dir, Depth::Full) == Signature(key.public_key()), "signed by the key");

    // Same size, different bytes: only a full check sees it in files/.
    test::write_file(dir / "files/data/a.cfg", "two");
    check(error_of([&] { verify(dir, Depth::Launch); }).empty(), "launch check: files/ by size only");
    check(error_of([&] { verify(dir, Depth::Full); }) == "files/data/a.cfg does not match the signed pack", "full check sees it");
    test::write_file(dir / "files/data/a.cfg", "one");

    // Patches are always hashed.
    test::write_file(dir / "patches/game.exe.patchset.toml", "y");
    check(!error_of([&] { verify(dir, Depth::Launch); }).empty(), "patches are hashed at launch");
    test::write_file(dir / "patches/game.exe.patchset.toml", "x");

    // The address file the launcher writes doesn't count.
    test::write_file(dir / "files/data/address.cfg", "set net_masterServerAddress \"a:1\"");
    check(error_of([&] { verify(dir, Depth::Full); }).empty(), "address.cfg doesn't count");

    // Extra files can't be slipped in.
    test::write_file(dir / "patches/extra.patchset.toml", "x");
    check(contains(error_of([&] { verify(dir, Depth::Launch); }), "not part of the signed pack"), "an extra file is refused");
    fs::remove(dir / "patches/extra.patchset.toml");
    test::write_file(dir / "patches/.hidden", "x");
    check(error_of([&] { verify(dir, Depth::Full); }).empty(), "dot files don't count");

    fs::rename(dir / "files/data/a.cfg", dir / "a.cfg");
    check(error_of([&] { verify(dir, Depth::Full); }) == "files/data/a.cfg is missing from the signed pack", "a missing file");
    fs::rename(dir / "a.cfg", dir / "files/data/a.cfg");

    // A re-signed manifest with another key verifies as that key, which the trust store then
    // refuses for an already pinned pack.
    SecretKey other = SecretKey::generate();
    sign(dir, other);
    check(verify(dir, Depth::Full) == Signature(other.public_key()), "re-signed: verifies as the other key");

    // Editing the manifest breaks the signature.
    const std::string text = test::read_file(dir / "manifest.json");
    std::string edited = text;
    edited.replace(edited.find("1.0.0"), 5, "1.0.1");
    test::write_file(dir / "manifest.json", edited);
    check(contains(error_of([&] { verify(dir, Depth::Full); }), "changed after it was signed"), "an edited manifest");
    test::write_file(dir / "manifest.json", text);

    // The pack moved to another version under the same manifest.
    const std::string pack_text = test::read_file(dir / "pack.json");
    test::write_file(dir / "pack.json", R"({"format":1,"id":"org.example.test","name":"Test","version":"1.0.1"})");
    check(error_of([&] { verify(dir, Depth::Full); }) == "manifest.json was signed for org.example.test 1.0.0, not org.example.test 1.0.1",
          "a manifest for another version");
    test::write_file(dir / "pack.json", pack_text);

    fs::remove(dir / "manifest.sig");
    check(error_of([&] { verify(dir, Depth::Full); }) == "the pack has only one of manifest.json and manifest.sig", "only a manifest");

    // The manifest's layout: the Rust's field order, pretty, a newline at the end.
    check(text.rfind("{\n  \"format\": 1,\n  \"id\": \"org.example.test\",\n  \"version\": \"1.0.0\",\n  \"key\": \"ed25519:", 0) == 0 &&
              contains(text, "\"files\": {\n    \"files/data/a.cfg\": {\n      \"size\": 3,\n      \"sha256\": \"") &&
              text.back() == '\n',
          "manifest.json laid out like the Rust's");
    check(error_of([] { Manifest::from_json(R"({"format":1,"id":"a","version":"1","key":"k","files":{},"x":1})"); }) ==
              "unknown field `x`, expected one of `format`, `id`, `version`, `key` or `files`",
          "manifest: unknown fields refused");
    check(error_of([] { Manifest::from_json(R"({"format":1,"id":"a","version":"1","key":"k","files":{"p":{"size":-1,"sha256":""}}})"); }) ==
              "invalid value: integer `-1`, expected u64",
          "manifest: negative size refused");
    {
        const fs::path d2 = pack_dir("format");
        sign(d2, key);
        std::string t = test::read_file(d2 / "manifest.json");
        t.replace(t.find("\"format\": 1"), 11, "\"format\": 2");
        test::write_file(d2 / "manifest.json", t);
        test::write_file(d2 / "manifest.sig", sign_bytes(key, t));
        check(error_of([&] { verify(d2, Depth::Full); }) == "manifest.json: format 2 is newer than this launcher understands; update it",
              "a newer manifest format");
    }

    // A files_from pack's game files are signed through its list, not the manifest.
    {
        const fs::path d3 = pack_dir("files-from");
        test::write_file(d3 / "pack.json",
                         R"({"format":1,"id":"org.example.test","name":"Test","version":"1.0.0","files_from":{"scmod1":"https://a.org/l.txt"}})");
        auto files = pack_files(d3, sf::launch::profiles::read_pack(d3));
        check(files == std::vector<std::string>{"pack.json", "patches/game.exe.patchset.toml"}, "files_from: files/ isn't the pack's");
    }
#ifndef _WIN32
    fs::create_symlink("/etc/hostname", dir / "patches/link");
    check(error_of([&] { pack_files(dir, sf::launch::profiles::read_pack(dir)); }) == "patches/link: not a regular file",
          "a link is not a regular file");
#endif
}

void real(const fs::path& profiles) {
    int n = 0;
    for (const auto& e : fs::directory_iterator(profiles)) {
        if (!fs::is_regular_file(e.path() / "manifest.sig")) continue;
        ++n;
        const std::string name = sf::launch::ustr(e.path().filename());
        std::string err;
        Signature sig;
        try {
            sig = verify(e.path(), Depth::Full);
        } catch (const std::exception& ex) {
            err = ex.what();
        }
        check(err.empty() && sig, "real " + name + " verifies" + (err.empty() ? "" : ": " + err));
        if (sig) std::printf("  %s: signed by %s\n", name.c_str(), sig->fingerprint().c_str());
        const std::string text = test::read_file(e.path() / "manifest.json");
        check(Manifest::from_json(text).to_json() == text, "real " + name + "'s manifest.json reads and writes back the same");
    }
    std::printf("checked %d real signed profile(s)\n", n);
}

}  // namespace

int main(int argc, char** argv) {
    test::exit_on_crash();
    base64();
    rfc8032();
    keys();
    detached();
    packs();
    if (argc > 1) real(upath(argv[1]));
    std::printf("%s (%d failed)\n", test::failures ? "FAILED" : "ok", test::failures);
    return test::failures ? 1 : 0;
}
