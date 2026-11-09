// Tests sf::pack (pack.json / local.json) and sf::launch::profiles (profiles folder, state sync).
// Groups marked "Rust:" port sc-profile's and sc-launch's tests case for case.
//
// Usage: pack_test [profiles folder]. With a folder of real installed profiles, also checks each
// one loads, and says whether its pack.json comes out byte for byte the same (it does for one the
// Rust launcher wrote).

#include <thread>

#include <nlohmann/json.hpp>

#include "launch/profiles.hpp"
#include "pack/pack.hpp"

#include "check.hpp"

using namespace sf::pack;
namespace profiles = sf::launch::profiles;
using sf::launch::upath;
using sf::launch::ustr;
using test::check;
using test::contains;

namespace {

const std::string MINIMAL = R"({"format":1,"id":"xyz.starfury.main","name":"Starfury","version":"1.0.0"})";

// The error from parsing `text` (and validating, if `validate`), or "" if it was fine.
std::string pack_error(const std::string& text, bool validate = true) {
    try {
        Pack p = Pack::from_json(text);
        if (validate) p.validate();
        return "";
    } catch (const Error& e) {
        return e.what();
    }
}

// MINIMAL with the top-level fields in `edit` (a JSON object) replaced, like the Rust test's bad().
std::string edited(const std::string& edit) {
    nlohmann::json v = nlohmann::json::parse(MINIMAL);
    const nlohmann::json e = nlohmann::json::parse(edit);
    for (auto it = e.begin(); it != e.end(); ++it) v[it.key()] = it.value();
    return v.dump();
}
bool bad(const std::string& edit) { return !pack_error(edited(edit)).empty(); }

}  // namespace

int main(int argc, char** argv) {
    test::exit_on_crash();
    const fs::path tmp = test::fresh_dir(fs::temp_directory_path() / "sf_pack_test");

    // Rust: pack_parses_and_merges_local_settings
    {
        Pack p = Pack::from_json(R"({
              "$schema": "x", "format": 1, "id": "xyz.starfury.main", "name": "Starfury", "version": "1.2.0",
              "server": { "address": "starfury.xyz:3801" },
              "game": { "args": ["-a"] },
              "patches": { "on_error": "continue", "plugins": ["patches/hooks.dll"] }
            })");
        check(pack_error(p.to_json()).empty(), "a full pack validates");
        Profile plain = Profile::from(p, Local{});
        check(plain.name == "Starfury", "name from the pack");
        check(plain.full_args() == std::vector<std::string>{"-a"}, "args from the pack");
        check(plain.address_cfg() == std::string("set net_masterServerAddress \"starfury.xyz:3801\"\r\n"), "address.cfg");
        check(plain.patches.method == PatchMethod::Inject && plain.patches.on_error == OnError::Continue, "patches settings");
        Local local = Local::from_json(
            R"({"name":"Mine","server":"","local_server":true,"args":["-b"],"trace_files":true,"patches":{"game.exe":{"x":false}}})");
        Profile mine = Profile::from(p, local);
        check(mine.name == "Mine" && !mine.server, "local.json name; server \"\" = none");
        check(mine.full_args() == std::vector<std::string>{"-launchLocalServer", "-a", "-b"}, "local server first, then pack, then local args");
        check(mine.trace_files && plain.own_game_data, "trace_files; own game data by default");
        Local shared;
        shared.shared_game_data = true;
        check(!Profile::from(p, shared).own_game_data, "shared_game_data");
        check(mine.patch_choices.at("game.exe").at("x") == false, "patch choices");
        Local blank;
        blank.name = "  ";
        check(Profile::from(p, blank).name == "Starfury", "a blank local name falls back to the pack's");
    }

    // Rust: pack_rejects_bad_input
    check(contains(pack_error(R"({"format":1,"id":"a","name":"a","version":"1.0.0","bogus":1})"),
                   "pack.json: unknown field `bogus`, expected one of `$schema`, `format`"),
          "an unknown field is an error, naming it");
    check(!bad("{}"), "MINIMAL is fine");
    for (const char* e : {R"({"format":2})", R"({"id":"Upper.Case"})", R"({"id":"a..b"})", R"({"version":"1.0"})",
                          R"({"server":{"address":"evil\"; quit"}})", R"({"branding":{"logo":"../logo.png"}})",
                          R"({"branding":{"logo":"resources/../x"}})", R"({"branding":{"accent":"red"}})",
                          R"({"branding":{"background":["resources/a.jpg","../b.jpg"]}})", R"({"requires":{"game":["abc"]}})",
                          R"({"update":{"url":"https://a.org/pack.json","min_version":"soon"}})", R"({"files_from":{}})",
                          R"({"files_from":{"manifest":"https://a.org/a.json","scmod1":"https://a.org/b.txt"}})",
                          R"({"files_from":{"manifest":"ftp://a.org/a.json"}})", R"({"patches":{"plugins":["../x.dll"]}})",
                          R"({"patches":{"plugins":["patches/x.exe"]}})", R"({"patches":{"core":["Bad_ID"]}})",
                          R"({"download":{"url":"https://a.org/p.zip","sha256":"xyz"}})", R"({"name":"  "})"})
        check(bad(e), std::string("rejected: ") + e);
    for (const char* e : {R"({"branding":{"background":["resources/a.jpg","resources/b.jpg"]}})",
                          R"({"update":{"url":"https://a.org/pack.json","min_version":"1.0.0"}})",
                          R"({"files_from":{"scmod1":"https://a.org/manifest.txt"}})", R"({"patches":{"plugins":["patches/X.DLL"]}})"})
        check(!bad(e), std::string("accepted: ") + e);
    check(contains(pack_error(R"({"format":1,"id":"a","name":"a","version":"1.0.0","patches":{"method":"bake"}})"),
                   "unknown variant `bake`, expected `inject`"),
          "patches.method other than inject");
    check(contains(pack_error(R"({"format":1,"id":"a","name":"a"})"), "missing field `version`"), "a missing field is named");
    check(contains(pack_error(R"({"format":"1","id":"a","name":"a","version":"1.0.0"})"), "invalid type: string \"1\", expected u32"),
          "a wrong type says what was expected");
    check(contains(pack_error(R"({"format":-1,"id":"a","name":"a","version":"1.0.0"})"), "expected u32"), "format -1");
    check(contains(pack_error("{nope"), "pack.json: "), "a syntax error names the file");
    check(contains(pack_error(edited(R"({"id":"Upper.Case"})")), "pack.json: id \"Upper.Case\": use lower-case"),
          "validation messages name the file and quote the value");
    check(pack_error(edited(R"({"server":{"address":"evil\"; quit"}})")) == "server \"evil\\\"; quit\": expected host:port",
          "address message as in Rust (quotes escaped)");
    check(!pack_error(edited(R"({"description":null})"), false).empty(), "null for a non-optional field is an error");
    check(pack_error(edited(R"({"update":null,"files_from":null})"), false).empty(), "null for an optional field is unset");

    // Rust: versions_and_resources
    check(parse_version("1.2.3") == std::tuple<uint64_t, uint64_t, uint64_t>{1, 2, 3}, "version 1.2.3");
    check(parse_version("1.2.3-beta.1") == std::tuple<uint64_t, uint64_t, uint64_t>{1, 2, 3}, "version with -pre");
    check(parse_version("1.2.3+build.7") == std::tuple<uint64_t, uint64_t, uint64_t>{1, 2, 3}, "version with +build");
    check(!parse_version("1.2") && !parse_version("1.2.3.4") && !parse_version("1..3") && !parse_version("a.b.c") &&
              !parse_version("99999999999999999999.0.0"),
          "bad versions");
    check(parse_version("1.10.0") > parse_version("1.9.9"), "versions compare as numbers");
    check(resource_rel("resources/img/logo.png") == std::string("img/logo.png"), "resource_rel");
    check(!resource_rel("resources/") && !resource_rel("files/x.png") && !resource_rel("resources/a\\b") &&
              !resource_rel("resourcesx/a"),
          "resource_rel refuses");
    check(valid_pack_id("org.example.server-1") && !valid_pack_id("") && !valid_pack_id("a.") && !valid_pack_id("a_b") &&
              !valid_pack_id(std::string(129, 'a')),
          "valid_pack_id");

    // Rust: pack_round_trips, background_is_one_or_many
    {
        Pack p = Pack::from_json(MINIMAL);
        check(Pack::from_json(p.to_json()).id == p.id, "round trip");
        check(!contains(p.to_json(), "patches"), "default patches are left out");
        check(p.to_json() == "{\n  \"format\": 1,\n  \"id\": \"xyz.starfury.main\",\n  \"name\": \"Starfury\",\n  \"version\": "
                             "\"1.0.0\",\n  \"server\": {\n    \"local\": false\n  }\n}\n",
              "to_json: serde_json's pretty layout, server always written");
        Pack one = Pack::from_json(edited(R"({"branding":{"background":"resources/a.jpg"}})"));
        check(one.branding.background == std::vector<std::string>{"resources/a.jpg"}, "background: one string");
        check(contains(one.to_json(), "\"background\": \"resources/a.jpg\""), "one background is written as a string");
        Pack many = Pack::from_json(edited(R"({"branding":{"background":["resources/a.jpg","resources/b.jpg"]}})"));
        check(Pack::from_json(many.to_json()).branding.background.size() == 2, "two backgrounds round-trip");
        Local l = Local::from_json(R"({"patches":{"game.exe":{"b":true,"a":false}},"local_server":false})");
        check(Local::from_json(l.to_json()).to_json() == l.to_json() && contains(l.to_json(), "\"local_server\": false"),
              "local.json round trip (local_server false is kept: it overrides the pack)");
        check(Local{}.to_json() == "{}\n", "an empty local.json");
    }

    // Rust (profiles.rs): names, slugs
    for (const char* ok : {"starfury", "local dev"}) check(profiles::valid_name(ok), std::string("valid name ") + ok);
    for (const char* no : {"..", ".staging", "a/b", "a\\b", "C:x", ""}) check(!profiles::valid_name(no), std::string("invalid name ") + no);
    check(profiles::slug("Starfury community server") == "starfury-community-server", "slug");
    check(profiles::slug("  Local (dev) ") == "local-dev", "slug: spaces and brackets");
    check(profiles::slug("!!!") == "profile", "slug: nothing left");
    check(profiles::slug(std::string(39, 'a') + " b") == std::string(39, 'a'), "slug: cut at 40, no trailing dash");

    // Profile::load + list + free_name + find_pack
    const fs::path root = tmp / "profiles";
    {
        test::write_file(root / "one" / "pack.json", edited(R"({"id":"org.one","name":"One"})"));
        test::write_file(root / "two" / "pack.json", edited(R"({"id":"org.two","version":"bad"})"));
        test::write_file(root / "two" / "local.json", "{}");
        test::write_file(root / ".hidden" / "pack.json", MINIMAL);
        fs::create_directories(root / "not-a-profile");
        auto l = profiles::list(root);
        check(l.size() == 2 && l[0].name == "one" && l[1].name == "two", "list: profiles only, sorted, no dot folders");
        check(l.size() == 2 && l[0].profile && l[0].profile->name == "One", "list: a good profile loads");
        check(l.size() == 2 && !l[1].profile && contains(l[1].error, "version \"bad\"") && l[1].pack && l[1].pack->id == "org.two",
              "list: a broken one has its error, and its pack if that parses");
        check(profiles::find_pack(root, "org.two") == root / "two" && !profiles::find_pack(root, "org.none"), "find_pack");
        check(profiles::free_name(root, "One") == "one-2" && profiles::free_name(root, "Three") == "three", "free_name");
        test::write_file(root / "one" / "local.json", "{\"bogus\": 1}");
        try {
            profiles::load(root / "one");
            check(false, "a bad local.json is an error");
        } catch (const std::exception& e) {
            check(contains(e.what(), "local.json: unknown field `bogus`"), "a bad local.json is an error");
        }
        // local.json's server isn't part of the pack's validation, but load() still refuses it.
        test::write_file(root / "one" / "local.json", R"({"server": "a\" ; quit"})");
        try {
            profiles::load(root / "one");
            check(false, "an unsafe local.json server is refused on load");
        } catch (const Error& e) {
            check(contains(e.what(), "expected host:port"), "an unsafe local.json server is refused on load");
        }
        fs::remove(root / "one" / "local.json");
        try {
            profiles::load(root / "missing");
            check(false, "no pack.json: error");
        } catch (const std::exception& e) {
            check(contains(e.what(), "pack.json: ") && contains(e.what(), "(os error "), "no pack.json: an io error naming the file");
        }
    }

    // write_atomic / write_pack: through a temp file, validated
    {
        Pack p = Pack::from_json(MINIMAL);
        profiles::write_pack(root / "one", p);
        check(test::read_file(root / "one" / "pack.json") == p.to_json() && !fs::exists(root / "one" / ".pack.json.tmp"),
              "write_pack writes, no temp file left");
        p.version = "x";
        bool refused = false;
        try {
            profiles::write_pack(root / "one", p);
        } catch (const Error&) {
            refused = true;
        }
        check(refused && Pack::from_json(test::read_file(root / "one" / "pack.json")).version == "1.0.0",
              "write_pack refuses an invalid pack and keeps the old file");
        Local l;
        l.server = "a.b:1";
        profiles::write_local(root / "one", l);
        check(profiles::read_local(root / "one").server == std::string("a.b:1"), "write_local / read_local");
        check(!profiles::read_local(root / "missing").server, "read_local: missing file = defaults");
    }

    // Rust: moves_profiles
    {
        const fs::path from = tmp / "move/from", to = tmp / "move/to";
        test::write_file(from / "one" / "pack.json", "{}");
        test::write_file(from / "one" / "files" / "a.cfg", "a");
        fs::create_directories(from / "not-a-profile");
        check(profiles::move_profiles(from, to) == 1, "move_profiles: one profile moved");
        check(test::read_file(to / "one" / "files" / "a.cfg") == "a" && !fs::exists(from / "one"), "move_profiles: moved with its files");
        check(fs::exists(from / "not-a-profile"), "move_profiles: only profiles move");
        test::write_file(from / "one" / "pack.json", "{}");
        bool refused = false;
        try {
            profiles::move_profiles(from, to);
        } catch (const std::runtime_error& e) {
            refused = contains(e.what(), "already has a profile called one");
        }
        check(refused, "move_profiles never overwrites a profile");
    }

    // Rust: migrates_profile_toml
    {
        const fs::path dir = test::fresh_dir(tmp / "sf-migrate-x");
        test::write_file(dir / "profile.toml", "name = \"Old\"\nserver = \"a.b:1\"\ntrace_files = true\n[[patch]]\noffset = \"0x10\"\n"
                                               "original = \"90\"\npatched = \"91\"\n");
        try {
            profiles::load(dir);
            check(false, "old inline patches are not dropped silently");
        } catch (const std::runtime_error& e) {
            check(contains(e.what(), "patchset"), "old inline patches are not dropped silently");
        }
        test::write_file(dir / "profile.toml", "name = \"Old\"\nserver = \"a.b:1\"\ntrace_files = true\n");
        Profile p = profiles::load(dir);
        check(p.name == "Old" && p.server == std::string("a.b:1"), "migrated: name and server");
        check(p.local_server, "migrated: the old default local_server = true is kept");
        check(p.trace_files, "migrated: trace_files");
        check(fs::is_regular_file(dir / "profile.toml.migrated"), "migrated: the old file is kept aside");
        check(profiles::read_pack(dir).id == "local.sf-migrate-x", "migrated: id from the folder name");
        const fs::path typo = test::fresh_dir(tmp / "typo");
        test::write_file(typo / "profile.toml", "nmae = \"x\"\n");
        bool refused = false;
        try {
            profiles::load(typo);
        } catch (const std::runtime_error& e) {
            refused = contains(e.what(), "unknown field `nmae`");
        }
        check(refused, "migration: a misspelt key is an error");
    }

    // Data folder and the chosen profiles location
    {
        sf::launch::set_env("STAR_FORGE_DATA_DIR", ustr(tmp / "data"));
        sf::launch::set_env("STAR_FORGE_PROFILES_DIR", std::nullopt);
        check(profiles::data_dir() == tmp / "data", "data_dir: STAR_FORGE_DATA_DIR");
        check(profiles::default_profiles_dir() == tmp / "data" / "profiles", "default_profiles_dir: standard");
        profiles::set_profiles_location(tmp / "elsewhere");
        check(profiles::default_profiles_dir() == tmp / "elsewhere", "default_profiles_dir: the chosen one");
        profiles::set_profiles_location(std::nullopt);
        check(profiles::default_profiles_dir() == tmp / "data" / "profiles", "set_profiles_location(none): back to standard");
        sf::launch::set_env("STAR_FORGE_PROFILES_DIR", ustr(tmp / "env"));
        check(profiles::default_profiles_dir() == tmp / "env", "STAR_FORGE_PROFILES_DIR wins");
        sf::launch::set_env("STAR_FORGE_PROFILES_DIR", std::nullopt);
        sf::launch::set_env("STAR_FORGE_DATA_DIR", std::nullopt);
    }

    // Rust (state.rs): newest_settings_win
    {
        const fs::path shared = tmp / "shared", profile_dir = tmp / "state-profile";
        const std::string rel = kSharedStateFiles[0];
        test::write_file(shared / upath(rel), "shared");
        Profile profile;
        profile.own_game_data = true;
        profiles::sync_shared(profile_dir, profile, shared);
        const fs::path own = profiles::profile_local_appdata(profile_dir) / upath(rel);
        check(test::read_file(own) == "shared", "sync: the shared settings reach a new profile");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        test::write_file(own, "changed");
        profiles::sync_shared(profile_dir, profile, shared);
        check(test::read_file(shared / upath(rel)) == "changed", "sync: changed in the session, copied back");
        test::write_file(shared / upath(rel), "again");
        profiles::sync_shared(profile_dir, Profile{}, shared);
        check(test::read_file(own) == "changed", "sync: profiles sharing the normal folder are left alone");
        profiles::sync_shared(profile_dir, profile, std::nullopt);
        check(test::read_file(own) == "changed", "sync: no shared folder known, nothing done");
    }

    // Real profiles written by the Rust launcher: same bytes back.
    if (argc > 1) {
        int n = 0;
        for (const auto& e : profiles::list(upath(argv[1]))) {
            if (!e.pack) continue;
            ++n;
            const std::string text = test::read_file(e.dir / "pack.json");
            const std::string out = e.pack->to_json();
            check(e.profile.has_value(), "real pack " + e.name + ": loads (" + e.error + ")");
            check(Pack::from_json(out).to_json() == out, "real pack " + e.name + ": writing it is stable");
            // A pack.json the Rust launcher wrote (write_pack) comes out byte for byte the same; one
            // installed as published (the author's own layout) needn't.
            std::printf("  %s: %s\n", e.name.c_str(),
                        out == text ? "byte-identical to the file on disk" : "differs from the file on disk (author's layout)");
        }
        std::printf("checked %d real profile(s)\n", n);
    }
    return test::failures ? 1 : 0;
}
