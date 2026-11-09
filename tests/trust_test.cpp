// Tests sf::launch::trust (which key may sign which pack) and sf::launch::patching (the
// launcher's summary of a profile's patches and its check against the game before a launch).
// Groups marked "Rust:" port the Rust tests.

#include <cstring>

#include "launch/env.hpp"
#include "launch/patching.hpp"
#include "launch/trust.hpp"
#include "patch/sha1.hpp"

#include "check.hpp"

namespace fs = std::filesystem;
namespace trust = sf::launch::trust;
namespace patching = sf::launch::patching;
using sf::launch::signing::PublicKey;
using sf::launch::signing::SecretKey;
using sf::launch::signing::Signature;
using test::check;
using test::contains;
using Trust = trust::Trust;

namespace {

template <class F>
std::string error_of(F f) {
    try {
        f();
        return "";
    } catch (const std::exception& e) {
        return e.what();
    }
}

// Rust: pins_on_first_use, then more.
void pins(const fs::path& data) {
    const PublicKey a = SecretKey::generate().public_key(), b = SecretKey::generate().public_key();
    const std::string id = "org.example.trust";
    check(trust::check(id, std::nullopt) == Trust{}, "unsigned, nothing pinned: unsigned");
    check(trust::check(id, a) == Trust{Trust::NewKey, a, ""}, "a new key");
    trust::pin(id, a);
    check(trust::check(id, a) == Trust{Trust::Pinned, a, ""}, "the pinned key");
    check(error_of([&] { trust::check(id, b); }) ==
              id + " was signed with key " + a.fingerprint() + " when you installed it, but this copy is signed with " +
                  b.fingerprint() + ". If its server really changed keys, remove the profile and install the pack again.",
          "another key is refused");
    check(error_of([&] { trust::check(id, std::nullopt); }) ==
              id + " is normally signed by its publisher, but this copy isn't; not trusting it",
          "can't drop the signature once pinned");
    check(trust::check("org.example.other", b) == Trust{Trust::NewKey, b, ""}, "pins are per pack id");
    check(test::read_file(data / "trust.json") == "{\n  \"packs\": {\n    \"org.example.trust\": {\n      \"key\": \"" + a.to_string() +
                                                     "\"\n    }\n  }\n}\n",
          "trust.json laid out like the Rust's");
    trust::unpin(id);
    check(trust::check(id, b) == Trust{Trust::NewKey, b, ""}, "unpinned: any key again");
    check(test::read_file(data / "trust.json") == "{\n  \"packs\": {}\n}\n", "trust.json emptied");

    // A damaged store stops installs instead of trusting everything.
    test::write_file(data / "trust.json", R"({"packs":{"x":{"key":"k","extra":1}}})");
    check(contains(error_of([&] { trust::check(id, b); }), "trust.json: unknown field `extra`"), "a damaged trust.json is an error");
    test::write_file(data / "trust.json", "{}");
    check(error_of([&] { trust::check(id, b); }).empty(), "packs may be missing");
    fs::remove(data / "trust.json");
}

void known() {
    const SecretKey pub_key = SecretKey::generate();
    const std::string key_text = pub_key.public_key().to_string();
    const std::vector<trust::KnownPublisher> list{{"Starfury", "xyz.starfury", key_text.c_str()}};
    trust::set_known_for_test(&list);
    const PublicKey k = pub_key.public_key(), other = SecretKey::generate().public_key();
    check(trust::check("xyz.starfury", k) == Trust{Trust::Known, k, "Starfury"}, "a known publisher's own id");
    check(trust::check("xyz.starfury.main", k) == Trust{Trust::Known, k, "Starfury"}, "an id under its prefix (.)");
    check(trust::check("xyz.starfury-test", k) == Trust{Trust::Known, k, "Starfury"}, "an id under its prefix (-)");
    check(trust::check("xyz.starfurynot", other) == Trust{Trust::NewKey, other, ""}, "a look-alike id isn't theirs");
    check(error_of([&] { trust::check("xyz.starfury.main", other); }) ==
              "xyz.starfury.main belongs to Starfury, but this copy is signed by someone else (key " + other.fingerprint() + ")",
          "their id with another key is refused, even on first install");
    check(contains(error_of([&] { trust::check("xyz.starfury.main", std::nullopt); }), "normally signed"),
          "their id unsigned is refused");
    trust::set_known_for_test(nullptr);
    check(trust::kKnown.empty(), "no publishers built in yet");
}

// --- patching ---

const std::string PACK = R"({"format":1,"id":"org.example.p","name":"P","version":"1.0.0")";

// patch_test's minimal PE32+: one section at rva 0x1000, file offset 0x200, 0x10 bytes.
std::string pe() {
    std::string d(0x210, '\0');
    d[0] = 'M';
    d[1] = 'Z';
    d[0x3c] = 0x40;
    std::memcpy(&d[0x40], "PE\0\0", 4);
    d[0x46] = 1;
    d[0x54] = static_cast<char>(0xf0);
    d[0x58] = 0x0b;
    d[0x59] = 0x02;
    const uint64_t base = 0x140000000;
    std::memcpy(&d[0x70], &base, 8);
    const size_t s = 0x58 + 0xf0;
    const uint32_t vsize = 0x10, rva = 0x1000, raw_size = 0x10, raw = 0x200;
    std::memcpy(&d[s + 8], &vsize, 4);
    std::memcpy(&d[s + 12], &rva, 4);
    std::memcpy(&d[s + 16], &raw_size, 4);
    std::memcpy(&d[s + 20], &raw, 4);
    d[0x202] = 0x74;
    d[0x203] = 0x05;
    d[0x204] = static_cast<char>(0xaa);
    return d;
}

std::string set_for(const std::string& exe, const std::string& sha1) {
    return "format = 1\nexe = \"" + exe + "\"\nsha1 = \"" + sha1 + "\"\nimage_base = 0x140000000\n\n"
           "[[patch]]\nid = \"one\"\n[[patch.write]]\nva = 0x140001002\norig = \"74 05\"\nbytes = \"90 90\"\n\n"
           "[[patch]]\nid = \"two\"\ndefault = false\noptional = true\n[[patch.write]]\nva = 0x140001006\norig = \"ffff\"\nbytes = \"1111\"\n";
}

void patches(const fs::path& base) {
    const std::string game = pe();
    const std::string game_sha = sf::patch::sha1_hex(game.data(), game.size());
    const fs::path win64 = base / "game/win64";
    test::write_file(win64 / "Game.EXE", game);  // found whatever its case
    const fs::path exe = win64 / "game.exe";

    const fs::path dir = test::fresh_dir(base / "profile");
    test::write_file(dir / "pack.json", PACK + "}");
    test::write_file(dir / "patches/game.exe.patchset.toml", set_for("game.exe", game_sha));
    auto profile = sf::pack::Profile::load(dir);
    patching::Summary s = patching::summary(dir, profile);
    check(s == patching::Summary{{"game.exe"}, 1, 0, {}}, "summary: one module, one patch on");
    check(error_of([&] { check(patching::preflight(dir, profile, exe).empty(), "a matching game: no warnings"); }).empty(),
          "a matching game starts");

    // The player turns the optional patch on; it doesn't fit, so it's skipped with a warning.
    test::write_file(dir / "local.json", R"({"patches":{"GAME.exe":{"two":true}}})");
    profile = sf::pack::Profile::load(dir);
    check(patching::summary(dir, profile).patches == 2, "the player's choice counts (module name in any case)");
    auto w = patching::preflight(dir, profile, exe);
    check(w.size() == 1 && contains(w[0], "game.exe: optional patch skipped: "), "an optional patch that doesn't fit: skipped");

    // Another game build stops the launch...
    test::write_file(dir / "patches/game.exe.patchset.toml", set_for("game.exe", std::string(40, '0')));
    std::string err = error_of([&] { patching::preflight(dir, profile, exe); });
    check(err.rfind("The pack's game patches don't match this game: game.exe is a different build", 0) == 0,
          "another build stops the launch: " + err);
    // ...unless the pack says to go on.
    test::write_file(dir / "pack.json", PACK + R"(,"patches":{"on_error":"continue"}})");
    profile = sf::pack::Profile::load(dir);
    w = patching::preflight(dir, profile, exe);
    check(!w.empty() && contains(w[0], "is a different build") && contains(w[0], ". Starting anyway (the pack allows it)."),
          "on_error continue: a warning first");

    // More problems, joined.
    test::write_file(dir / "pack.json", PACK + R"(,"patches":{"plugins":["patches/x.dll"]}})");
    test::write_file(dir / "patches/game.exe.patchset.toml", set_for("game.exe", game_sha));
    test::write_file(dir / "patches/other.dll.patchset.toml", set_for("other.dll", game_sha));
    test::write_file(dir / "patches/broken.patchset.toml", "format = 2");
    test::write_file(dir / "patches/more.patchset.toml", set_for("game.exe", game_sha));
    profile = sf::pack::Profile::load(dir);
    s = patching::summary(dir, profile);
    check(s.modules == std::vector<std::string>{"game.exe", "other.dll"} && s.plugins == 1 && s.errors.size() == 1 &&
              contains(s.errors[0], "patches/broken.patchset.toml"),
          "summary: modules, plugins, and the broken file");
    err = error_of([&] { patching::preflight(dir, profile, exe); });
    check(contains(err, "other.dll is not in the game folder") && contains(err, "plugin patches/x.dll is missing from the profile") &&
              contains(err, "patches/broken.patchset.toml") && contains(err, "; "),
          "problems joined: " + err);
    // A module in the game root (one up from win64) is found too.
    fs::remove(dir / "patches/broken.patchset.toml");
    fs::remove(dir / "patches/more.patchset.toml");
    test::write_file(base / "game/other.dll", game);
    test::write_file(dir / "patches/x.dll", "MZ");
    profile = sf::pack::Profile::load(dir);
    check(error_of([&] { patching::preflight(dir, profile, exe); }).empty(), "a module under the game root is found");

    // Core patches come first; an unknown one is an error.
    test::write_file(dir / "pack.json", PACK + R"(,"patches":{"core":["no-such-patch"]}})");
    profile = sf::pack::Profile::load(dir);
    s = patching::summary(dir, profile);
    check(s.errors.size() == 1 && contains(s.errors[0], "unknown core patch \"no-such-patch\""), "an unknown core patch");
}

}  // namespace

int main() {
    test::exit_on_crash();
    const fs::path base = test::fresh_dir(fs::temp_directory_path() / "sf-trust-test");
    const fs::path data = base / "data";
    sf::launch::set_env(sf::launch::env::kDataDir, sf::launch::ustr(data));
    pins(data);
    known();
    patches(base);
    std::printf("%s (%d failed)\n", test::failures ? "FAILED" : "ok", test::failures);
    return test::failures ? 1 : 0;
}
