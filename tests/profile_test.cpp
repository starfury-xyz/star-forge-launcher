// Unit test for sf::profile: load() on hand-written profile folders, and normalize().

#include "profile/profile.hpp"

#include "common/text.hpp"
#include "test_util.hpp"

using namespace sf::profile;
using test::check;

namespace {

const std::string kMinimalPack = R"({ "format": 1, "id": "org.example.test", "name": "Test Pack", "version": "1.0.0" })";

std::wstring g_root;  // <exe dir>\profile_test_data

// A fresh profile folder named `name` with the given pack.json (and local.json if non-empty).
std::wstring make(const wchar_t* name, const std::string& pack, const std::string& local = "") {
    std::wstring dir = test::fresh_dir(g_root + L"\\" + name);
    if (!pack.empty()) test::write_file(dir + L"\\pack.json", pack);
    if (!local.empty()) test::write_file(dir + L"\\local.json", local);
    return dir;
}

// The message load() throws for this folder, or "" if it loaded.
std::string load_error(const std::wstring& dir) {
    try {
        load(dir);
        return "";
    } catch (const Error& e) {
        return e.what();
    }
}

}  // namespace

int main() {
    test::exit_on_crash();
    g_root = test::exe_dir() + L"\\profile_test_data";
    CreateDirectoryW(g_root.c_str(), nullptr);

    // Defaults: only pack.json, no local.json.
    {
        Profile p = load(make(L"minimal", kMinimalPack));
        check(p.name == "Test Pack", "name comes from pack.json");
        check(!p.trace_files, "trace_files defaults to off");
        check(p.own_game_data, "own_game_data defaults to on");
        check(p.patches.method == PatchMethod::Inject && p.patches.on_error == OnError::Exit,
              "patches default to inject / exit");
        check(p.patches.plugins.empty() && p.patches.core.empty() && p.patch_choices.empty(), "no patches by default");
    }

    // The real STARFURY sample pack from star-forge loads (extra fields are ignored).
    {
        Profile p = load(test::exe_dir() + L"\\data\\starfury");
        check(p.name == "STARFURY", "samples/starfury/pack.json loads");
    }

    // pack.json patches settings.
    {
        Profile p = load(make(L"patches", R"({ "format": 1, "name": "P", "patches": {
            "on_error": "continue", "plugins": ["patches/a.dll"], "core": ["loose-before-packed"] } })"));
        check(p.patches.on_error == OnError::Continue, "patches.on_error continue");
        check(p.patches.plugins.size() == 1 && p.patches.plugins[0] == "patches/a.dll", "patches.plugins");
        check(p.patches.core.size() == 1 && p.patches.core[0] == "loose-before-packed", "patches.core");
    }

    // local.json changes on top of the pack.
    {
        Profile p = load(make(L"local", kMinimalPack, R"({ "name": "My Server", "trace_files": true,
            "shared_game_data": true, "patches": { "Game": { "fast-start": false } } })"));
        check(p.name == "My Server", "local.json name wins");
        check(p.trace_files, "local.json trace_files");
        check(!p.own_game_data, "shared_game_data turns own_game_data off");
        check(p.patch_choices.count("Game") && p.patch_choices["Game"].count("fast-start") &&
                  !p.patch_choices["Game"]["fast-start"],
              "local.json patch choices");
    }
    {
        Profile p = load(make(L"blank_name", kMinimalPack, R"({ "name": "   " })"));
        check(p.name == "Test Pack", "blank local.json name falls back to the pack's");
    }

    // Server and game args (what sc-inject uses), as in sc-profile's pack_parses_and_merges_local_settings.
    {
        const std::string pack = R"({ "format": 1, "id": "x", "name": "Starfury", "version": "1.2.0",
            "server": { "address": "starfury.xyz:3801" }, "game": { "args": ["-a"] } })";
        Profile plain = load(make(L"server", pack));
        check(game_args(plain) == std::vector<std::string>{"-a"}, "game args from the pack");
        check(address_cfg(plain) == std::string("set net_masterServerAddress \"starfury.xyz:3801\"\r\n"),
              "address.cfg for the pack's server");
        Profile mine = load(make(L"server_local", pack, R"({ "server": "", "local_server": true, "args": ["-b"] })"));
        check(!mine.server && !address_cfg(mine), "local.json server \"\": no server, no address.cfg");
        check(game_args(mine) == std::vector<std::string>{"-launchLocalServer", "-a", "-b"},
              "local server flag first, then the pack's args, then local.json's");
        Profile other = load(make(L"server_other", pack, R"({ "server": "127.0.0.1:3801" })"));
        check(other.server == std::string("127.0.0.1:3801"), "local.json server replaces the pack's");
        Profile bad = load(make(L"server_bad", pack, R"({ "server": "a\" ; quit" })"));
        try {
            address_cfg(bad);
            check(false, "unsafe server address rejected");
        } catch (const Error& e) {
            check(test::contains(e.what(), "expected host:port"), "unsafe server address rejected");
        }
    }

    // windows_path_from_arg: wine's Z: drive.
    check(windows_path_from_arg(L"/home/a/game.exe") == L"Z:\\home\\a\\game.exe", "unix path -> Z:");
    check(windows_path_from_arg(L"C:\\Games\\x") == L"C:\\Games\\x", "windows path unchanged");

    // Errors: each one names the file, so agent.log says what to fix.
    {
        std::string e = load_error(make(L"no_pack", ""));
        check(test::contains(e, "pack.json: "), "missing pack.json is an error naming the file");
    }
    check(load_error(make(L"bad_json", "{ nope")).rfind("pack.json: ", 0) == 0, "malformed pack.json");
    check(load_error(make(L"array", "[]")) == "pack.json: expected a JSON object", "pack.json must be an object");
    check(test::contains(load_error(make(L"format2", R"({ "format": 2, "name": "X" })")), "format 2 is not supported"),
          "unsupported format");
    check(test::contains(load_error(make(L"no_format", R"({ "name": "X" })")), "format 0 is not supported"),
          "missing format");
    check(load_error(make(L"no_name", R"({ "format": 1, "name": " " })")) == "pack.json: name is empty", "blank name");
    check(test::contains(load_error(make(L"bad_on_error", R"({ "format": 1, "name": "X", "patches": { "on_error": "maybe" } })")),
                         "pack.json: patches.on_error \"maybe\""),
          "unknown patches.on_error");
    check(load_error(make(L"bad_type", R"({ "format": 1, "name": 5 })")).rfind("pack.json: ", 0) == 0,
          "wrong type in pack.json");
    check(load_error(make(L"bad_local", kMinimalPack, R"({ "trace_files": "yes" })")).rfind("local.json: ", 0) == 0,
          "wrong type in local.json");

    // normalize, matching sc_profile::normalize.
    check(normalize(L"C:/Games//star conflict/") == L"C:\\Games\\star conflict", "normalize slashes");
    check(normalize(L"\\\\?\\C:\\a\\b") == L"C:\\a\\b", "normalize drops \\\\?\\");
    check(normalize(L"\\??\\C:\\a") == L"C:\\a", "normalize drops \\??\\");
    check(normalize(L"C:\\") == L"C:\\", "normalize keeps a drive root");

    // relative_to / profile_file: the same cases as sc-profile's Rust tests.
    const std::wstring ROOT = L"Z:\\home\\user\\.local\\share\\Steam\\steamapps\\common\\star conflict";
    auto rel = [&](const std::wstring& full) { return relative_to(ROOT, full).value_or(L"(none)"); };
    check(rel(ROOT + L"\\data\\address.cfg") == L"data\\address.cfg", "relative_to: inside root");
    check(rel(L"z:/HOME/user/.local/share/Steam/steamapps/common/Star Conflict//data/ui/x.lua") == L"data\\ui\\x.lua",
          "relative_to: case-insensitive and slash tolerant");
    check(rel(L"\\\\?\\" + ROOT + L"\\data\\a.cfg") == L"data\\a.cfg", "relative_to: \\\\?\\ prefix");
    check(rel(L"Z:\\home\\user\\other\\data\\a.cfg") == L"(none)", "relative_to: outside root");
    check(rel(ROOT + L"2\\data\\a.cfg") == L"(none)", "relative_to: sibling folder with the same prefix");
    // Not in the Rust tests: with a one-character suffix the separator check isn't the only
    // thing rejecting it (the rest starts with an empty part), so test a longer one too.
    check(rel(ROOT + L"_backup\\data\\a.cfg") == L"(none)", "relative_to: sibling folder with a longer suffix");
    check(rel(ROOT) == L"(none)" && rel(ROOT + L"\\") == L"(none)", "relative_to: root itself");
    check(rel(ROOT + L"\\data\\..\\..\\x") == L"(none)", "relative_to: rejects ..");
    check(rel(ROOT + L"\\data\\.\\x") == L"(none)", "relative_to: rejects .");
    check(rel(ROOT + L"\\data\\x.cfg:evil") == L"(none)", "relative_to: rejects streams (:)");
    check(profile_file(L"Z:\\p\\", L"data\\a.cfg") == L"Z:\\p\\files\\data\\a.cfg", "profile_file joins");
    check(state_local_appdata(L"Z:\\p\\") == L"Z:\\p\\state\\AppData\\Local", "state_local_appdata joins");

    return test::failures ? 1 : 0;
}
