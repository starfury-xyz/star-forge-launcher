// Tests sf::gui::Backend, the launcher's backend, the way its interface uses it: commands through
// invoke() with Tauri's JSON, and the events and host calls they cause, recorded by a fake host.
// The game is a fake Steam install (on Linux with a Proton whose runtime is a script), packs come
// from folders and a local HTTP server.

#include <chrono>
#include <thread>

#include "app/lifecycle.hpp"
#include "crypto/sha256.hpp"
#include "gui/backend.hpp"
#include "launch/env.hpp"
#include "launch/net.hpp"
#include "launch/profiles.hpp"

#include "check.hpp"
#include "http_server.hpp"

namespace fs = std::filesystem;
using namespace sf::launch;
using sf::gui::json;
using test::check;
using test::contains;

namespace {

struct FakeHost : sf::gui::Host {
    std::mutex mu;
    std::vector<std::pair<std::string, json>> events;
    std::vector<std::string> opened, picks;
    std::optional<std::string> pick_answer;
    int hidden = 0, restored = 0, exited = 0;

    void emit(const std::string& event, const json& payload) override {
        std::lock_guard<std::mutex> lock(mu);
        events.emplace_back(event, payload);
    }
    void open(const std::string& target) override {
        std::lock_guard<std::mutex> lock(mu);
        opened.push_back(target);
    }
    std::optional<std::string> pick_folder(const std::string& title, const std::optional<std::string>& start) override {
        std::lock_guard<std::mutex> lock(mu);
        picks.push_back("folder " + title + (start ? " at " + *start : ""));
        return pick_answer;
    }
    std::optional<std::string> pick_file(const std::string& title, const std::string& filter,
                                         const std::vector<std::string>& exts) override {
        std::lock_guard<std::mutex> lock(mu);
        picks.push_back("file " + title + " (" + filter + ": " + exts.at(0) + ")");
        return pick_answer;
    }
    void hide_to_tray() override {
        std::lock_guard<std::mutex> lock(mu);
        ++hidden;
    }
    void restore() override {
        std::lock_guard<std::mutex> lock(mu);
        ++restored;
    }
    void exit() override {
        std::lock_guard<std::mutex> lock(mu);
        ++exited;
    }

    // Payloads of one event, in order.
    std::vector<json> of(const std::string& name) {
        std::lock_guard<std::mutex> lock(mu);
        std::vector<json> out;
        for (const auto& [n, p] : events)
            if (n == name) out.push_back(p);
        return out;
    }
    std::string last_opened() {
        std::lock_guard<std::mutex> lock(mu);
        return opened.empty() ? "" : opened.back();
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mu);
        events.clear();
    }
};

FakeHost host;
sf::gui::Backend* g_backend = nullptr;
fs::path g_helper;  // Windows: proc_helper.exe, the stand-in for sc-inject.exe

json call(const std::string& command, const json& args = json::object()) { return g_backend->invoke(command, args); }

std::string error_of(const std::string& command, const json& args = json::object()) {
    try {
        call(command, args);
        return "";
    } catch (const std::exception& e) {
        return e.what();
    }
}

// Waits up to 20 s for `done`.
template <class F>
bool wait_for(F done) {
    for (int i = 0; i < 400; ++i) {
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

std::string phase() { return call("get_launcher_snapshot")["phase"]; }

std::string sha(const std::string& s) { return sf::crypto::sha256_hex(s.data(), s.size()); }

const std::string kPng = std::string("\x89PNG\r\n\x1a\n", 8) + "png";
const std::string kJpeg = "\xff\xd8\xff\xe0jpeg";

void prefs(const fs::path& data) {
    auto& b = *g_backend;
    b.load_prefs();
    const json defaults = call("get_launcher_preferences");
    check(defaults.dump() ==
              R"({"backgroundFolder":"","checkUpdates":true,"closeBehavior":"exit","factionTheme":"starforge","gameDirectory":"",)"
              R"("launchBehavior":"tray","packBranding":true,"protonPath":"","selectedProfile":"","setupDone":false})",
          "defaults: " + defaults.dump());

    test::write_file(data / "launcher.json", R"({"selectedProfile":"x","bogus":1})");
    b.load_prefs();
    check(b.prefs().selected_profile == "x" && b.prefs().setup_done, "an older launcher.json: setup counts as done, unknown keys ignored");
    test::write_file(data / "launcher.json", R"({"setupDone":false,"launchBehavior":"nope","selectedProfile":"x"})");
    b.load_prefs();
    check(b.prefs().selected_profile.empty() && !b.prefs().setup_done, "a bad value: all defaults");
    test::write_file(data / "launcher.json", "not json");
    b.load_prefs();
    check(!b.prefs().setup_done, "not JSON: defaults, setup not done");

    const json next = call("update_launcher_preferences", {{"update", {{"key", "launchBehavior"}, {"value", "keepOpen"}}}});
    check(next["launchBehavior"] == "keepOpen" && b.prefs().launch_behavior == sf::gui::LaunchBehavior::KeepOpen, "update: set");
    check(test::read_file(data / "launcher.json") ==
              "{\n  \"selectedProfile\": \"\",\n  \"launchBehavior\": \"keepOpen\",\n  \"closeBehavior\": \"exit\",\n"
              "  \"factionTheme\": \"starforge\",\n  \"packBranding\": true,\n  \"backgroundFolder\": \"\",\n"
              "  \"gameDirectory\": \"\",\n  \"protonPath\": \"\",\n  \"checkUpdates\": true,\n  \"setupDone\": false\n}",
          "launcher.json laid out as serde_json's to_string_pretty");
    check(!fs::exists(data / "launcher.json.tmp"), "no temporary file left");
    auto upd = [](const char* key, json value) { return json{{"update", {{"key", key}, {"value", value}}}}; };
    check(error_of("update_launcher_preferences", upd("nope", 1)) == "unknown preference nope", "an unknown preference");
    check(error_of("update_launcher_preferences", upd("launchBehavior", "x")) ==
              "invalid value for launchBehavior: unknown variant `x`, expected one of `tray`, `keepOpen`, `exit`",
          "a bad variant");
    check(error_of("update_launcher_preferences", upd("closeBehavior", "x")) ==
              "invalid value for closeBehavior: unknown variant `x`, expected `tray` or `exit`",
          "a bad variant of two");
    check(error_of("update_launcher_preferences", upd("packBranding", "yes")) ==
              "invalid value for packBranding: invalid type: string \"yes\", expected a boolean",
          "a wrong type");
    check(error_of("update_launcher_preferences", {{"update", {{"key", "packBranding"}}}}) ==
              "invalid args `update` for command `update_launcher_preferences`: missing field `value`",
          "a missing field");
    check(b.prefs().pack_branding && b.prefs().launch_behavior == sf::gui::LaunchBehavior::KeepOpen, "failed updates change nothing");
    check(error_of("frob") == "command frob not found", "an unknown command");
}

void profiles_and_packs(const fs::path& base, test::FileServer& srv) {
    auto& b = *g_backend;
    const fs::path profiles = base / "data" / "profiles";
    const std::string u = srv.base();

    json info = call("get_setup_info");
    check(info["needed"] == true && info["profilesDir"] == ustr(profiles) && info["standardProfilesDir"] == ustr(profiles) &&
              info["dataDir"] == ustr(base / "data") && info["profileCount"] == 0 && info["appimage"].is_null(),
          "setup info: " + info.dump());
    check(error_of("check_game_folder", {{"path", ustr(base)}}) ==
              "That folder doesn't hold Star Conflict (no win64\\game.exe). Pick the folder Steam installed it in.",
          "not a game folder");
    check(error_of("check_game_folder", {}) ==
              "invalid args `path` for command `check_game_folder`: command check_game_folder missing required key path",
          "a missing argument");

    // The player's own profiles.
    auto input = [](json id, const char* name, const char* server, bool local, bool own) {
        return json{{"profile", {{"id", id}, {"name", name}, {"server", server}, {"localServer", local}, {"ownGameData", own}}}};
    };
    check(error_of("save_profile", input(nullptr, "  ", "", false, true)) == "Give the profile a name.", "a name is needed");
    check(error_of("save_profile", input(nullptr, std::string(65, 'x').c_str(), "", false, true)) ==
              "The profile name is too long (64 characters at most).",
          "a name too long");
    check(error_of("save_profile", input(nullptr, "Bad", "a b", false, true)).rfind("The server address can only use", 0) == 0 &&
              !fs::exists(profiles / "bad"),
          "a bad address: refused, nothing left behind");
    check(call("save_profile", input(nullptr, " My Server ", "STARFURY.XYZ:3801", false, true)) == "my-server", "new profile");
    const sf::pack::Pack own = profiles::read_pack(profiles / "my-server");
    check(own.id == "local.my-server" && own.name == "My Server" && own.server.address == std::string("STARFURY.XYZ:3801") &&
              own.schema && own.version == "1.0.0",
          "its pack.json");
    check(b.prefs().selected_profile == "my-server", "the first profile is selected");
    check(call("save_profile", input(nullptr, "My Server", "", false, true)) == "my-server-2" && b.prefs().selected_profile == "my-server",
          "a second one: a free name, selection kept");
    test::write_file(profiles / "my-server/files/data/address.cfg", "set net_masterServerAddress \"STARFURY.XYZ:3801\"\r\n");
    check(call("save_profile", input("my-server", "Renamed", "", true, false)) == "my-server", "edit own");
    const sf::pack::Pack edited = profiles::read_pack(profiles / "my-server");
    check(edited.name == "Renamed" && !edited.server.address && edited.server.local &&
              profiles::read_local(profiles / "my-server").shared_game_data,
          "edited in pack.json, game data choice in local.json");
    check(!fs::exists(profiles / "my-server/files/data/address.cfg"), "a generated address.cfg goes with the server");
    test::write_file(profiles / "my-server-2/files/data/address.cfg", "set net_masterServerAddress \"a:1\"\n// mine\n");
    call("save_profile", input("my-server-2", "Two", "", false, true));
    check(fs::exists(profiles / "my-server-2/files/data/address.cfg"), "a hand-written one stays");
    check(error_of("save_profile", input("nope", "X", "", false, true)) == "profile \"nope\" does not exist", "editing a missing profile");
    check(error_of("save_profile", {{"profile", {{"name", "X"}}}}) ==
              "invalid args `profile` for command `save_profile`: missing field `server`",
          "a missing field");

    // A server's pack, from a folder.
    const fs::path pk = base / "pk";
    const std::string pack_json = R"({"format":1,"id":"org.example.gui","name":"GUI Pack","version":"1.0.0",)"
                                  R"("links":{"website":"https://a.org","source":"ftp://x"},"server":{"address":"a.org:1"},)"
                                  R"("branding":{"logo":"resources/logo.png","accent":"#ff0000"},)"
                                  R"("update":{"url":")" + u + R"(/pack.json"}})";
    test::write_file(pk / "pack.json", pack_json);
    test::write_file(pk / "files/data/a.lua", "a");
    test::write_file(pk / "resources/logo.png", kPng);
    json review = call("inspect_pack", {{"source", ustr(pk)}});
    check(review["token"] == 1 && review["id"] == "org.example.gui" && review["name"] == "GUI Pack" && review["origin"] == ustr(pk) &&
              review["signer"].is_null() && review["newKey"] == false && review["managed"] == true && review["server"] == "a.org:1" &&
              review["files"] == json::array({"data/a.lua"}) && review["fileCount"] == 1 && review["update"].is_null() &&
              review["runsCode"] == false && review["filesHost"].is_null(),
          "review: " + review.dump());
    check(call("install_pack", {{"token", 1}}) == "gui-pack" && b.prefs().selected_profile == "gui-pack", "install: selected");
    check(host.of("download-progress").empty(), "nothing to download: no download window");
    check(error_of("install_pack", {{"token", 1}}) == "This pack review has expired; open the pack again.", "a token is used once");
    review = call("inspect_pack", {{"source", ustr(pk)}});
    check(review["token"] == 2 && review["update"].dump() ==
                                      R"({"filesAdded":0,"filesRemoved":0,"name":"GUI Pack","patches":0,"profile":"gui-pack","server":"a.org:1","version":"1.0.0"})",
          "a review of an installed pack: the update: " + review["update"].dump());
    call("discard_pack", {{"token", 2}});
    check(error_of("install_pack", {{"token", 2}}) == "This pack review has expired; open the pack again.", "discarded");
    call("discard_pack", {{"token", 99}});

    const json listed = call("list_profiles");
    check(listed.size() == 3 && listed[0]["id"] == "gui-pack" && listed[0]["managed"] == true &&
              listed[0]["links"].dump() == R"([{"key":"website","label":"Website"},{"key":"source","label":"Source"}])" &&
              listed[1].dump() == R"({"description":"","error":null,"id":"my-server","links":[],"localServer":true,"managed":false,)"
                                  R"("name":"Renamed","ownGameData":false,"packId":"local.my-server","patches":0,"server":null,"version":"1.0.0"})",
          "list: " + listed.dump());

    call("open_pack_link", {{"id", "gui-pack"}, {"key", "website"}});
    check(host.last_opened() == "https://a.org", "a pack link opens");
    check(error_of("open_pack_link", {{"id", "gui-pack"}, {"key", "source"}}) == "Not opening ftp://x: only web links are allowed.",
          "only web links");
    check(error_of("open_pack_link", {{"id", "gui-pack"}, {"key", "news"}}) == "This pack has no such link.", "no such link");
    call("open_profile_folder", {{"id", "gui-pack"}});
    check(host.last_opened() == ustr(profiles / "gui-pack"), "a profile folder opens");
    call("open_profile_folder", {{"id", nullptr}});
    check(host.last_opened() == ustr(profiles), "the profiles folder opens");

    const json brand = call("get_profile_branding", {{"id", "gui-pack"}});
    check(brand["logo"] == "data:image/png;base64,iVBORw0KGgpwbmc=" && brand["icon"].is_null() && brand["background"].is_null() &&
              brand["accent"] == "#ff0000",
          "branding: " + brand.dump());
#ifndef _WIN32
    fs::remove(profiles / "gui-pack/resources/logo.png");
    fs::create_symlink(pk / "resources/logo.png", profiles / "gui-pack/resources/logo.png");
    check(call("get_profile_branding", {{"id", "gui-pack"}})["logo"].is_null(), "an image linked from outside resources/ is refused");
#endif

    // A managed pack keeps the player's edits in local.json.
    call("save_profile", input("gui-pack", "Mine", "a.org:1", false, true));
    const sf::pack::Local local = profiles::read_local(profiles / "gui-pack");
    check(local.name == std::string("Mine") && !local.server && !local.local_server && profiles::read_pack(profiles / "gui-pack").name == "GUI Pack",
          "managed: name in local.json, the pack's server not copied");

    // Updates.
    srv.put("/pack.json", R"({"format":1,"id":"org.example.gui","name":"GUI Pack","version":"1.1.0"})");
    check(call("check_pack_update", {{"id", "gui-pack"}}).dump() ==
              R"({"id":"gui-pack","url":")" + u + R"(/pack.json","version":"1.1.0"})",
          "check_pack_update");
    check(call("check_pack_updates").size() == 1, "check_pack_updates: only packs with an update address");
    check(error_of("check_pack_update", {{"id", "my-server"}}) == "Renamed doesn't publish updates.", "no update address");
    srv.put("/pack.json", "nope");
    check(contains(error_of("check_pack_update", {{"id", "gui-pack"}}), u + "/pack.json"), "a failed check is an error");
    check(call("check_pack_updates").empty(), "... but quiet in the background check");

    // A review lists 50 files at most; a pack kept with the launcher says so instead of its folder.
    const fs::path many = base / "data" / "staging" / "samples" / "many";
    test::write_file(many / "pack.json", R"({"format":1,"id":"org.example.many","name":"Many","version":"1.0.0"})");
    for (int i = 0; i < 51; ++i) test::write_file(many / "files/data" / ("f" + std::to_string(100 + i) + ".lua"), "x");
    review = call("inspect_pack", {{"source", ustr(many)}});
    check(review["fileCount"] == 51 && review["files"].size() == 50 && review["files"][49] == "data/f149.lua",
          "a review shows the first 50 files");
    check(review["origin"] == "Included with STAR FORGE", "a bundled sample's origin: " + review["origin"].dump());
    call("discard_pack", {{"token", review["token"]}});

    // A pack whose game files come from a list: the download window.
    srv.put("/a.cfg", "listed");
    srv.put("/list.txt", "SCMOD1\ndata/a.cfg\t" + sha("listed") + "\t" + u + "/a.cfg\n");
    test::write_file(base / "pk2/pack.json", R"({"format":1,"id":"org.example.dl","name":"DL","version":"1.0.0","files_from":{"scmod1":")" +
                                                 u + R"(/list.txt"}})");
    review = call("inspect_pack", {{"source", ustr(base / "pk2")}});
    check(review["filesHost"] == net::host(u) && review["downloadBytes"].is_null(), "review: the file list's host");
    host.clear();
    check(call("install_pack", {{"token", review["token"]}}) == "dl", "install with a file list");
    const auto dl = host.of("download-progress");
    std::string kinds;
    for (const auto& e : dl) kinds += e["kind"].get<std::string>() + " ";
    check(kinds == "start file bytes verified finish " && dl[0]["action"] == "install" && dl[0]["title"] == "DL" && dl[0]["listed"] == 1 &&
              dl[0]["files"] == 1 && dl[0]["signed"] == false && dl[0]["source"] == u + "/list.txt" && dl[3]["size"] == 6 &&
              dl[4]["error"].is_null() && dl[4]["cancelled"] == false && dl[4]["id"] == dl[0]["id"],
          "download events: " + kinds);
    check(test::read_file(profiles / "dl/files/data/a.cfg") == "listed", "the file arrived");

    // Nothing to download: no window. Each download has its own id.
    {
        sf::gui::Backend::Session s(b, "update", "X");
        remote::FileList list;
        remote::SyncEvent plan(remote::SyncEvent::Plan);
        plan.list = &list;
        host.clear();
        s.report(plan);
        s.report(remote::SyncEvent(remote::SyncEvent::File));
        s.finish(std::nullopt);
        check(host.of("download-progress").empty(), "nothing to download: no events");
    }
    // Cancelling a download.
    {
        sf::gui::Backend::Session s(b, "update", "X");
        remote::FileList list;
        remote::SyncEvent plan(remote::SyncEvent::Plan);
        plan.list = &list;
        plan.files = 1;
        host.clear();
        s.report(plan);
        const uint64_t id = host.of("download-progress").at(0)["id"];
        check(id != dl[0]["id"].get<uint64_t>(), "downloads have their own ids");
        call("cancel_download", {{"id", id}});
        check(s.report(remote::SyncEvent(remote::SyncEvent::File)) == std::string(sf::gui::kCancelled) && s.cancelled(),
              "a cancelled download stops at its next step");
        s.finish(std::string(sf::gui::kCancelled));
        check(host.of("download-progress").back()["cancelled"] == true, "... and says so");
    }

    // Samples.
    check(call("list_sample_packs").dump() ==
              R"([{"key":"starfury","name":"STARFURY"},{"key":"sc-classic-europe","name":"Star Conflict Classic Europe"}])",
          "sample packs");
    check(call("sample_pack_source", {{"key", "starfury"}}) == "https://starfury.web.garage.cosmosim.dev/pack.json", "a linked sample");
    const fs::path sample = upath(call("sample_pack_source", {{"key", "sc-classic-europe"}}).get<std::string>());
    check(sample == base / "data/staging/samples/sc-classic-europe" &&
              test::read_file(sample / "pack.json") == test::read_file(fs::path(SF_SOURCE_DIR) / "samples/sc-classic-europe/pack.json"),
          "a bundled sample, written for the review: " + ustr(sample));
    check(error_of("sample_pack_source", {{"key", "x"}}) == "unknown sample pack", "an unknown sample");

    // Links before the interface listens are kept; after, they go straight to it.
    b.receive_link("starforge://install?url=a");
    check(host.of("pack-link").empty(), "a link before the interface listens: kept");
    check(call("take_pending_link") == "starforge://install?url=a" && call("take_pending_link").is_null(), "... handed over once");
    const int restored = host.restored;
    b.receive_link("starforge://install?url=b");
    check(host.of("pack-link").back() == "starforge://install?url=b" && host.restored == restored + 1, "later links: shown at once");

    // Pickers.
    host.pick_answer = "/picked";
    check(call("pick_pack_file") == "/picked" && host.picks.back() == "file Import server pack (Server pack: zip)", "pick a pack file");
    check(call("pick_folder", {{"title", "T"}, {"start", ustr(base)}}) == "/picked" && host.picks.back() == "folder T at " + ustr(base),
          "pick a folder, starting somewhere");
    call("pick_folder", {{"title", "T"}, {"start", ustr(base / "gone")}});
    check(host.picks.back() == "folder T", "... only from a folder that exists");
    host.pick_answer.reset();
    check(call("pick_background_folder").is_null() && host.picks.back() == "folder Background folder", "a cancelled pick");

    // Deleting forgets the pack's key and the selection.
    call("update_launcher_preferences", {{"update", {{"key", "selectedProfile"}, {"value", "dl"}}}});
    trust::pin("org.example.dl", signing::SecretKey::generate().public_key());
    call("delete_profile", {{"id", "dl"}});
    check(!fs::exists(profiles / "dl") && b.prefs().selected_profile.empty(), "delete: gone, selection cleared");
    check(!trust::pinned("org.example.dl"), "delete: the pack's key is forgotten");
    check(error_of("delete_profile", {{"id", "../data"}}) == "profile \"../data\" does not exist", "delete: only profiles");
}

// The play button, on a fake game.
void game(const fs::path& base) {
    auto& b = *g_backend;
    const fs::path profiles = base / "data" / "profiles";

    // Nothing installed.
    host.clear();
    call("initialize_launcher");
    b.wait_idle();
    auto states = host.of("launcher-state-changed");
    json snap = call("get_launcher_snapshot");
    check(states.size() == 2 && states[0]["phase"] == "checking" && states[0]["busy"] == true && snap["phase"] == "failed" &&
              snap["revision"] == states[1]["revision"],
          "no game: checking, then failed: " + snap.dump());
#ifndef _WIN32
    check(snap["primaryAction"] == "fix" && snap["fixLabel"] == "Install from Steam", "a Steam fix is offered: " + snap.dump());
    call("perform_launcher_action", {{"action", "fix"}});
    check(host.last_opened() == "steam://install/212070" && call("get_launcher_snapshot")["primaryAction"] == "retry",
          "fix opens Steam, then the button is Retry");
#endif
    check(error_of("perform_launcher_action", {{"action", "launch"}}) == "This action is not available right now.", "no launch while failed");

    // A game, the tools, and on Linux a Proton in the game's library.
    const fs::path library = base / "home/.local/share/Steam";
    const fs::path game = library / "steamapps/common/star conflict", tools = base / "tools";
    test::write_file(game / "win64/game.exe", "MZ");
    fs::create_directories(game / "data");
#ifdef _WIN32
    // The "game": proc_helper, failing to start.
    fs::create_directories(tools);
    fs::copy_file(g_helper, tools / "sc-inject.exe", fs::copy_options::overwrite_existing);
    set_env("SF_EXIT", "3");
#else
    test::write_file(tools / "sc-inject.exe", "MZ");
#endif
    test::write_file(tools / "sc_agent.dll", "MZ");
    b.tool_folders = {base / "nowhere", tools};
#ifndef _WIN32
    const fs::path proton = library / "steamapps/common/Proton 9.0", run = library / "steamapps/common/SteamLinuxRuntime_4/run";
    test::write_file(proton / "proton", "");
    test::write_file(proton / "toolmanifest.vdf", "\"manifest\"\n{\n  \"require_tool_appid\" \"4183110\"\n}\n");
    test::write_file(library / "steamapps/appmanifest_4183110.acf",
                     "\"AppState\"\n{\n\t\"appid\"\t\t\"4183110\"\n\t\"installdir\"\t\t\"SteamLinuxRuntime_4\"\n}");
    // The "game": a while, then a failed start.
    test::write_file(run, "#!/bin/sh\nsleep 1\nexit 3\n");
    fs::permissions(run, fs::perms::owner_all);
    test::write_file(library / "steamapps/compatdata/212070/config_info", "v1\n" + ustr(proton) + "/files/share/fonts/\n");
    fs::create_directories(library / "steamapps/compatdata/212070/pfx");
    test::write_file(library / "steamapps/appmanifest_212070.acf",
                     "\"AppState\"\n{\n\t\"appid\"\t\t\"212070\"\n\t\"installdir\"\t\t\"star conflict\"\n}");
#endif
    test::write_file(library / "appcache/librarycache/212070_library_hero.jpg", kJpeg);
    test::write_file(library / "appcache/librarycache/212070/abc/logo.png", kPng);

    host.clear();
    call("update_launcher_preferences", {{"update", {{"key", "gameDirectory"}, {"value", ustr(game)}}}});
    b.wait_idle();
    snap = call("get_launcher_snapshot");
    check(snap["phase"] == "ready" && snap["primaryAction"] == "launch" && snap["gameDirectory"] == ustr(game) &&
              snap["blockingReason"].is_null(),
          "a new game folder is detected at once: " + snap.dump());
#ifndef _WIN32
    check(snap["proton"] == ustr(proton) + " inside SteamLinuxRuntime_4", "the Proton in use: " + snap["proton"].dump());
    const json compat = call("detect_compat_tools");
    check(compat["supported"] == true && contains(compat["proton"].dump(), ustr(proton)), "Proton builds to choose from");
#else
    check(snap["proton"].is_null() && call("detect_compat_tools").dump() == R"({"proton":[],"supported":false})", "no Proton on Windows");
#endif
    const json art = call("get_artwork");
#ifndef _WIN32
    check(art["hero"] == "data:image/jpeg;base64,/9j/4GpwZWc=" && art["logo"] == "data:image/png;base64,iVBORw0KGgpwbmc=",
          "Steam's artwork: " + art.dump());
#endif
    // The player's folder: images that are too big or not images are passed over.
    test::write_file(base / "bad/big.png", kPng + std::string(8 << 20, 'x'));
    test::write_file(base / "bad/fake.png", "not an image");
    call("update_launcher_preferences", {{"update", {{"key", "backgroundFolder"}, {"value", ustr(base / "bad")}}}});
    check(call("get_artwork")["hero"] == art["hero"], "no usable image in the folder: Steam's art");
    test::write_file(base / "bg/b.PNG", kPng);
    test::write_file(base / "bg/a.txt", "x");
    call("update_launcher_preferences", {{"update", {{"key", "backgroundFolder"}, {"value", ustr(base / "bg")}}}});
    check(call("get_artwork")["hero"] == "data:image/png;base64,iVBORw0KGgpwbmc=", "the player's own background first");
    call("view_game_install");
    check(host.last_opened() == ustr(game), "view the game folder");
    check(error_of("view_launch_log") == "The game has not been started from the launcher yet.", "no launch log yet");

    check(error_of("perform_launcher_action", {{"action", "launch"}}) == "", "launch...");
    b.wait_idle();
    check(host.of("notification").back() == json::array({"Choose a server profile first.", 2}) && phase() == "ready",
          "... needs a profile");

    call("update_launcher_preferences", {{"update", {{"key", "selectedProfile"}, {"value", "my-server"}}}});
    call("update_launcher_preferences", {{"update", {{"key", "launchBehavior"}, {"value", "tray"}}}});
    test::write_file(profiles / "my-server/inject.log", "error: boom\n");
    const json pending = call("inspect_pack", {{"source", ustr(base / "pk")}});
    host.clear();
    call("perform_launcher_action", {{"action", "launch"}});
    b.wait_idle();
#ifndef _WIN32
    // The runtime script takes a second, so the game is seen running.
    check(phase() == "running" && host.hidden == 1, "running, the launcher in the tray");
    check(error_of("delete_profile", {{"id", "my-server-2"}}) == "Close the game before deleting a profile.", "no deleting while it runs");
    check(error_of("finish_setup", {{"gameDir", ""}, {"profilesDir", "/x"}, {"moveProfiles", false}}) ==
              "Close the game before changing the setup.",
          "no setup changes while it runs");
    check(error_of("install_pack", {{"token", pending["token"]}}) == "Close the game before installing or updating a pack.",
          "no installs while it runs");
#endif
    check(wait_for([] { return phase() == "ready"; }), "the game exits");
    check(wait_for([] { return host.restored >= 2; }) && host.hidden == 1, "the launcher went to the tray and comes back");
    check(host.of("notification").back() == json::array({"The game did not start: boom", 2}), "its failure is shown");
    std::string phases;
    for (const auto& s : host.of("launcher-state-changed")) phases += s["phase"].get<std::string>() + " ";
    check(phases == "launching running ready ", "phases: " + phases);
    call("view_launch_log");
    check(host.last_opened() == ustr(base / "data" / "game-launch.log") && fs::exists(base / "data" / "game-launch.log"), "the launch log opens");
}

void setup(const fs::path& base) {
    auto& b = *g_backend;
    const fs::path old = base / "data" / "profiles", moved = base / "elsewhere";
    check(error_of("finish_setup", {{"gameDir", ""}, {"profilesDir", "rel"}, {"moveProfiles", true}}) ==
              "Pick a full path for the profiles folder.",
          "a relative profiles folder");
    check(contains(error_of("finish_setup", {{"gameDir", ustr(base)}, {"profilesDir", ustr(moved)}, {"moveProfiles", true}}),
                   "doesn't hold Star Conflict"),
          "a game folder that isn't one");
    const fs::path game = base / "home/.local/share/Steam/steamapps/common/star conflict";
    call("finish_setup", {{"gameDir", ustr(game)}, {"profilesDir", ustr(moved)}, {"moveProfiles", true}});
    b.wait_idle();
    check(fs::exists(moved / "my-server/pack.json") && !fs::exists(old / "my-server"), "profiles moved");
    check(profiles::default_profiles_dir() == moved && call("get_setup_info")["profilesDir"] == ustr(moved), "the new folder is used");
#ifndef _WIN32
    check(b.prefs().setup_done && b.prefs().game_directory.empty(), "setup done; the folder Steam knows isn't stored");
#else
    check(b.prefs().setup_done && b.prefs().game_directory == ustr(game), "setup done, the chosen game folder kept");
#endif
    const fs::path other = base / "game2";
    test::write_file(other / "win64" / "game.exe", "MZ");
    fs::create_directories(other / "data");
    call("finish_setup", {{"gameDir", ustr(other)}, {"profilesDir", ustr(moved)}, {"moveProfiles", false}});
    b.wait_idle();
    check(b.prefs().game_directory == ustr(other), "another game folder is stored");
    call("finish_setup", {{"gameDir", ""}, {"profilesDir", ustr(old)}, {"moveProfiles", false}});
    b.wait_idle();
    check(profiles::default_profiles_dir() == old && fs::exists(moved / "my-server") && b.prefs().game_directory.empty(),
          "back to the standard folder, without moving; an empty game folder means find it");
}

// Exit after Play: the launcher quits and the game goes on alone (so the launcher never sees it
// stop; this runs last).
void exit_after_play(const fs::path& game) {
    call("update_launcher_preferences", {{"update", {{"key", "gameDirectory"}, {"value", ustr(game)}}}});
    g_backend->wait_idle();
    call("update_launcher_preferences", {{"update", {{"key", "launchBehavior"}, {"value", "exit"}}}});
    const json id = call("save_profile", {{"profile", {{"name", "Last"}, {"server", ""}, {"localServer", false}, {"ownGameData", true}}}});
    call("update_launcher_preferences", {{"update", {{"key", "selectedProfile"}, {"value", id}}}});
    call("perform_launcher_action", {{"action", "launch"}});
    g_backend->wait_idle();
    check(host.exited == 1 && phase() == "running", "launch behaviour exit: the launcher exits");
}

// Rust (lifecycle.rs): waits_for_ready_and_sting_in_either_order, watchdog_reveals_without_ui_ready.
void startup_gate() {
    sf::app::StartupGate g;
    check(!g.native_ready() && !g.ready() && g.finish_sting() && !g.finish_sting(), "gate: ready, then sting; once");
    sf::app::StartupGate h;
    h.native_ready();
    check(!h.finish_sting() && h.ready(), "gate: sting, then ready");
    sf::app::StartupGate w;
    w.native_ready();
    check(w.watchdog(), "gate: the watchdog reveals without the interface");
}

}  // namespace

int run(int argc, char** argv) {
#ifdef _WIN32
    if (argc < 2) {
        std::printf("usage: gui_test <proc_helper.exe>\n");
        return 2;
    }
    g_helper = fs::absolute(fs::u8path(argv[1]));
#else
    (void)argc;
    (void)argv;
#endif
    const fs::path base = test::fresh_dir(fs::temp_directory_path() / "sf-gui-test");
    set_env(env::kDataDir, ustr(base / "data"));
    set_env(env::kProfilesDir, std::nullopt);
    set_env(env::kToolsDir, std::nullopt);
    set_env(env::kGameDir, std::nullopt);
    set_env(env::kProton, std::nullopt);
    set_env("APPIMAGE", std::nullopt);
#ifndef _WIN32
    set_env("HOME", ustr(base / "home"));  // Steam is looked for here
#endif
    test::FileServer srv;
    sf::gui::Backend backend(host);
    g_backend = &backend;

    try {
        prefs(base / "data");
        profiles_and_packs(base, srv);
        game(base);
        setup(base);
        startup_gate();
        exit_after_play(base / "home/.local/share/Steam/steamapps/common/star conflict");
    } catch (const std::exception& e) {
        check(false, std::string("unexpected error: ") + e.what());
    }

    std::printf("%s (%d failed)\n", test::failures ? "FAILED" : "ok", test::failures);
    return test::failures ? 1 : 0;
}

int main(int argc, char** argv) {
    test::exit_on_crash();
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::printf("FAIL: unexpected error: %s\n", e.what());
        return 1;
    }
}
