// Tests sf::launch::mods: the patch toggle list for a client profile and a server install, and
// switching patches on/off in local.json and patcher.toml. Also core::requested, which decides
// which core patches the agent and launcher load.

#include "launch/mods.hpp"
#include "launch/servers.hpp"
#include "launch/patching.hpp"
#include "launch/profiles.hpp"
#include "patch/core.hpp"


#include "check.hpp"

namespace fs = std::filesystem;
using namespace sf::launch;
using test::check;
using test::contains;

namespace {

const char* kCore = "loose-before-packed";  // the one core patch today
// A stand-in proxy build: what install_proxy looks for is the sfp_version export's name.
const std::string kProxyBuild("MZ proxy sfp_version\0 v1", 24);

template <class F>
std::string error_of(F f) {
    try {
        f();
        return "";
    } catch (const std::exception& e) {
        return e.what();
    }
}

// A pack's own game.exe patchset: "one" (on), "two" (off), overlapping at 0x140001003.
const std::string kGameSet = R"(
format = 1
exe = "game.exe"
sha1 = "0000000000000000000000000000000000000000"
build = "1.2.3"
image_base = 0x140000000

[[patch]]
id = "one"
title = "Nop a check"
[[patch.write]]
va = 0x140001002
orig = "74 05"
bytes = "90 90"

[[patch]]
id = "two"
default = false
optional = true
[[patch.write]]
va = 0x140001003
orig = "05 aa"
bytes = "90 90"
)";

// A server exe's patchset: "a" (on), "b" (on), no overlap.
const std::string kShardSet = R"(
format = 1
exe = "Shard.exe"
sha1 = "0000000000000000000000000000000000000000"
image_base = 0x400000

[[patch]]
id = "a"
[[patch.write]]
va = 0x401000
orig = "74"
bytes = "eb"

[[patch]]
id = "b"
[[patch.write]]
va = 0x402000
orig = "75"
bytes = "eb"
)";

const mods::Toggle* get(const mods::List& l, const std::string& module, const std::string& id) {
    for (const auto& t : l.toggles)
        if (t.module == module && t.id == id) return &t;
    return nullptr;
}

mods::List client(const fs::path& dir) { return mods::client(dir, profiles::load(dir)); }

}  // namespace

int main() {
    test::exit_on_crash();
    const fs::path tmp = test::fresh_dir(fs::temp_directory_path() / "sf_mods_test");

    // core::requested: the pack's list, plus core patches switched on by the player.
    {
        using sf::patch::core::requested;
        check(requested({}, {}).empty(), "requested: nothing asked, nothing switched on");
        check(requested({kCore}, {}) == std::vector<std::string>{kCore}, "requested: the pack's list");
        check(requested({}, {{"GAME.EXE", {{kCore, true}}}}) == std::vector<std::string>{kCore},
              "requested: switched on in local.json (module ignoring case)");
        check(requested({}, {{"game.exe", {{kCore, false}}}}).empty(), "requested: switched off is not loaded");
        check(requested({kCore}, {{"game.exe", {{kCore, true}}}}).size() == 1, "requested: no duplicates");
        check(requested({}, {{"Shard.exe", {{kCore, true}}}}).empty(), "requested: wrong module is ignored");
    }

    // Client: a profile with a pack that asks for no core patch and has its own patchset.
    const fs::path profile = tmp / "profile";
    test::write_file(profile / "pack.json", R"({ "format": 1, "id": "org.example.test", "name": "T", "version": "1.0.0" })");
    test::write_file(profile / "patches" / "game.exe.patchset.toml", kGameSet);
    {
        mods::List l = client(profile);
        check(l.errors.empty() && l.warnings.empty(), "client: no errors or warnings");
        check(l.toggles.size() == 3, "client: core patch + the pack's two");
        const mods::Toggle* core = get(l, "game.exe", kCore);
        check(core && core->source == "core" && !core->on && !core->default_on && !core->chosen,
              "client: a core patch the pack doesn't ask for is listed, off");
        const mods::Toggle* one = get(l, "game.exe", "one");
        const mods::Toggle* two = get(l, "game.exe", "two");
        check(one && one->on && one->default_on && one->source == "patches/game.exe.patchset.toml" && one->build == "1.2.3",
              "client: the pack's patch, on by default, with its source and build");
        check(two && !two->on && two->optional, "client: default = false is off; optional shown");
        check(one && one->conflicts == std::vector<std::string>{"game.exe:two"} && two &&
                  two->conflicts == std::vector<std::string>{"game.exe:one"},
              "client: overlapping patches name each other as conflicts");
        check(core && core->conflicts.empty(), "client: the core patch conflicts with neither");
        check(patching::summary(profile, profiles::load(profile)).patches == 1, "summary: only \"one\" is on");
    }

    // Switching the core patch on loads it in the launcher (and the agent, same rule).
    mods::set_client(profile, "GAME.EXE", kCore, true);
    {
        check(contains(test::read_file(profile / "local.json"), kCore), "set_client: written to local.json");
        mods::List l = client(profile);
        const mods::Toggle* core = get(l, "game.exe", kCore);
        check(core && core->on && core->chosen == true && !core->default_on, "set_client: core patch on by the player");
        check(patching::summary(profile, profiles::load(profile)).patches == 2, "summary: core patch now counted");
    }
    mods::set_client(profile, "game.exe", "one", false);
    mods::set_client(profile, "game.exe", kCore, std::nullopt);
    {
        mods::List l = client(profile);
        check(!get(l, "game.exe", "one")->on && get(l, "game.exe", "one")->chosen == false, "set_client: pack patch off");
        check(!get(l, "game.exe", kCore)->on && !get(l, "game.exe", kCore)->chosen, "set_client: back to default");
        sf::pack::Local local = profiles::read_local(profile);
        check(local.patches.size() == 1 && local.patches["game.exe"].size() == 1, "set_client: cleared entries are removed");
    }
    mods::set_client(profile, "game.exe", "one", std::nullopt);
    check(profiles::read_local(profile).patches.empty(), "set_client: last switch cleared leaves no patches map");
    check(contains(error_of([&] { mods::set_client(profile, "game.exe", "nope", true); }), "has no patch \"nope\""),
          "set_client: unknown patch refused");
    check(contains(error_of([&] { mods::set_client(profile, "Shard.exe", "one", true); }), "for Shard.exe"),
          "set_client: patch of another module refused");

    // A pack that asks for the core patch: on by default; the player can still switch it off.
    const fs::path asks = tmp / "asks";
    test::write_file(asks / "pack.json", std::string(R"({ "format": 1, "id": "org.example.asks", "name": "A", "version": "1.0.0",
        "patches": { "core": [")") + kCore + R"("] } })");
    {
        check(get(client(asks), "game.exe", kCore)->default_on, "client: core patch the pack asks for is on by default");
        mods::set_client(asks, "game.exe", kCore, false);
        check(!get(client(asks), "game.exe", kCore)->on, "client: ... and can be switched off");
        check(patching::summary(asks, profiles::load(asks)).patches == 0, "summary: switched-off core patch not counted");
    }

    // A switch naming a patch that doesn't exist is a warning.
    test::write_file(asks / "local.json", R"({ "patches": { "game.exe": { "gone": true } } })");
    {
        mods::List l = client(asks);
        check(l.warnings.size() == 1 && contains(l.warnings[0], "no patch called \"gone\""), "client: stale switch warns");
    }

    // Server: patchsets in cloud/starforge/patchsets, switches in patcher.toml.
    const fs::path cloud = tmp / "cloud";
    test::write_file(cloud / "starforge" / "patchsets" / "Shard.exe.patchset.toml", kShardSet);
    test::write_file(cloud / "starforge" / "patchsets" / "game.exe.patchset.toml", kGameSet);
    test::write_file(cloud / "starforge" / "patchsets" / "Broken.exe.patchset.toml", "format = 1 =");
    {
        mods::List l = mods::server(cloud);
        check(l.errors.size() == 1 && contains(l.errors[0], "Broken.exe.patchset.toml"), "server: a broken patchset is an error");
        check(l.toggles.size() == 4, "server: every patch of the readable sets");
        check(l.toggles.front().module == "game.exe" && l.toggles.back().module == "Shard.exe", "server: sorted by module");
        const mods::Toggle* a = get(l, "Shard.exe", "a");
        check(a && a->on && a->source == "starforge/patchsets/Shard.exe.patchset.toml", "server: on by default, with source");
    }
    mods::set_server(cloud, "shard.exe", "a", false);
    {
        const std::string text = test::read_file(cloud / "starforge" / "patcher.toml");
        check(contains(text, "[patches.\"Shard.exe\"]\n\"a\" = false"), "set_server: written under the exe's name");
        check(contains(text, "on_error = \"exit\""), "set_server: on_error kept at its default");
        check(!get(mods::server(cloud), "Shard.exe", "a")->on, "set_server: off now");
    }
    mods::set_server(cloud, "Shard.exe", "a", std::nullopt);
    check(mods::read_server_config(cloud).patches.empty(), "set_server: cleared");
    check(contains(error_of([&] { mods::set_server(cloud, "Shard.exe", "zzz", true); }), "this server has no patch"),
          "set_server: unknown patch refused");

    // clashes_on: only when both are on.
    {
        mods::List l = client(profile);
        check(mods::clashes_on(l, "game.exe", "one").empty(), "clashes_on: \"two\" is off, no clash");
        mods::set_client(profile, "game.exe", "two", true);
        l = client(profile);
        check(mods::clashes_on(l, "GAME.EXE", "two") == std::vector<std::string>{"game.exe:one"}, "clashes_on: both on");
        mods::set_client(profile, "game.exe", "two", std::nullopt);
        check(!mods::find(l, "game.exe", "zzz"), "find: unknown is null");
    }

    // server_status: the proxy DLL's state and each exe checked against its patchset.
    {
        const fs::path s = tmp / "status";
        auto proxy = [&] { return mods::server_status(s).proxy; };
        fs::create_directories(s);
        check(proxy() == mods::ServerStatus::NoDll, "status: no DLL");
        test::write_file(s / "libcrypto-1_1_tg.dll", "stock");
        check(proxy() == mods::ServerStatus::Stock, "status: the game's own DLL only");
        test::write_file(s / "LIBCRYPTO-1_1_TG_ORIG.DLL", "stock");
        check(proxy() == mods::ServerStatus::Restored, "status: proxy overwritten by the original (name ignoring case)");
        test::write_file(s / "libcrypto-1_1_tg.dll", kProxyBuild);
        check(proxy() == mods::ServerStatus::Installed, "status: proxy installed");
        test::write_file(s / "libcrypto-1_1_tg.dll", "stock v2");
        check(proxy() == mods::ServerStatus::Restored, "status: a game update's DLL over the proxy");
        test::write_file(s / "libcrypto-1_1_tg.dll", kProxyBuild);
        fs::remove(s / "LIBCRYPTO-1_1_TG_ORIG.DLL");
        check(proxy() == mods::ServerStatus::Broken, "status: the proxy without the original");
        test::write_file(s / "libcrypto-1_1_tg.dll", "some other dll");
        check(proxy() == mods::ServerStatus::Stock, "status: without the original it's just some DLL");

        test::write_file(s / "starforge" / "patchsets" / "Shard.exe.patchset.toml", kShardSet);
        mods::ServerStatus st = mods::server_status(s);
        check(st.exes.size() == 1 && !st.exes[0].sha1 && st.exes[0].report.errors.size() == 1 &&
                  contains(st.exes[0].report.errors[0], "Shard.exe is not in"),
              "status: a patchset's exe missing is a problem");
        test::write_file(s / "Shard.exe", "not this build");
        st = mods::server_status(s);
        check(st.exes.size() == 1 && st.exes[0].sha1 && contains(st.exes[0].report.errors[0], "different build"),
              "status: a different build is a problem");
        check(st.config && st.config->exit_on_error && st.errors.empty(), "status: default config, no errors");
        test::write_file(s / "starforge" / "patcher.toml", "on_error = 3");
        st = mods::server_status(s);
        check(!st.config && st.errors.size() == 1 && contains(st.errors[0], "on_error"), "status: broken config is an error");
    }

    // install_proxy / uninstall_proxy: the game's DLL kept as the original, put back on uninstall.
    {
        const fs::path s = tmp / "install", build = tmp / "proxy.dll";
        auto proxy = [&] { return mods::server_status(s).proxy; };
        auto dll = [&] { return test::read_file(s / "libcrypto-1_1_tg.dll"); };
        test::write_file(build, kProxyBuild);
        fs::create_directories(s);
        check(contains(error_of([&] { mods::install_proxy(s, build); }), "is this the game's cloud folder"),
              "install: no DLL is refused");
        test::write_file(s / "libcrypto-1_1_tg.dll", "stock");
        test::write_file(tmp / "not-proxy.dll", "MZ something else");
        check(contains(error_of([&] { mods::install_proxy(s, tmp / "not-proxy.dll"); }), "not a STAR FORGE server proxy"),
              "install: only a proxy build");
        check(dll() == "stock" && proxy() == mods::ServerStatus::Stock, "install: a refused install changes nothing");

        check(mods::install_proxy(s, build) == mods::ServerStatus::Stock, "install: from stock");
        check(proxy() == mods::ServerStatus::Installed && dll() == kProxyBuild &&
                  test::read_file(s / "libcrypto-1_1_tg_orig.dll") == "stock",
              "install: proxy in, original kept");
        check(fs::is_directory(s / "starforge" / "patchsets") && fs::is_directory(s / "starforge" / "logs") &&
                  mods::read_server_config(s).exit_on_error,
              "install: starforge/ folders and a default patcher.toml");
        test::write_file(s / "starforge" / "patcher.toml", "on_error = \"continue\"\n");
        const std::string v2 = kProxyBuild + " v2";
        test::write_file(build, v2);
        check(mods::install_proxy(s, build) == mods::ServerStatus::Installed && dll() == v2 &&
                  test::read_file(s / "libcrypto-1_1_tg_orig.dll") == "stock",
              "install: again updates the proxy only");
        check(!mods::read_server_config(s).exit_on_error, "install: an existing patcher.toml is kept");

        test::write_file(s / "libcrypto-1_1_tg.dll", "stock v2");  // Steam updated the game
        check(mods::install_proxy(s, build) == mods::ServerStatus::Restored && dll() == v2 &&
                  test::read_file(s / "libcrypto-1_1_tg_orig.dll") == "stock v2",
              "install: after a Steam verify/update, the new DLL becomes the original");

        mods::uninstall_proxy(s);
        check(proxy() == mods::ServerStatus::Stock && dll() == "stock v2" && !fs::exists(s / "libcrypto-1_1_tg_orig.dll") &&
                  fs::exists(s / "starforge" / "patcher.toml"),
              "uninstall: the original back, starforge/ kept");
        check(contains(error_of([&] { mods::uninstall_proxy(s); }), "isn't installed"), "uninstall: twice is refused");
        test::write_file(s / "libcrypto-1_1_tg_orig.dll", "stock");
        mods::uninstall_proxy(s);
        check(proxy() == mods::ServerStatus::Stock && dll() == "stock v2", "uninstall: restored just drops the stale original");

        test::write_file(s / "libcrypto-1_1_tg.dll", kProxyBuild);  // proxy, original lost
        check(contains(error_of([&] { mods::install_proxy(s, build); }), "verify the game files"), "install: broken is refused");
        check(contains(error_of([&] { mods::uninstall_proxy(s); }), "verify the game files"), "uninstall: broken is refused");
    }

    // run_state: server.json as sc-server.exe keeps it.
    {
        const fs::path s = tmp / "runstate";
        fs::create_directories(s);
        check(servers::run_state(s, 1000).state == servers::RunState::Stopped, "run_state: no server.json is stopped");
        test::write_file(s / "starforge" / "server.json",
                         R"({"supervisor": 5, "hub": 6, "mongod": 7, "mongoPort": 27017, "started": 900, "heartbeat": 990})");
        auto r = servers::run_state(s, 1000);
        check(r.state == servers::RunState::Running && r.started == 900 && r.mongo_port == 27017 && r.own_mongo,
              "run_state: a fresh heartbeat is running");
        check(servers::run_state(s, 990 + servers::kStaleAfter + 1).state == servers::RunState::Stale, "run_state: an old heartbeat is stale");
        test::write_file(s / "starforge" / "server.json", R"({"supervisor": 5, "hub": 6, "mongod": 0, "mongoPort": 27217, "st)");
        check(servers::run_state(s, 1000).state == servers::RunState::Stale, "run_state: an unreadable server.json is stale");
    }

    // patcher.toml parsing, as the proxy DLL reads it.
    {
        auto c = mods::ServerConfig::parse("on_error = \"continue\"\nunknown = 1\n[patches.\"Hub.exe\"]\nx = true\n", "p");
        check(!c.exit_on_error && c.patches["Hub.exe"]["x"], "config: on_error, patches, unknown keys ignored");
        check(mods::ServerConfig::parse(c.to_toml(), "p").patches == c.patches, "config: round-trips");
        check(contains(error_of([] { mods::ServerConfig::parse("on_error = \"maybe\"", "p"); }), "on_error must be"),
              "config: bad on_error");
        check(contains(error_of([] { mods::ServerConfig::parse("[patches]\nx = true", "p"); }), "per-exe tables"),
              "config: patches not per exe");
        check(contains(error_of([] { mods::ServerConfig::parse("[patches.\"a.exe\"]\nx = 1", "p"); }), "true or false"),
              "config: non-bool switch");
        check(mods::read_server_config(tmp / "nowhere").exit_on_error, "config: missing file is the default");

        // [files]: the overlay and state folders.
        check(c.overlay == "overlay" && c.state == "state", "config: default overlay and state folders");
        check(!contains(c.to_toml(), "[files]"), "config: default folders aren't written out");
        auto f = mods::ServerConfig::parse("[files]\noverlay = 'Z:\\src\\game'\n[patches.\"Hub.exe\"]\nx = false\n", "p");
        check(f.overlay == "Z:\\src\\game" && f.state == "state", "config: [files] overlay");
        auto back = mods::ServerConfig::parse(f.to_toml(), "p");
        check(back.overlay == f.overlay && back.state == "state" && back.patches == f.patches, "config: [files] round-trips");
        check(contains(error_of([] { mods::ServerConfig::parse("[files]\nstate = ''", "p"); }), "folder name or path"),
              "config: empty [files] state");
        check(contains(error_of([] { mods::ServerConfig::parse("files = 1", "p"); }), "files must be a table"),
              "config: files not a table");

        // [stack]: main thread stack per exe.
        auto s = mods::ServerConfig::parse("[stack]\n\"Shard.exe\" = 16\n", "p");
        check(s.stack_mb == std::map<std::string, int>{{"Shard.exe", 16}}, "config: [stack]");
        check(mods::ServerConfig::parse(s.to_toml(), "p").stack_mb == s.stack_mb, "config: [stack] round-trips");
        check(contains(error_of([] { mods::ServerConfig::parse("[stack]\n\"a.exe\" = 0", "p"); }), "1 to 1024"),
              "config: [stack] 0 MB");
        check(contains(error_of([] { mods::ServerConfig::parse("[stack]\n\"a.exe\" = '16M'", "p"); }), "1 to 1024"),
              "config: [stack] not a number");
    }

    std::printf("%s\n", test::failures ? "SOME CHECKS FAILED" : "all checks passed");
    return test::failures ? 1 : 0;
}
