// Tests sc-inject.exe by running it on fake_game.exe (which writes down how it was started).
//
// Usage: inject_test <scenario>; ctest runs each scenario as its own test.

#include <windows.h>

#include <shellapi.h>

#include "common/text.hpp"
#include "inject/inject.hpp"
#include "test_util.hpp"

using sf::inject::quote;
using test::check;
using test::contains;

namespace {

std::wstring g_win64;     // where inject_test, sc-inject.exe and sc_agent.dll are
std::wstring g_fakegame;  // where fake_game.exe is (the "game" folder)
std::wstring g_game;      // fake_game.exe
std::wstring g_profile;   // a fresh profile folder for this scenario

const std::string kPack = R"({ "format": 1, "id": "org.example.test", "name": "Test Pack", "version": "1.0.0",
    "server": { "address": "127.0.0.1:3801" } })";

// Runs sc-inject.exe with `args`; returns its exit code and what it printed.
DWORD run_inject(const std::vector<std::wstring>& args, std::string& printed) {
    std::wstring cmd = quote(g_win64 + L"\\sc-inject.exe");
    for (const auto& a : args) cmd += L" " + quote(a);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    CreatePipe(&read, &write, &sa, 0);
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = write;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        check(false, "sc-inject.exe starts");
        return 999;
    }
    CloseHandle(write);
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(read, buf, sizeof buf, &n, nullptr) && n) printed.append(buf, n);
    CloseHandle(read);
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 999;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

// The usual arguments for this scenario's profile, then `extra`.
std::vector<std::wstring> args(std::vector<std::wstring> extra = {}) {
    std::vector<std::wstring> a{L"--game", g_game, L"--profile", g_profile};
    a.insert(a.end(), extra.begin(), extra.end());
    return a;
}

std::string inject_log() { return test::read_file(g_profile + L"\\inject.log"); }
std::string game_saw() { return test::read_file(g_profile + L"\\fake_game.txt"); }
bool game_ran() { return GetFileAttributesW((g_profile + L"\\fake_game.txt").c_str()) != INVALID_FILE_ATTRIBUTES; }

// Arguments that need quoting, in every way CommandLineToArgvW treats specially.
const std::vector<std::wstring> kTricky = {
    L"plain", L"with space", L"tab\there", L"say \"hi\"", L"trail\\", L"space trail\\", L"a\\\\b", L"a\\\"b",
    L"", L"\\\\server\\share\\x y", L"\u00fcn\u00efc\u00f8d\u00e9 \u2605",
};

// quote(): sc-inject's own test cases, then a round trip through Windows' own parser.
void quoting() {
    check(quote(L"-launchLocalServer") == L"-launchLocalServer", "plain word unchanged");
    check(quote(L"Z:\\star conflict\\win64\\game.exe") == L"\"Z:\\star conflict\\win64\\game.exe\"", "path with space");
    check(quote(L"a\\") == L"a\\", "trailing backslash, no space: unchanged");
    check(quote(L"a b\\") == L"\"a b\\\\\"", "trailing backslash before the closing quote: doubled");
    check(quote(L"say \"hi\"") == L"\"say \\\"hi\\\"\"", "inner quotes escaped");
    for (const auto& s : kTricky) {
        std::wstring line = L"prog.exe " + quote(s);
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(line.c_str(), &argc);
        bool ok = argv && argc == 2 && s == argv[1];
        LocalFree(argv);
        if (!ok) std::printf("  round trip failed for [%s]\n", sf::utf8(s).c_str());
        check(ok, "CommandLineToArgvW gives back what quote() quoted");
    }
}

// Game args after "--" reach the game exactly; the game runs in its own folder with SC_PROFILE
// set; its exit code comes back; address.cfg is written for the pack's server.
void game_args() {
    test::write_file(g_profile + L"\\pack.json", kPack);
    std::vector<std::wstring> extra{L"--"};
    extra.insert(extra.end(), kTricky.begin(), kTricky.end());
    extra.push_back(L"--exit=42");
    std::string printed;
    check(run_inject(args(extra), printed) == 42, "the game's exit code is sc-inject's");
    std::string expect = "profile_env=" + sf::utf8(g_profile) + "\ncwd=" + sf::utf8(g_fakegame) + "\n";
    for (const auto& s : kTricky) expect += "arg=" + sf::utf8(s) + "\n";
    expect += "arg=--exit=42\n";
    const std::string saw = game_saw();
    if (saw != expect) std::printf("game saw:\n%s\nexpected:\n%s\n", saw.c_str(), expect.c_str());
    check(saw == expect, "the game got SC_PROFILE, its own folder as working dir, and every arg unchanged");
    const std::string log = inject_log();
    check(contains(log, "profile " + sf::utf8(g_profile) + "\n"), "inject.log: profile");
    check(contains(log, "starting \"" + sf::utf8(g_game) + "\" plain \"with space\""), "inject.log: command line");
    check(contains(log, "game exited with 42\n"), "inject.log: exit code");
    check(contains(printed, "sc-inject: game exited with 42\n"), "stdout gets the same lines, prefixed");
    check(test::read_file(g_profile + L"\\files\\data\\address.cfg") == "set net_masterServerAddress \"127.0.0.1:3801\"\r\n",
          "address.cfg written for the pack's server");
}

// Without "--", the game gets the profile's args: -launchLocalServer first, then the pack's.
void profile_args() {
    test::write_file(g_profile + L"\\pack.json", R"({ "format": 1, "id": "x", "name": "T", "version": "1.0.0",
        "server": { "local": true }, "game": { "args": ["-x y"] } })");
    std::string printed;
    check(run_inject(args(), printed) == 0, "exit code 0");
    check(contains(game_saw(), "arg=-launchLocalServer\narg=-x y\n"), "the game got the profile's args");
    check(GetFileAttributesW((g_profile + L"\\files\\data\\address.cfg").c_str()) == INVALID_FILE_ATTRIBUTES,
          "no server: no address.cfg");
}

// sc-launch passes Unix paths under wine (/home/...): they become Z:\home\...
void unix_paths() {
    if (g_game.rfind(L"Z:\\", 0) != 0) {
        std::printf("SKIP: not under wine (no Z: drive paths)\n");
        return;
    }
    auto unix = [](std::wstring p) {
        p = p.substr(2);
        for (auto& c : p)
            if (c == L'\\') c = L'/';
        return p;
    };
    test::write_file(g_profile + L"\\pack.json", kPack);
    std::string printed;
    check(run_inject({L"--game", unix(g_game), L"--profile", unix(g_profile)}, printed) == 0, "exit code 0");
    check(contains(game_saw(), "profile_env=" + sf::utf8(g_profile) + "\n"), "SC_PROFILE is the Z: path");
}

// Bad command lines: exit 1 and say why (there's no profile yet, so only on stdout).
void bad_args() {
    std::string printed;
    check(run_inject({}, printed) == 1, "no arguments: exit 1");
    check(contains(printed, "sc-inject: error: --game is required\n"), "no arguments: --game is required");
    printed.clear();
    check(run_inject({L"--game", g_game}, printed) == 1, "no profile: exit 1");
    check(contains(printed, "sc-inject: error: --profile is required\n"), "--profile is required");
    printed.clear();
    check(run_inject(args({L"--bogus"}), printed) == 1, "unknown option: exit 1");
    check(contains(printed, "error: unknown argument \"--bogus\""), "unknown argument named");
}

// Problems found before the game starts: say why in inject.log, exit 1, game never runs.
void refuses(const char* what, const std::string& why) {
    std::string printed;
    check(run_inject(args(), printed) == 1, "exit 1");
    const std::string log = inject_log();
    check(contains(log, "error: ") && contains(log, why), what);
    check(!game_ran(), "the game was not started");
}

void bad_profile() {
    test::write_file(g_profile + L"\\pack.json", R"({ "format": 2, "name": "X" })");
    refuses("inject.log: the profile's problem", "pack.json: format 2 is not supported");
}

void bad_patchset() {
    test::write_file(g_profile + L"\\pack.json", kPack);
    CreateDirectoryW((g_profile + L"\\patches").c_str(), nullptr);
    test::write_file(g_profile + L"\\patches\\broken.patchset.toml", "format = 1\n");
    refuses("inject.log: the broken patchset, by name", "broken.patchset.toml");
}

void bad_server() {
    test::write_file(g_profile + L"\\pack.json", kPack);
    test::write_file(g_profile + L"\\local.json", R"({ "server": "a\" ; quit" })");
    refuses("inject.log: bad server address", "server \"a\" ; quit\": expected host:port");
}

void no_agent() {
    test::write_file(g_profile + L"\\pack.json", kPack);
    std::string printed;
    check(run_inject(args({L"--dll", g_win64 + L"\\nope.dll"}), printed) == 1, "exit 1");
    check(contains(inject_log(), "error: agent not found at " + sf::utf8(g_win64) + "\\nope.dll"), "inject.log: agent not found");
    check(!game_ran(), "the game was not started");
}

std::string modules() { return test::read_file(g_profile + L"\\fake_game_modules.txt"); }

// The agent (sc_agent.dll next to sc-inject.exe, the default) is in the game before it runs.
void agent() {
    test::write_file(g_profile + L"\\pack.json", kPack);
    std::string printed;
    check(run_inject(args(), printed) == 0, "exit code 0");
    check(contains(inject_log(), "agent injected, resuming game\n"), "inject.log: agent injected");
    check(contains(modules(), "sc_agent.dll=0x"), "the agent was loaded in the game when its code ran");
    std::printf("  %s", modules().c_str());
    const std::string log = test::read_file(g_profile + L"\\agent.log");
    check(contains(log, " loaded into " + sf::utf8(g_game) + "\n"), "agent.log: loaded into fake_game.exe");
    check(contains(log, "file hooks installed\n"), "agent.log: the agent ran to the end");
}

// An agent that refuses to start (here: the pack asks for a core patch it doesn't have) stops the
// game before any of its code runs.
void agent_refuses() {
    test::write_file(g_profile + L"\\pack.json", R"({ "format": 1, "id": "x", "name": "T", "version": "1.0.0",
        "patches": { "core": ["no-such-patch"] } })");
    refuses("inject.log: the agent did not load", "did not load; if the pack has patches");
    check(contains(test::read_file(g_profile + L"\\agent.log"), "unknown core patch"), "agent.log says why");
}

// Something that isn't a DLL at all.
void not_a_dll() {
    test::write_file(g_profile + L"\\pack.json", kPack);
    test::write_file(g_profile + L"\\fake.dll", "not a DLL");
    std::string printed;
    check(run_inject(args({L"--dll", g_profile + L"\\fake.dll"}), printed) == 1, "exit 1");
    check(contains(inject_log(), "did not load"), "inject.log: did not load");
    check(!game_ran(), "the game was not started");
}

// zero_low.dll always loads at 0x200000000: LoadLibraryW's result cut to 32 bits is 0 there,
// which must not be mistaken for a failure.
void zero_low() {
    test::write_file(g_profile + L"\\pack.json", kPack);
    std::string printed;
    check(run_inject(args({L"--dll", g_win64 + L"\\zero_low.dll"}), printed) == 0, "exit code 0");
    check(contains(modules(), "zero_low.dll=0x200000000\n"), "zero_low.dll loaded at 0x200000000");
    check(contains(inject_log(), "agent injected, resuming game\n"), "inject.log: injected");
}

// A process the game leaves behind (like its local server) is stopped when the game exits.
void leftover() {
    test::write_file(g_profile + L"\\pack.json", kPack);
    std::string printed;
    check(run_inject(args({L"--", L"--leave-child"}), printed) == 0, "exit code 0");
    check(contains(inject_log(), "stopping 1 process(es) the game left running\n"), "inject.log: stopping the leftover");
    DWORD pid = std::strtoul(test::read_file(g_profile + L"\\linger.pid").c_str(), nullptr, 10);
    check(pid != 0, "the leftover process started");
    if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
        check(WaitForSingleObject(h, 5000) == WAIT_OBJECT_0, "the leftover process was stopped");
        CloseHandle(h);
    }
}

}  // namespace

int main(int argc, char** argv) {
    test::exit_on_crash();
    const std::string scenario = argc > 1 ? argv[1] : "";
    g_win64 = test::exe_dir();
    g_fakegame = g_win64 + L"\\fakegame\\win64";
    g_game = g_fakegame + L"\\fake_game.exe";
    g_profile = test::fresh_dir(g_win64 + L"\\inject_profiles\\" + sf::wide(scenario));

    if (scenario == "quoting") quoting();
    else if (scenario == "game_args") game_args();
    else if (scenario == "profile_args") profile_args();
    else if (scenario == "unix_paths") unix_paths();
    else if (scenario == "bad_args") bad_args();
    else if (scenario == "bad_profile") bad_profile();
    else if (scenario == "bad_patchset") bad_patchset();
    else if (scenario == "bad_server") bad_server();
    else if (scenario == "no_agent") no_agent();
    else if (scenario == "leftover") leftover();
    else if (scenario == "agent") agent();
    else if (scenario == "agent_refuses") agent_refuses();
    else if (scenario == "not_a_dll") not_a_dll();
    else if (scenario == "zero_low") zero_low();
    else {
        std::printf("unknown scenario \"%s\"\n", scenario.c_str());
        return 2;
    }
    return test::failures ? 1 : 0;
}
