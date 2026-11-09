// Tests sf::launch::prepare: the steps before a launch, in order, against a fake game folder, a
// local HTTP server for the pack's update address and file list, and a profile on disk.

#include <cstring>

#include "crypto/sha256.hpp"
#include "launch/env.hpp"
#include "launch/net.hpp"
#include "launch/prepare.hpp"
#include "launch/profiles.hpp"
#include "patch/sha1.hpp"

#include "check.hpp"
#include "http_server.hpp"

namespace fs = std::filesystem;
using namespace sf::launch;
using prepare::Event;
using test::check;
using test::contains;

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

// patch_test's minimal PE32+ (see there).
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
    return d;
}

// What prepare() reported, one line per event.
std::vector<std::string> g_events;
std::optional<std::string> record(const Event& e) {
    switch (e.kind) {
    case Event::Note: g_events.push_back("note: " + e.text); break;
    case Event::Update: g_events.push_back("update: " + e.update->version + (e.update->required ? " required" : "")); break;
    case Event::Sync:
        if (e.sync->kind == remote::SyncEvent::Plan) g_events.push_back("sync plan " + std::to_string(e.sync->files));
        break;
    case Event::SyncDone: g_events.push_back("sync done" + (e.sync_error ? ": " + *e.sync_error : std::string())); break;
    }
    return std::nullopt;
}

std::string joined() {
    std::string out;
    for (const auto& s : g_events) out += (out.empty() ? "" : " | ") + s;
    return out;
}

}  // namespace

int main() {
    test::exit_on_crash();
    const fs::path base = test::fresh_dir(fs::temp_directory_path() / "sf-prepare-test");
    set_env(env::kDataDir, ustr(base / "data"));

    // A game, the tools, and (on Linux) a Proton to start them with.
    const std::string game = pe();
    test::write_file(base / "game/win64/game.exe", game);
    test::write_file(base / "tools/sc-inject.exe", "MZ");
    test::write_file(base / "tools/sc_agent.dll", "MZ");
    Setup setup{base / "game", base / "tools", std::nullopt};
#ifndef _WIN32
    setup.proton = Proton{base / "proton", base / "runtime/run", base / "compat", base / "steam"};
#endif

    test::FileServer srv;
    const std::string u = srv.base();
    srv.put("/a.cfg", "listed");
    srv.put("/list.txt", "SCMOD1\ndata/a.cfg\t" + sha("listed") + "\t" + u + "/a.cfg\n");
    const std::string head = R"({"format":1,"id":"org.example.prep","name":"Prep","version":"1.0.0")";
    srv.put("/pack.json", R"({"format":1,"id":"org.example.prep","name":"Prep","version":"1.1.0"})");

    const fs::path dir = base / "profiles/prep";
    const std::string set = "format = 1\nexe = \"game.exe\"\nsha1 = \"" + sf::patch::sha1_hex(game.data(), game.size()) +
                            "\"\nimage_base = 0x140000000\n\n[[patch]]\nid = \"one\"\n[[patch.write]]\nva = 0x140001002\n"
                            "orig = \"74 05\"\nbytes = \"90 90\"\n\n[[patch]]\nid = \"two\"\noptional = true\n[[patch.write]]\n"
                            "va = 0x140001006\norig = \"ffff\"\nbytes = \"1111\"\n";
    test::write_file(dir / "patches/game.exe.patchset.toml", set);
    test::write_file(dir / "pack.json", head + R"(,"requires":{"game":[")" + std::string(64, 'a') + R"("]},"update":{"url":")" + u +
                                            R"(/pack.json"},"files_from":{"scmod1":")" + u + R"(/list.txt"}})");

    g_events.clear();
    prepare::Prepared p = prepare::prepare(setup, dir, {"-x"}, record);
    check(joined().rfind("note: Prep was made for a different game build; its files and patches may not work with this one. | "
                         "update: 1.1.0 | note: Prep 1.1.0 is available. | sync plan 1 | sync done | "
                         "note: Prep: updated 1 game file(s) from " + net::host(u) + " | note: game.exe: optional patch skipped: ",
                         0) == 0 &&
              g_events.size() == 7,
          "events, in order: " + joined());
    check(test::read_file(dir / "files/data/a.cfg") == "listed", "the listed file arrived");
    check(p.trust.kind == trust::Trust::Unsigned, "unsigned");
    check(p.profile.name == "Prep", "the profile");
    check(!p.command.args.empty() && p.command.args.back() == "-x", "the command ends with the game args");
#ifndef _WIN32
    check(p.command.program == base / "runtime/run", "started through the runtime");
#else
    check(p.command.program == base / "tools/sc-inject.exe", "started as sc-inject.exe");
#endif

    // The server no longer takes this version.
    srv.put("/pack.json", R"({"format":1,"id":"org.example.prep","name":"Prep","version":"1.1.0","update":{"url":"https://x.org/p","min_version":"1.1.0"}})");
    g_events.clear();
    check(error_of([&] { prepare::prepare(setup, dir, {}, record); }) ==
              "Prep's server needs pack version 1.1.0 or newer (you have 1.0.0). Update the pack first.",
          "a required update stops the launch");
    check(joined().find("update: 1.1.0 required") != std::string::npos, "... after saying so");

    // The update address is down: a note, and on.
    srv.put("/pack.json", "not json");
    g_events.clear();
    check(error_of([&] { prepare::prepare(setup, dir, {}, record); }).empty(), "an update check that fails doesn't stop it");
    check(contains(joined(), "note: Could not check Prep for updates: " + u + "/pack.json: "), "... it's a note: " + joined());

    // A cancelled download stops it, and says how the sync ended.
    srv.put("/pack.json", R"({"format":1,"id":"org.example.prep","name":"Prep","version":"1.0.0"})");
    srv.put("/b.cfg", "bee");
    srv.put("/list.txt", "SCMOD1\ndata/a.cfg\t" + sha("listed") + "\t" + u + "/a.cfg\ndata/b.cfg\t" + sha("bee") + "\t" + u + "/b.cfg\n");
    g_events.clear();
    std::string err = error_of([&] {
        prepare::prepare(setup, dir, {}, [](const Event& e) -> std::optional<std::string> {
            record(e);
            if (e.kind == Event::Sync && e.sync->kind == remote::SyncEvent::Bytes) return std::string("cancelled");
            return std::nullopt;
        });
    });
    check(err == "cancelled", "a cancel stops it: " + err);
    check(contains(joined(), "sync done: cancelled"), "... after SyncDone with the error");

    // Patches that don't fit the game stop it (on_error exit).
    test::write_file(dir / "patches/game.exe.patchset.toml", set.substr(0, set.find("sha1 = \"") + 8) + std::string(40, '0') +
                                                                 set.substr(set.find("\"\nimage_base")));
    err = error_of([&] { prepare::prepare(setup, dir, {}, prepare::Events([](const Event&) { return std::optional<std::string>(); })); });
    check(contains(err, "The pack's game patches don't match this game: game.exe is a different build"), "patches for another build: " + err);

    // A damaged signed pack stops it first.
    test::write_file(dir / "manifest.json", "{}");
    err = error_of([&] { prepare::prepare(setup, dir, {}, record); });
    check(err == "Prep: the pack has only one of manifest.json and manifest.sig. Reinstall the pack to repair it.", "a damaged pack: " + err);

    std::printf("%s (%d failed)\n", test::failures ? "FAILED" : "ok", test::failures);
    return test::failures ? 1 : 0;
}
