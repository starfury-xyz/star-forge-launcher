#pragma once

// sc-server.exe: starts and stops a server install's stack the way the game client does for its
// local server (win64\local_server.xml, `game.exe -launchLocalServer`): MongoDB first, unless its
// port is already taken, then Hub.exe, which starts the services in data\cloud.xml. Stopping is
// Ctrl+C to Hub, then to MongoDB, as the client does; a process that doesn't stop in time is
// killed. New in the C++ launcher; Windows only (on Linux it runs in the game's Proton prefix,
// so it shares a wine session, and so a console, with the servers).
//
//   sc-server.exe run <cloud dir>    the supervisor: starts the stack, stays until Hub exits, then
//                                    stops the MongoDB it started
//   sc-server.exe stop <cloud dir>   asks a running supervisor's Hub to stop, waits for the end
//   sc-server.exe ctrl-c <pid>       the supervisor's helper: Ctrl+C to that process's console
//
// While it runs, the supervisor keeps <cloud>\starforge\server.json up to date (pids and a
// heartbeat every few seconds) for the launcher, and logs to <cloud>\starforge\logs\sc-server.log.

#include <optional>
#include <string>

namespace sf::server {

inline constexpr const char* kStateFile = "server.json";  // in <cloud>\starforge

struct Settings {
    std::wstring cloud;          // the server install's cloud folder
    unsigned mongo_port = 27017;  // from data\cloud.xml's db_mongoHosts, else the game's default
    bool mongo_local = true;     // false: db_mongoHosts names another machine, so no mongod here
    std::wstring mongo_data;     // %LOCALAPPDATA%\Targem\StarConflict\cloud\mongodb\data
};

// The settings for a cloud folder, from its data\cloud.xml. Throws std::runtime_error.
Settings settings(const std::wstring& cloud);
// db_mongoHosts' first host:port from cloud.xml's text, if it has one.
std::optional<std::pair<std::string, unsigned>> mongo_host(const std::string& cloud_xml);

int run(const Settings& s);    // exit code: 0 after a clean stop
int stop(const Settings& s);   // exit code: 0 when the stack is down
int send_ctrl_c(unsigned long pid);  // exit code: 0 once sent, else the Windows error

}  // namespace sf::server
