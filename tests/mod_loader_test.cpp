// Unit test for sf::launch::mod_loader and mod.json (mod/manifest.hpp): a fake game, a profile
// with a pack file, and a handful of mods (folders and zips) that load, conflict, fail and need
// each other.

#include <miniz.h>

#include "check.hpp"
#include "launch/mod_loader.hpp"
#include "launch/profiles.hpp"
#include "mod/manifest.hpp"

namespace fs = std::filesystem;
namespace ml = sf::launch::mod_loader;
using test::check;
using test::contains;

namespace {

void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    test::write_file(p, text);
}

void write_zip(const fs::path& path, const std::vector<std::pair<std::string, std::string>>& entries) {
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    mz_zip_writer_init_heap(&zip, 0, 0);
    for (const auto& [name, body] : entries)
        mz_zip_writer_add_mem(&zip, name.c_str(), body.data(), body.size(), MZ_DEFAULT_COMPRESSION);
    void* buf = nullptr;
    size_t size = 0;
    mz_zip_writer_finalize_heap_archive(&zip, &buf, &size);
    fs::create_directories(path.parent_path());
    test::write_file(path, std::string(static_cast<char*>(buf), size));
    mz_free(buf);
    mz_zip_writer_end(&zip);
}

std::string manifest(const std::string& id, const std::string& extra = "") {
    return R"({"format":1,"id":")" + id + R"(","version":"1.0.0")" + extra + "}";
}

const ml::Mod* find(const ml::Result& r, const std::string& id) {
    for (const auto& m : r.mods)
        if (m.id == id) return &m;
    return nullptr;
}

bool has_conflict(const ml::Result& r, const std::string& what_part, const std::vector<std::string>& mods) {
    for (const auto& c : r.conflicts)
        if (contains(c.what, what_part) && c.mods == mods) return true;
    return false;
}

void manifest_tests() {
    auto m = sf::mod::Manifest::from_json(
        R"({"format":1,"id":"retro-layout","version":"1.2.3","needs":["loose-before-packed"],
            "requires":{"retro-icons":">=1.0"},"scripts":[{"file":"ui\\mods\\a.lua","after":"ui\\x.lua"}],
            "lua":{"replaces":["UI.A:B"]}})");
    check(m.name == "retro-layout" && m.scripts.size() == 1 && m.scripts[0].type == "SCRIPT" && m.lua_replaces.size() == 1,
          "mod.json: fields read, name defaults to id, script type defaults to SCRIPT");
    auto err = [](const std::string& text) {
        try {
            sf::mod::Manifest::from_json(text);
        } catch (const std::exception& e) {
            return std::string(e.what());
        }
        return std::string();
    };
    check(contains(err(R"({"format":1,"id":"a","colour":1})"), "unknown field `colour`"), "mod.json: unknown field refused");
    check(contains(err(R"({"format":1,"id":"Bad Id"})"), "lowercase"), "mod.json: bad id refused");
    check(contains(err(R"({"format":2,"id":"a"})"), "update the launcher"), "mod.json: newer format refused");
    check(contains(err(R"({"format":1,"id":"a","game":["abc"]})"), "SHA-256"), "mod.json: bad game hash refused");
    check(contains(err(R"({"format":1,"id":"sfmod"})"), "launcher's own"), "mod.json: id sfmod is reserved");
    check(sf::mod::valid_id("deck-glyphs") && sf::mod::valid_id("a.b_c") && !sf::mod::valid_id("-a") &&
              !sf::mod::valid_id("a--b") && !sf::mod::valid_id("A"),
          "valid_id");
    check(sf::mod::version_matches("1.2.0", ">=1.0") && sf::mod::version_matches("1.2.0", "*") &&
              !sf::mod::version_matches("0.9.0", ">=1.0.0") && sf::mod::version_matches("1.2.0", ">=1.0.0 <2.0.0") &&
              !sf::mod::version_matches("2.0.0", ">=1.0.0 <2.0.0") && sf::mod::version_matches("1.0.0", "1.0.0"),
          "version_matches");
}

}  // namespace

int main() {
    test::exit_on_crash();
    manifest_tests();

    const fs::path base = test::fresh_dir(fs::temp_directory_path() / "sf-mod-loader-test");
    const fs::path game = base / "game", prof = base / "profile", mods = prof / "mods";

    // The game: a resource list and a texture list.
    write(game / "data/ui/uiresources.txt",
          "\xEF\xBB\xBFItem\r\n{\r\n\tfileName \"ui\\scripts\\work\\uigamefuncs.lua\"\r\n\tresourceType \"SCRIPT\"\r\n"
          "\tloadingGroups \"FIRST_LEVEL\"\r\n}\r\n");
    write(game / "data/ui/textures/common/icons.txt",
          "Texture\n{\n\tname \"I1\"\n\trect \"0 0 1 1\"\n}\nTexture\n{\n\tname \"I2\"\n\trect \"0 0 2 2\"\n}\n");
    // The pack ships its own copy of the icon list: patches must start from that one.
    write(prof / "files/data/ui/textures/common/icons.txt",
          "Texture\n{\n\tname \"I1\"\n\trect \"9 9 1 1\"\n}\nTexture\n{\n\tname \"I2\"\n\trect \"9 9 2 2\"\n}\n");
    write(prof / "files/data/ui/pack.dds", "pack");

    // No mods/ folder: nothing happens.
    auto r0 = ml::build(prof, game, nullptr);
    check(r0.mods.empty() && !fs::exists(mods), "no mods folder: nothing loaded, nothing written");

    // a: a folder mod with a script, a patch and a file.
    write(mods / "a/mod.json", manifest("a", R"(,"needs":["loose-before-packed"],
          "scripts":[{"file":"ui\\mods\\a\\a.txt","type":"TEXTURE","after":"ui\\scripts\\work\\uigamefuncs.lua"},
                     {"file":"ui\\mods\\a\\a.lua","after":"ui\\scripts\\work\\uigamefuncs.lua"}],
          "lua":{"replaces":["UI.Wnd:Update"]})"));
    write(mods / "a/files/data/ui/mods/a/a.lua", "-- a");
    write(mods / "a/files/data/ui/shared.dds", "a");
    write(mods / "a/patches/icons.sfpatch",
          "patch \"data/ui/textures/common/icons.txt\"\n{\n\tset Texture[name=\"I1\"] { rect \"1 1 1 1\" }\n}\n");
    // b: a zip (in a top folder) changing the same icon, another one, and the same file.
    write_zip(mods / "b-1.0.0.zip",
              {{"b/mod.json", manifest("b", R"(,"lua":{"replaces":["UI.Wnd:Update"]})")},
               {"b/files/data/ui/shared.dds", "b"},
               {"b/files/data/ui/pack.dds", "b"},
               {"b/patches/icons.sfpatch",
                "patch \"data/ui/textures/common/icons.txt\"\n{\n\tset Texture[name=\"I1\"] { rect \"2 2 2 2\" }\n"
                "\tset Texture[name=\"I2\"] { hidden 1 }\n}\n"},
               {"b/patches/broken.sfpatch",
                "patch \"data/ui/textures/common/icons.txt\"\n{\n\tset Texture[name=\"I2\"] { rect \"5\" }\n"
                "\tremove Texture[name=\"Nope\"]\n}\n"}});
    // c needs a mod that isn't there; d needs c (so drops out too); e has a broken mod.json;
    // f is turned off; g needs a core patch that doesn't exist; h changes server data.
    write(mods / "c/mod.json", manifest("c", R"(,"requires":{"zzz":"*"})"));
    write(mods / "d/mod.json", manifest("d", R"(,"requires":{"c":">=1.0"})"));
    write(mods / "e/mod.json", "{\"format\":1}");
    write(mods / "f/mod.json", manifest("f"));
    write(mods / "f/files/data/ui/f.txt", "f");
    write(mods / "g/mod.json", manifest("g", R"(,"needs":["no-such-patch"])"));
    write(mods / "h/mod.json", manifest("h", R"(,"server":true)"));
    // i needs a newer Lua API than this launcher has; j's range is met.
    write(mods / "i/mod.json", manifest("i", R"(,"requires":{"sfmod":">=2"})"));
    write(mods / "j/mod.json", manifest("j", R"(,"requires":{"sfmod":">=1"})"));
    write(mods / "notes.txt", "a stray file, not a mod");
    // The player puts b before a; f is off.
    sf::pack::Local local;
    local.mods.order = {"b", "a"};
    local.mods.off = {"f"};
    sf::launch::profiles::write_local(prof, local);

    auto r = ml::build(prof, game, nullptr);
    std::vector<std::string> ids;
    for (const auto& m : r.mods) ids.push_back(m.id);
    check((ids == std::vector<std::string>{"b", "a", "c", "d", "b-1.0.0.zip", "e", "f", "g", "h", "i", "j"}) ||
              (ids == std::vector<std::string>{"b", "a", "c", "d", "e", "f", "g", "h", "i", "j"}),
          "load order: the player's, then the rest by id");
    check(find(r, "a") && find(r, "a")->loaded && find(r, "b") && find(r, "b")->loaded, "a and b load");
    check(find(r, "c") && !find(r, "c")->loaded && contains(find(r, "c")->reason, "needs mod \"zzz\""), "c: missing requirement");
    check(find(r, "d") && !find(r, "d")->loaded && contains(find(r, "d")->reason, "needs mod \"c\""),
          "d: its requirement didn't load");
    check(find(r, "f") && !find(r, "f")->loaded && find(r, "f")->reason == "turned off", "f: turned off");
    check(find(r, "g") && contains(find(r, "g")->reason, "no-such-patch"), "g: unknown core patch");
    check(find(r, "h") && contains(find(r, "h")->reason, "server"), "h: server mods aren't loaded");
    check(find(r, "i") && !find(r, "i")->loaded && contains(find(r, "i")->reason, "Lua API (sfmod) >=2"),
          "i: needs a newer sfmod");
    check(find(r, "j") && find(r, "j")->loaded, "j: sfmod requirement met");
    bool e_reported = false;
    for (const auto& m : r.mods)
        if (contains(m.reason, "missing field `id`")) e_reported = true;
    check(e_reported, "e: broken mod.json reported");

    // Merged icon list: from the pack's copy, b first then a (a wins rect), b's hidden kept.
    std::string icons = test::read_file(mods / ".merged/data/ui/textures/common/icons.txt");
    check(contains(icons, "name \"I1\"\n\trect \"1 1 1 1\"") && contains(icons, "rect \"9 9 2 2\"\n\thidden 1"),
          "merged: pack copy as base, patches in load order");
    check(has_conflict(r, "icons.txt: Texture[name=\"I1\"]#rect", {"b", "a"}), "conflict: same key, a wins");
    check(!has_conflict(r, "I2", {"b", "a"}), "no conflict where only one mod changes a block");
    bool broken_noted = false;
    for (const auto& n : find(r, "b")->notes) broken_noted |= contains(n, "patches/broken.sfpatch left out") && contains(n, "Nope");
    check(broken_noted && !contains(icons, "rect \"5\""), "a failing .sfpatch is left out whole (its first op too)");
    // Script registered right after uigamefuncs.lua, CRLF kept.
    std::string res = test::read_file(mods / ".merged/data/ui/uiresources.txt");
    check(contains(res, "uigamefuncs.lua\"\r\n\tresourceType \"SCRIPT\"\r\n\tloadingGroups \"FIRST_LEVEL\"\r\n}\r\n"
                        "Item\r\n{\r\n\tfileName \"ui\\mods\\a\\a.txt\"\r\n"),
          "scripts: registered after the named script");
    check(contains(res, "a\\a.lua\"\r\n\tresourceType \"SCRIPT\"\r\n\tloadingGroups \"FIRST_LEVEL\""),
          "scripts: loading group from the script they come after");
    check(res.rfind("\xEF\xBB\xBFItem\r\n{\r\n\tfileName \"ui\\sfmod\\sfmod.lua\"\r\n\tresourceType \"SCRIPT\"\r\n"
                    "\tloadingGroups \"APP_START\"\r\n}\r\n\r\nItem", 0) == 0,
          "sfmod.lua registered first, after the BOM");
    check(contains(test::read_file(mods / ".merged/data/ui/sfmod/sfmod.lua"), "SF.version = VERSION"),
          "sfmod.lua served from .merged");
    check(res.find("a\\a.txt") < res.find("a\\a.lua"), "scripts with the same anchor keep their order");
    // Whole files and Lua.
    check(has_conflict(r, "file data/ui/shared.dds", {"b", "a"}), "conflict: same file in two mods");
    check(has_conflict(r, "lua UI.Wnd:Update", {"b", "a"}), "conflict: same Lua function replaced");
    bool pack_noted = false;
    for (const auto& n : find(r, "b")->notes) pack_noted |= contains(n, "replaces the pack's data/ui/pack.dds");
    check(pack_noted, "a mod replacing a pack file is noted");
    check(r.core_patches == std::vector<std::string>{"loose-before-packed"}, "core patches from needs");

    // Layers: merged, then a (loaded last), then b's cache.
    check(r.layers.size() == 3 && r.layers[0] == "mods/.merged" && r.layers[1] == "mods/a/files" &&
              r.layers[2].rfind("mods/.cache/b-", 0) == 0 && contains(r.layers[2], "/files"),
          "layers: merged, then mods last-loaded first");
    check(fs::exists(prof / r.layers[2] / "data/ui/shared.dds"), "zip unpacked into the cache");
    std::string layers = test::read_file(mods / ".layers");
    check(contains(layers, "fingerprint " + r.fingerprint + "\n") && contains(layers, "mod b 1.0.0\nmod a 1.0.0\n") &&
              contains(layers, "core loose-before-packed\n") && contains(layers, "layer mods/.merged\nlayer mods/a/files\n"),
          ".layers written for the agent");
    check(r.fingerprint.size() == 12, "fingerprint");

    // The report reads back.
    auto again = ml::last(prof);
    check(again && again->fingerprint == r.fingerprint && again->mods.size() == r.mods.size() &&
              again->conflicts.size() == r.conflicts.size() && again->notes == r.notes,
          ".loaded.json round trip");

    // Same input, same output; swapping the order swaps the winner.
    check(ml::build(prof, game, nullptr).fingerprint == r.fingerprint, "deterministic");
    ml::set_order(prof, {"a", "b"});
    auto swapped = ml::build(prof, game, nullptr);
    check(swapped.fingerprint != r.fingerprint && has_conflict(swapped, "Texture[name=\"I1\"]#rect", {"a", "b"}) &&
              contains(test::read_file(mods / ".merged/data/ui/textures/common/icons.txt"), "rect \"2 2 2 2\""),
          "order a, b: b wins");

    // Switches.
    ml::set_on(prof, "f", true);
    ml::set_on(prof, "b", false);
    auto sw = ml::build(prof, game, nullptr);
    check(find(sw, "f")->loaded && !find(sw, "b")->loaded, "set_on: f on, b off");
    check(sf::launch::profiles::read_local(prof).mods.off == std::vector<std::string>{"b"}, "set_on saved in local.json");
    bool cache_kept = false;
    for (const auto& e : fs::directory_iterator(mods / ".cache")) cache_kept |= e.path().filename().string().rfind("b-", 0) == 0;
    check(cache_kept, "an off mod's unpacked copy is kept");

    // Removing the zip removes its cache; nothing loaded removes .layers.
    fs::remove(mods / "b-1.0.0.zip");
    for (const char* id : {"a", "f", "j"}) ml::set_on(prof, id, false);
    auto none = ml::build(prof, game, nullptr);
    check(!fs::exists(mods / ".layers"), "nothing loaded: no .layers");
    check(fs::is_empty(mods / ".cache"), "a removed zip's cache is cleaned up");
    check(!fs::exists(mods / ".merged"), "nothing merged: no .merged");
    (void)none;

    // The game build check (only asked when a mod lists builds).
    ml::set_on(prof, "a", true);
    write(mods / "a/mod.json", manifest("a", R"(,"game":[")" + std::string(64, 'a') + R"("])"));
    int asked = 0;
    auto gb = ml::build(prof, game, [&] {
        ++asked;
        return std::string(64, 'b');
    });
    check(asked == 1 && !find(gb, "a")->loaded && contains(find(gb, "a")->reason, "different game build"),
          "game build mismatch");

    fs::remove_all(base);
    return test::failures ? 1 : 0;
}
