// Tests the sc-launch program end to end: a server team's round trip (keygen, sign, file list,
// check) and a player's (install, list, update in place), run as the real executable.
//
// Usage: cli_test <sc-launch>

#include "launch/env.hpp"
#include "launch/process.hpp"

#include "check.hpp"

namespace fs = std::filesystem;
using namespace sf::launch;
using test::check;
using test::contains;

namespace {

fs::path g_exe, g_dir;

// Runs sc-launch with `args` in the test folder; its output (both streams) and exit code.
std::pair<int, std::string> sc(std::vector<std::string> args) {
    Command cmd;
    cmd.program = g_exe;
    cmd.args = std::move(args);
    cmd.cwd = g_dir;
    cmd.set_env(env::kDataDir, ustr(g_dir / "data"));
    cmd.set_env("STAR_FORGE_PACK_KEY", std::nullopt);
    const fs::path out = g_dir / "out.txt";
    auto code = spawn(cmd, Output{Output::Kind::File, out}).wait();
    std::string text = test::read_file(out);
    std::string clean;
    for (char c : text)
        if (c != '\r') clean += c;
    return {code.value_or(-1), clean};
}

}  // namespace

int main(int argc, char** argv) {
    test::exit_on_crash();
    if (argc < 2) {
        std::printf("usage: cli_test <sc-launch>\n");
        return 2;
    }
    g_exe = fs::absolute(fs::u8path(argv[1]));
    g_dir = test::fresh_dir(fs::temp_directory_path() / "sf-cli-test");
    const std::vector<std::string> prof{"--profiles-dir", ustr(g_dir / "profiles")};
    auto with = [&](std::vector<std::string> more) {
        std::vector<std::string> a = prof;
        a.insert(a.end(), more.begin(), more.end());
        return a;
    };

    auto [code, out] = sc({"--help"});
    check(code == 0 && out.rfind("sc-launch - Star Conflict launcher with switchable profiles\n", 0) == 0, "--help");
    std::tie(code, out) = sc({"bogus"});
    check(code == 1 && out == "sc-launch: unknown command; see sc-launch --help\n", "an unknown command: " + out);
    std::tie(code, out) = sc({"--frob"});
    check(code == 1 && out == "sc-launch: unknown option --frob\n", "an unknown option");
    std::tie(code, out) = sc({"--key"});
    check(code == 1 && out == "sc-launch: --key needs a value\n", "an option without its value");
    std::tie(code, out) = sc(with({"list"}));
    check(code == 0 && contains(out, "no profiles in "), "no profiles yet");
    std::tie(code, out) = sc(with({"run", "../x"}));
    check(code == 1 && out == "sc-launch: \"../x\" is not a valid profile name\n", "a bad profile name");

    // A server team's side.
    std::tie(code, out) = sc({"pack", "keygen", "key.txt"});
    check(code == 0 && contains(out, "public key:  ed25519:") && contains(out, "fingerprint: "), "keygen");
    std::tie(code, out) = sc({"pack", "keygen", "key.txt"});
    check(code == 1 && out == "sc-launch: key.txt already exists; not overwriting a key\n", "keygen won't overwrite");
    test::write_file(g_dir / "pk/pack.json", R"({"format":1,"id":"org.example.cli","name":"CLI Test","version":"1.0.0"})");
    test::write_file(g_dir / "pk/files/data/a.lua", "hi");
    std::tie(code, out) = sc({"pack", "sign", "pk"});
    check(code == 1 && out == "sc-launch: this needs --key <file> or STAR_FORGE_PACK_KEY\n", "sign needs a key");
    std::tie(code, out) = sc({"pack", "sign", "pk", "--key", "key.txt"});
    check(code == 0 && contains(out, "signed org.example.cli 1.0.0 (2 files) with key "), "sign: " + out);
    std::tie(code, out) = sc({"pack", "files", "pk/files", "list.txt", "--key", "key.txt"});
    check(code == 1 && contains(out, "needs --base-url"), "an SCMOD1 list needs --base-url");
    std::tie(code, out) = sc({"pack", "files", "pk/files", "list.txt", "--base-url", "https://a.org/f", "--key", "key.txt"});
    check(code == 0 && contains(out, "wrote list.txt: 1 file(s), SCMOD1") && fs::exists(g_dir / "list.txt.sig"), "an SCMOD1 list, signed");
    std::tie(code, out) = sc({"pack", "files", "pk/files", "list.json"});
    check(code == 0 && out == "wrote list.json: 1 file(s), STAR FORGE list\n" && !fs::exists(g_dir / "list.json.sig"),
          "a JSON list, unsigned");
    std::tie(code, out) = sc(with({"--game-dir", ustr(g_dir / "nogame"), "pack", "check", "pk"}));
    check(code == 0 && contains(out, "CLI Test 1.0.0 (org.example.cli)\n") && contains(out, "  signed with a new key ") &&
              contains(out, "(patches not checked against the game: ") && out.size() > 8 &&
              out.substr(out.size() - 8) == "pack OK\n",
          "pack check: " + out);

    // A player's side.
    std::tie(code, out) = sc(with({"install", "pk", "--yes"}));
    check(code == 0 && contains(out, "installed as profile cli-test"), "install --yes");
    std::tie(code, out) = sc(with({"list"}));
    check(code == 0 && out == "cli-test             -                        client only   0 patch(es)  CLI Test\n", "list: " + out);
    test::write_file(g_dir / "pk/files/data/a.lua", "changed");
    std::tie(code, out) = sc(with({"install", "pk", "--yes"}));
    check(code == 1 && out == "sc-launch: files/data/a.lua does not match the signed pack\n", "a changed signed pack is refused: " + out);
    std::tie(code, out) = sc(with({"update", "cli-test"}));
    check(code == 1 && out == "sc-launch: cli-test has no update address\n", "update without an address");
    std::tie(code, out) = sc(with({"updates"}));
    check(code == 0 && out == "all packs are up to date\n", "updates");

    // Patch switches: the core patch is listed and can be switched on for this profile.
    std::tie(code, out) = sc(with({"patches", "cli-test"}));
    check(code == 0 && contains(out, "game.exe (core") && contains(out, "  off  loose-before-packed "), "patches: " + out);
    std::tie(code, out) = sc(with({"patches", "cli-test", "on", "game.exe", "loose-before-packed"}));
    check(code == 0 && out == "cli-test: game.exe loose-before-packed is on\n", "patches on: " + out);
    std::tie(code, out) = sc(with({"list"}));
    check(contains(out, "client only   1 patch(es)"), "list counts the switched-on core patch: " + out);
    std::tie(code, out) = sc(with({"patches", "cli-test"}));
    check(contains(out, "  on * loose-before-packed "), "patches: marked as the player's switch");
    std::tie(code, out) = sc(with({"patches", "cli-test", "default", "game.exe", "loose-before-packed"}));
    check(code == 0 && contains(out, "back to the pack's default"), "patches default");
    std::tie(code, out) = sc(with({"patches", "cli-test", "yes", "game.exe", "loose-before-packed"}));
    check(code == 1 && out == "sc-launch: expected on, off or default, not \"yes\"\n", "patches: a bad switch word");

    // Mods: list, switch, order; and the author's diff.
    std::tie(code, out) = sc(with({"mods", "cli-test"}));
    check(code == 0 && contains(out, "no mods in "), "mods: none yet: " + out);
    test::write_file(g_dir / "profiles/cli-test/mods/zeta/mod.json", R"({"format":1,"id":"zeta","name":"Zeta","version":"1.0.0"})");
    test::write_file(g_dir / "profiles/cli-test/mods/alpha/mod.json", R"({"format":1,"id":"alpha","version":"2.0.0"})");
    std::tie(code, out) = sc(with({"mods", "cli-test"}));
    check(code == 0 && out.find("alpha") < out.find("zeta") && contains(out, "1.0.0") && contains(out, "Zeta"),
          "mods: listed by id: " + out);
    std::tie(code, out) = sc(with({"mods", "cli-test", "order", "zeta", "alpha"}));
    check(code == 0, "mods order: " + out);
    std::tie(code, out) = sc(with({"mods", "cli-test", "off", "alpha"}));
    check(code == 0 && out == "cli-test: mod alpha is off\n", "mods off: " + out);
    std::tie(code, out) = sc(with({"mods", "cli-test"}));
    check(out.find("zeta") < out.find("alpha") && contains(out, "off             alpha"), "mods: order and switch shown: " + out);
    std::tie(code, out) = sc(with({"mods", "cli-test", "on", "nope"}));
    check(code == 1 && contains(out, "no mod \"nope\""), "mods on: unknown id refused: " + out);
    test::write_file(g_dir / "stock.txt", "Texture\n{\n\tname \"A\"\n\trect \"0 0 1 1\"\n}\n");
    test::write_file(g_dir / "modded.txt", "Texture\n{\n\tname \"A\"\n\trect \"5 5 1 1\"\n}\n");
    std::tie(code, out) = sc({"mod", "diff", ustr(g_dir / "stock.txt"), ustr(g_dir / "modded.txt"), "data\\ui\\x.txt"});
    check(code == 0 && out == "patch \"data/ui/x.txt\"\n{\n\tset Texture\n\t{\n\t\trect \"5 5 1 1\"\n\t}\n}\n",
          "mod diff: " + out);

    // A server install: proxy in place, one exe whose build doesn't match, so status fails.
    const fs::path cloud = g_dir / "cloud";
    test::write_file(cloud / "libcrypto-1_1_tg.dll", "orig");
    test::write_file(g_dir / "proxy.dll", std::string("MZ sfp_version\0", 15));
    std::tie(code, out) = sc({"server", "install", ustr(cloud), ustr(g_dir / "proxy.dll")});
    check(code == 0 && contains(out, "installed the proxy from"), "server install: " + out);
    test::write_file(cloud / "Hub.exe", "MZ");
    test::write_file(cloud / "starforge/patchsets/Hub.exe.patchset.toml", R"(format = 1
exe = "Hub.exe"
sha1 = "0000000000000000000000000000000000000000"
image_base = 0x400000
[[patch]]
id = "a"
[[patch.write]]
va = 0x401000
orig = "74"
bytes = "eb"
)");
    std::tie(code, out) = sc({"server", "status", ustr(cloud)});
    check(code == 1 && contains(out, "proxy:   installed") && contains(out, "  on   a ") &&
              contains(out, "problem: Hub.exe is a different build") && contains(out, "server has problems\n"),
          "server status: " + out);
    std::tie(code, out) = sc({"server", "off", ustr(cloud), "hub.exe", "a"});
    check(code == 0 && contains(out, "restart the server"), "server off");
    check(contains(test::read_file(cloud / "starforge/patcher.toml"), "[patches.\"Hub.exe\"]\n\"a\" = false\n"),
          "server off: written to patcher.toml");
    std::tie(code, out) = sc({"server", "status", ustr(cloud)});
    check(contains(out, "  off* a "), "server status: the switch is shown");
    std::tie(code, out) = sc({"server", "uninstall", ustr(cloud)});
    check(code == 0 && test::read_file(cloud / "libcrypto-1_1_tg.dll") == "orig", "server uninstall: " + out);
    std::tie(code, out) = sc({"server", "status", ustr(cloud)});
    check(contains(out, "proxy:   not installed"), "server status after uninstall: " + out);

    std::printf("%s (%d failed)\n", test::failures ? "FAILED" : "ok", test::failures);
    return test::failures ? 1 : 0;
}
