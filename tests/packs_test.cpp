// Tests sf::launch::packs (pack sources, staging for review, install and update, signed packs,
// update checks). Zips are written with miniz; URL installs come from a local HTTP server.
// Groups marked "Rust:" port packs.rs's tests.

#include <algorithm>

#include <miniz.h>

#include "crypto/sha256.hpp"
#include "launch/env.hpp"
#include "launch/packs.hpp"
#include "launch/profiles.hpp"

#include "check.hpp"
#include "http_server.hpp"

namespace fs = std::filesystem;
using namespace sf::launch::packs;
namespace profiles = sf::launch::profiles;
namespace signing = sf::launch::signing;
namespace trust = sf::launch::trust;
using sf::launch::upath;
using sf::launch::ustr;
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

std::string sha(const std::string& s) { return sf::crypto::sha256_hex(s.data(), s.size()); }

std::string upper(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

std::string zip_bytes(const std::vector<std::pair<std::string, std::string>>& entries) {
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    mz_zip_writer_init_heap(&zip, 0, 0);
    for (const auto& [name, body] : entries)
        if (!mz_zip_writer_add_mem(&zip, name.c_str(), body.data(), body.size(), MZ_DEFAULT_COMPRESSION))
            check(false, "test bug: miniz refused " + name);
    void* buf = nullptr;
    size_t size = 0;
    mz_zip_writer_finalize_heap_archive(&zip, &buf, &size);
    std::string out(static_cast<char*>(buf), size);
    mz_free(buf);
    mz_zip_writer_end(&zip);
    return out;
}

void write_zip(const fs::path& path, const std::vector<std::pair<std::string, std::string>>& entries) {
    test::write_file(path, zip_bytes(entries));
}

std::string manifest(const std::string& v, const std::string& extra = "") {
    return R"({"format":1,"id":"org.example.test","name":"Test Server","version":")" + v +
           R"(","server":{"address":"a.org:1"})" + extra + "}";
}

// Rust: parses_sources.
void sources() {
    auto url = [](const std::string& s) { return Source::parse(s).url; };
    check(Source::parse("https://a.org/p.json") == Source{Source::Url, {}, "https://a.org/p.json"}, "an https URL");
    check(url("starforge://install?url=https%3A%2F%2Fa.org%2Fpack.json") == "https://a.org/pack.json", "a starforge link, decoded");
    check(url("starforge://install/?x=1&url=https://a.org/p.zip") == "https://a.org/p.zip", "a starforge link with other fields");
    check(url("  starforge://install?url=https://a.org/a+b%20c.zip ") == "https://a.org/a b c.zip", "+ and %20 are spaces, trimmed");
    check(error_of([] { Source::parse("starforge://install?url=http%3A%2F%2Fa.org%2Fp.zip"); }) ==
              "http://a.org/p.zip: packs must be downloaded over https",
          "a starforge link to plain http");
    check(error_of([] { Source::parse("starforge://other"); }) == "starforge link without an install url: starforge://other",
          "a starforge link that isn't an install");
    check(error_of([] { Source::parse("starforge://installx?url=https://a.org/p"); }) ==
              "starforge link without an install url: starforge://installx?url=https://a.org/p",
          "installx isn't install");
    check(error_of([] { Source::parse("starforge://install?url=https%3"); }) == "bad %-escape in link", "a cut %-escape");
    check(error_of([] { Source::parse("starforge://install?url=https%3A%2F%2Fa.org%2F%FF"); }) == "link is not valid UTF-8",
          "a link that isn't UTF-8");
    check(error_of([] { Source::parse("ftp://a.org/p.zip"); }) == "unsupported link: ftp://a.org/p.zip", "ftp");
    check(error_of([] { Source::parse("http://localhost:8000/p.zip"); }).empty(), "http from this machine is fine");
    check(error_of([] { Source::parse("http://[::1]/p.zip"); }).empty(), "... [::1] too");
    check(error_of([] { Source::parse("http://evil.org/p.zip"); }) == "http://evil.org/p.zip: packs must be downloaded over https",
          "http from elsewhere");
    check(error_of([] { Source::parse("http://localhost.evil.org/p.zip"); }) != "", "a look-alike localhost");
    check(Source::parse("/home/user/pack.zip") == Source{Source::Path, upath("/home/user/pack.zip"), ""}, "a path");
}

// Rust: filters_pack_entries.
void entries() {
    for (const char* ok : {"pack.json", "files/data/address.cfg", "resources/logo.png", "patches/game.exe.patchset.toml",
                           "README.md", "LICENSE", "changelog.txt", "manifest.json", "manifest.sig"})
        check(allowed(ok), std::string("allowed: ") + ok);
    for (const char* bad : {"files", "local.json", "agent.log", "__MACOSX/x", "files/data/.gitkeep", ".hidden/x", "readme/x", "state/x"})
        check(!allowed(bad), std::string("not allowed: ") + bad);
    check(clean_rel("a/../b") == std::nullopt, "clean_rel: ..");
    check(clean_rel("C:/x") == std::nullopt, "clean_rel: a drive");
    check(clean_rel("files/data/a.cfg.") == std::nullopt, "clean_rel: a trailing dot");
    check(clean_rel("files/con.lua") == std::nullopt, "clean_rel: a device name");
    check(clean_rel("/a//b\\c") == std::optional<std::string>("a/b/c"), "clean_rel: empty parts and backslashes");
    check(clean_rel("//") == std::nullopt, "clean_rel: nothing");
}

// Rust: installs_and_updates_a_pack.
void installs(const fs::path& base) {
    const fs::path profiles_dir = base / "profiles";
    const fs::path v1 = base / "v1.zip";
    write_zip(v1, {{"test/pack.json", manifest("1.0.0")},
                   {"test/files/data/a.cfg", "one"},
                   {"test/files/data/old.cfg", "old"},
                   {"test/local.json", R"({"name":"sneaky"})"},
                   {"test/../escape.txt", "x"}});
    Source src;
    src.path = v1;
    Staged staged = stage(src, profiles_dir);
    check(staged.files == std::vector<std::string>{"data/a.cfg", "data/old.cfg"}, "staged files");
    check(!staged.verified, "a local zip isn't verified");
    check(staged.trust == Trust{}, "unsigned");
    check(staged.origin == ustr(v1), "origin is the path");
    check(staged.reach.files == 2 && !staged.reach.runs_code(), "reach: two files, no code");
    check(staged.warnings == std::vector<std::string>{"This pack has no update address, so the launcher can't check for new versions."},
          "the one warning: no update address");
    check(fs::is_regular_file(staged.root / "pack.json") && staged.dir.parent_path() == profiles_dir / ".staging",
          "staged inside the profiles folder");
    const std::string name = install(staged, profiles_dir, sf::launch::remote::quiet).name;
    check(name == "test-server", "installed as test-server");
    const fs::path dir = profiles_dir / name;
    check(!fs::exists(dir / "local.json"), "packs can't ship player settings");
    check(!fs::exists(base / "escape.txt") && !fs::exists(profiles_dir / "escape.txt"), "nothing escapes");
    check(!fs::exists(staged.dir), "the staging folder is gone");

    sf::pack::Local mine;
    mine.name = "Mine";
    profiles::write_local(dir, mine);
    test::write_file(dir / "state/x", "the game's");
    const fs::path v2 = base / "v2.zip";
    write_zip(v2, {{"pack.json", manifest("1.1.0")}, {"files/data/a.cfg", "two"}});
    src.path = v2;
    const std::string name2 = install(stage(src, profiles_dir), profiles_dir, sf::launch::remote::quiet).name;
    check(name2 == name, "same pack id updates in place");
    check(test::read_file(dir / "files/data/a.cfg") == "two", "a.cfg updated");
    check(!fs::exists(dir / "files/data/old.cfg"), "old.cfg gone");
    check(sf::pack::Profile::load(dir).name == "Mine", "local settings survive updates");
    check(test::read_file(dir / "state/x") == "the game's", "the game's data survives updates");
    check(profiles::read_pack(dir).version == "1.1.0", "the new version");
    check(!fs::exists(dir / ".update-old"), "no leftovers");

    const fs::path bad = base / "bad.zip";
    write_zip(bad, {{"readme.txt", "no manifest"}});
    src.path = bad;
    check(error_of([&] { stage(src, profiles_dir); }) == ustr(bad) + ": no pack.json at the top of the zip", "a zip without pack.json");
    write_zip(bad, {{"a/pack.json", manifest("1.0.0")}, {"b/x", "y"}});
    check(contains(error_of([&] { stage(src, profiles_dir); }), "no pack.json at the top"), "two top folders");
    test::write_file(bad, "not a zip");
    check(contains(error_of([&] { stage(src, profiles_dir); }), ": not a pack zip ("), "not a zip");
    check(fs::is_empty(profiles_dir / ".staging"), "failed stages leave nothing behind");

    // A pack folder.
    const fs::path folder = base / "folder";
    test::write_file(folder / "pack.json", manifest("1.2.0"));
    test::write_file(folder / "files/data/a.cfg", "three");
    test::write_file(folder / "local.json", "{}");
    test::write_file(folder / "notes/x.txt", "x");
    src.path = folder;
    Staged f = stage(src, profiles_dir);
    check(f.files == std::vector<std::string>{"data/a.cfg"} && !fs::exists(f.root / "local.json") && !fs::exists(f.root / "notes"),
          "a folder: only the pack's parts are copied");
    discard(f);
    check(!fs::exists(f.dir), "discard removes the staging folder");
    src.path = base / "nothing";
    fs::create_directories(src.path);
    check(error_of([&] { stage(src, profiles_dir); }) == ustr(src.path) + ": no pack.json in this folder", "a folder without pack.json");

    // Checks a pack must pass.
    write_zip(bad, {{"pack.json", manifest("1.0.0", R"(,"requires":{"launcher":"99.0.0"})")}});
    src.path = bad;
    check(error_of([&] { stage(src, profiles_dir); }) ==
              std::string("Test Server needs STAR FORGE Launcher 99.0.0 or newer (this is ") + SF_VERSION + "); update the launcher first",
          "a newer launcher required");
    write_zip(bad, {{"pack.json", manifest("1.0.0", R"(,"patches":{"core":["no-such"]})")}});
    check(contains(error_of([&] { stage(src, profiles_dir); }), "unknown core patch \"no-such\""), "an unknown core patch");
    write_zip(bad, {{"pack.json", manifest("1.0.0", R"(,"files_from":{"scmod1":"https://a.org/l"})")}, {"files/x", "y"}});
    check(error_of([&] { stage(src, profiles_dir); }) == "the pack has both a files/ folder and files_from; use one or the other",
          "files/ and files_from");
    write_zip(bad, {{"pack.json", manifest("1.0.0", R"(,"branding":{"logo":"resources/logo.png","background":["resources/a.png","resources/b.png"]},"patches":{"plugins":["patches/x.dll"]})")},
                    {"resources/a.png", "png"},
                    {"patches/broken.patchset.toml", "format = 2"},
                    {"patches/x.dll", "MZ"}});
    Staged w = stage(src, profiles_dir);
    check(w.warnings.size() == 6 && w.warnings[0] == "branding.logo points at resources/logo.png, which is not in the pack" &&
              w.warnings[1] == "branding.background points at resources/b.png, which is not in the pack" &&
              contains(w.warnings[2], "Broken patch file: patches/broken.patchset.toml") &&
              contains(w.warnings[3], "changes the game's code but isn't signed") && contains(w.warnings[4], "(plugins)") &&
              contains(w.warnings[5], "no update address"),
          "warnings, in the Rust's order: " + std::to_string(w.warnings.size()));
    check(w.reach.patchsets == 1 && w.reach.plugins == 1 && w.reach.runs_code(), "reach counts code");
    discard(w);
}

// Rust: signed_packs_keep_their_key.
void signed_packs(const fs::path& base) {
    const fs::path profiles_dir = base / "signed-profiles";
    auto make = [&](const std::string& name, const std::string& version, const signing::SecretKey* key) {
        const fs::path dir = base / name;
        test::write_file(dir / "pack.json", R"({"format":1,"id":"org.example.signed","name":"Signed","version":")" + version + R"("})");
        test::write_file(dir / "patches/game.exe.patchset.toml", "x");
        if (key) signing::sign(dir, *key);
        Source s;
        s.path = dir;
        return s;
    };
    auto key = signing::SecretKey::generate();
    auto other = signing::SecretKey::generate();

    Staged staged = stage(make("v1", "1.0.0", &key), profiles_dir);
    check(staged.trust == Trust{Trust::NewKey, key.public_key(), ""}, "a new key");
    check(staged.reach.runs_code(), "a patchset is code");
    check(std::none_of(staged.warnings.begin(), staged.warnings.end(), [](const std::string& w) { return contains(w, "isn't signed"); }),
          "no unsigned-code warning");
    const std::string before = test::read_file(staged.root / "pack.json");
    const std::string name = install(staged, profiles_dir, sf::launch::remote::quiet).name;
    const fs::path dir = profiles_dir / name;
    check(test::read_file(dir / "pack.json") == before, "a signed pack.json stays byte for byte");
    check(verify_installed(dir) == Trust{Trust::Pinned, key.public_key(), ""}, "pinned on install");

    check(contains(error_of([&] { stage(make("v2-other", "1.1.0", &other), profiles_dir); }), "was signed with key"),
          "another key can't update it");
    check(contains(error_of([&] { stage(make("v2-unsigned", "1.1.0", nullptr), profiles_dir); }), "normally signed"),
          "nor an unsigned copy");
    Staged v2 = stage(make("v2", "1.1.0", &key), profiles_dir);
    check(v2.trust == Trust{Trust::Pinned, key.public_key(), ""}, "the same key: pinned");
    install(v2, profiles_dir, sf::launch::remote::quiet);
    check(profiles::read_pack(dir).version == "1.1.0", "updated");

    test::write_file(dir / "patches/game.exe.patchset.toml", "tampered");
    check(error_of([&] { verify_installed(dir); }) ==
              "Signed: patches/game.exe.patchset.toml does not match the signed pack. Reinstall the pack to repair it.",
          "a tampered pack is refused before launch");
    test::write_file(dir / "patches/game.exe.patchset.toml", "x");

    // Signed, but installed some other way: pinned at its first launch.
    trust::unpin("org.example.signed");
    check(verify_installed(dir) == Trust{Trust::Pinned, key.public_key(), ""}, "pinned at first launch");
    check(trust::pinned("org.example.signed") == key.public_key(), "... in trust.json");
    trust::unpin("org.example.signed");
}

void urls(const fs::path& base) {
    const fs::path profiles_dir = base / "url-profiles";
    test::FileServer srv;
    const std::string u = srv.base();
    const std::string zip = zip_bytes({{"pack.json", manifest("2.0.0")}, {"files/data/a.cfg", "zipped"}});
    srv.put("/p.zip", zip);
    srv.put("/pack.json", manifest("2.0.0", R"(,"download":{"url":")" + u + R"(/p.zip","sha256":")" + sha(zip) + R"("})"));
    srv.put("/bad.json", manifest("2.0.0", R"(,"download":{"url":")" + u + R"(/p.zip","sha256":")" + sha("other") + R"("})"));
    srv.put("/wrong.json", manifest("3.0.0", R"(,"download":{"url":")" + u + R"(/p.zip","sha256":")" + sha(zip) + R"("})"));
    srv.put("/list.txt", "SCMOD1\ndata/l.cfg\t" + sha("listed") + "\t" + u + "/l.cfg\n");
    srv.put("/l.cfg", "listed");
    srv.put("/listed.json", R"({"format":1,"id":"org.example.listed","name":"Listed","version":"1.0.0","files_from":{"scmod1":")" + u +
                                R"(/list.txt"}})");

    auto from = [&](const std::string& path) {
        Source s;
        s.kind = Source::Url;
        s.url = u + path;
        return s;
    };
    Staged z = stage(from("/p.zip"), profiles_dir);
    check(!z.verified && z.origin == u + "/p.zip" && z.files == std::vector<std::string>{"data/a.cfg"}, "a zip by URL");
    discard(z);

    Staged p = stage(from("/pack.json"), profiles_dir);
    check(p.verified, "a published pack.json's zip is verified by its SHA-256");
    check(p.pack.update && p.pack.update->url == u + "/pack.json", "unsigned: updates come from the same address");
    check(!p.pack.download, "download is dropped");
    check(profiles::read_pack(p.root).update->url == u + "/pack.json", "... and written to the staged pack.json");
    const std::string name = install(p, profiles_dir, sf::launch::remote::quiet).name;
    check(test::read_file(profiles_dir / name / "files/data/a.cfg") == "zipped", "installed from the zip");

    check(error_of([&] { stage(from("/bad.json"), profiles_dir); }) ==
              u + "/p.zip: download does not match the SHA-256 in " + u + "/bad.json; not installing it",
          "a zip that doesn't match its SHA-256");
    check(error_of([&] { stage(from("/wrong.json"), profiles_dir); }) ==
              "the published pack.json describes org.example.test 3.0.0 but the download contains org.example.test 2.0.0",
          "a zip with another version");

    // A published pack.json without download is the whole pack; its files come from its list.
    Staged l = stage(from("/listed.json"), profiles_dir);
    check(l.remote && l.files == std::vector<std::string>{"data/l.cfg"} && l.reach.files == 1, "a files_from pack: the list's files");
    check(std::any_of(l.warnings.begin(), l.warnings.end(), [](const std::string& w) { return contains(w, "without a signature"); }),
          "an unsigned list is mentioned");
    auto inst = install(l, profiles_dir, sf::launch::remote::quiet);
    check(!inst.warning && test::read_file(profiles_dir / inst.name / "files/data/l.cfg") == "listed", "its files downloaded on install");
    srv.put("/list.txt", "SCMOD1\ndata/l.cfg\t" + sha("new") + "\t" + u + "/l2.cfg\n");
    srv.put("/l2.cfg", "not what the list says");
    Staged l2 = stage(from("/listed.json"), profiles_dir);
    inst = install(l2, profiles_dir, sf::launch::remote::quiet);
    check(inst.warning && contains(*inst.warning, "Listed is installed, but its game files didn't all download ("),
          "a file that fails its hash: installed, with a warning");

    // Update checks.
    sf::pack::Pack installed = profiles::read_pack(profiles_dir / name);
    check(!check_update(installed), "the same version: nothing new");
    srv.put("/pack.json", manifest("2.1.0"));
    auto found = check_update(installed);
    check(found && found->version == "2.1.0" && !found->required && found->url == u + "/pack.json", "a newer version");
    srv.put("/pack.json", manifest("2.1.0", R"(,"update":{"url":"https://x.org/p.json","min_version":"2.1.0"})"));
    found = check_update(installed);
    check(found && found->required, "a newer version the server requires");
    srv.put("/pack.json", R"({"format":1,"id":"org.example.other","name":"O","version":"9.0.0"})");
    check(error_of([&] { check_update(installed); }) == u + "/pack.json: publishes pack org.example.other instead of org.example.test",
          "another pack at the address");
    srv.put("/pack.json", manifest("1.0.0"));
    check(!check_update(installed), "an older version: nothing new");
    installed.update->url = "http://evil.org/p.json";
    check(contains(error_of([&] { check_update(installed); }), "packs must be downloaded over https"), "updates need https too");
    installed.update.reset();
    check(!check_update(installed), "no update address: nothing to check");

    // The game build a pack was made for.
    const fs::path exe = base / "game.exe";
    test::write_file(exe, "the game");
    sf::pack::Pack g;
    g.name = "G";
    check(!game_build_warning(g, exe), "no builds listed: no warning");
    g.requires_.game = {std::string(64, '0'), upper(sha("the game"))};
    check(!game_build_warning(g, exe), "a listed build (any case): no warning");
    g.requires_.game = {std::string(64, '0')};
    check(game_build_warning(g, exe) == "G was made for a different game build; its files and patches may not work with this one.",
          "another build: a warning");
    check(!game_build_warning(g, base / "missing.exe"), "no game.exe to check: no warning");
}

}  // namespace

int main() {
    test::exit_on_crash();
    const fs::path base = test::fresh_dir(fs::temp_directory_path() / "sf-packs-test");
    sf::launch::set_env(sf::launch::env::kDataDir, ustr(base / "data"));
    sources();
    entries();
    installs(base);
    signed_packs(base);
    urls(base);
    std::printf("%s (%d failed)\n", test::failures ? "FAILED" : "ok", test::failures);
    return test::failures ? 1 : 0;
}
