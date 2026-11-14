// sc-server.exe with stand-in server programs (fake_server.exe as mongod, Hub and Shard): starts
// the stack, refuses a second supervisor, stops it with Ctrl+C (Hub, then the Shard Hub left
// behind, then MongoDB) and cleans up server.json.
//
//   server_test <sc-server.exe> <fake_server.exe>

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include "check.hpp"
#include "server/server.hpp"

namespace fs = std::filesystem;
using test::check;
using test::contains;

namespace {

// Runs sc-server.exe with `args`; waits for it unless `wait` is false (then its handle).
DWORD sc_server(const fs::path& exe, const std::wstring& args, bool wait, HANDLE* process = nullptr) {
    std::wstring cmd = L"\"" + exe.wstring() + L"\" " + args;
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.wstring().c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        check(false, "could not start sc-server.exe");
        return 999;
    }
    CloseHandle(pi.hThread);
    if (!wait) {
        *process = pi.hProcess;
        return 0;
    }
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = 999;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return code;
}

bool wait_for(const fs::path& file, const std::string& text, DWORD ms) {
    for (DWORD t = 0; t < ms; t += 100) {
        std::error_code ec;
        if (fs::exists(file, ec) && (text.empty() || contains(test::read_file(file), text))) return true;
        Sleep(100);
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    test::exit_on_crash();
    if (argc != 3) return std::fputs("usage: server_test <sc-server.exe> <fake_server.exe>\n", stderr), 2;
    const fs::path sc = fs::absolute(argv[1]), fake = fs::absolute(argv[2]);
    const fs::path base = test::fresh_dir(fs::temp_directory_path() / "sf_server_test");
    const fs::path cloud = base / "cloud", marks = base / "marks", local = base / "local";
    fs::create_directories(marks);
    fs::create_directories(cloud / "mongodb");
    fs::copy_file(fake, cloud / "Hub.exe");
    fs::copy_file(fake, cloud / "Shard.exe");
    fs::copy_file(fake, cloud / "mongodb" / "mongod.exe");
    test::write_file(cloud / "mongodb" / "config.cfg", "dbpath = ./data\n");
    // A port of its own, so a real MongoDB on 27017 doesn't matter.
    test::write_file(cloud / "data" / "cloud.xml",
                     "<Cloud>\n<!-- <CVar name=\"db_mongoHosts\" value=\"10.0.0.1:1\" /> -->\n<CommonCVars>\n"
                     "<CVar name=\"db_mongoHosts\" value=\"127.0.0.1:27399\" />\n</CommonCVars>\n</Cloud>\n");
    SetEnvironmentVariableW(L"SF_FAKE_DIR", marks.wstring().c_str());
    SetEnvironmentVariableW(L"LOCALAPPDATA", local.wstring().c_str());

    // settings(): the port from cloud.xml (not the commented-out one), the game's data folder.
    {
        const auto s = sf::server::settings(cloud.wstring() + L"\\");
        check(s.mongo_port == 27399 && s.mongo_local, "settings: db_mongoHosts port, comments skipped");
        check(s.cloud == cloud.wstring(), "settings: trailing slash dropped");
        check(s.mongo_data == local.wstring() + L"\\Targem\\StarConflict\\cloud\\mongodb\\data", "settings: mongo data folder");
        check(!sf::server::mongo_host("<CVar name=\"x\" value=\"1\" />"), "mongo_host: none");
        auto remote = sf::server::mongo_host("<CVar name=\"db_mongoHosts\" value=\"db.example:27017,b:2\" />");
        check(remote && remote->first == "db.example" && remote->second == 27017, "mongo_host: first of several");
    }

    check(sc_server(sc, L"stop \"" + cloud.wstring() + L"\"", true) == 0, "stop: nothing running is fine");

    HANDLE sup = nullptr;
    sc_server(sc, L"run \"" + cloud.wstring() + L"\"", false, &sup);
    const fs::path state = cloud / "starforge" / "server.json";
    check(wait_for(state, "", 30000), "run: server.json written");
    check(wait_for(marks / "hub.txt", "up", 5000) && wait_for(marks / "shard.txt", "up", 5000) &&
              wait_for(marks / "mongod.txt", "up", 5000),
          "run: mongod, Hub and the Shard it starts are up");
    check(fs::is_directory(local / "Targem" / "StarConflict" / "cloud" / "mongodb" / "data"), "run: mongo data folder made");
    const std::string json = test::read_file(state);
    check(contains(json, "\"mongoPort\": 27399") && contains(json, "\"heartbeat\": "), "server.json: " + json);
    check(sc_server(sc, L"run \"" + cloud.wstring() + L"\"", true) == 1, "run: a second supervisor is refused");

    check(sc_server(sc, L"stop \"" + cloud.wstring() + L"\"", true) == 0, "stop: exit 0");
    check(WaitForSingleObject(sup, 5000) == WAIT_OBJECT_0, "stop: the supervisor has exited");
    DWORD code = 999;
    GetExitCodeProcess(sup, &code);
    CloseHandle(sup);
    check(code == 0, "run: exit 0 after a stop");
    check(contains(test::read_file(marks / "hub.txt"), "ctrl-c"), "stop: Hub got Ctrl+C");
    check(contains(test::read_file(marks / "shard.txt"), "ctrl-c"), "stop: the Shard Hub left got Ctrl+C");
    check(contains(test::read_file(marks / "mongod.txt"), "ctrl-c"), "stop: MongoDB got Ctrl+C");
    check(!fs::exists(state), "stop: server.json removed");
    const std::string log = test::read_file(cloud / "starforge" / "logs" / "sc-server.log");
    check(contains(log, "started MongoDB") && contains(log, "Hub stopped") && contains(log, "Shard.exe is still running after Hub") &&
              contains(log, "MongoDB stopped") && contains(log, "server stopped"),
          "log:\n" + log);

    std::printf("%s (%d failed)\n", test::failures ? "FAILED" : "ok", test::failures);
    return test::failures ? 1 : 0;
}
