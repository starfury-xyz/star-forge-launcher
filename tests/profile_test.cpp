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

    return test::failures ? 1 : 0;
}
