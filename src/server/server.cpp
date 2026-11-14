#include "server/server.hpp"

#include <winsock2.h>
#include <windows.h>

#include <tlhelp32.h>

#include <cstdarg>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace sf::server {
namespace {

// Timeouts, as the game's own local_server.xml has them.
constexpr DWORD kMongoStartMs = 15000, kHubCheckMs = 1000, kHubStopMs = 15000, kMongoStopMs = 10000;
constexpr DWORD kLeftoverStopMs = 5000, kHeartbeatMs = 3000;

std::wstring g_log;  // <cloud>\starforge\logs\sc-server.log

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

// A line to the log file and stdout.
void log(const char* fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[2200];
    std::snprintf(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02d.%03d %s\n", t.wYear, t.wMonth, t.wDay, t.wHour,
                  t.wMinute, t.wSecond, t.wMilliseconds, msg);
    std::fputs(line, stdout);
    std::fflush(stdout);
    if (g_log.empty()) return;
    HANDLE f = CreateFileW(g_log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD n = 0;
    WriteFile(f, line, DWORD(std::strlen(line)), &n, nullptr);
    CloseHandle(f);
}

std::optional<std::string> read_file(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

std::wstring root(const Settings& s) { return s.cloud + L"\\starforge"; }
std::wstring state_file(const Settings& s) { return root(s) + L"\\" + widen(kStateFile); }

// One supervisor per cloud folder: a named event, by a hash of the folder's lowercased path.
std::wstring stop_event_name(const Settings& s) {
    std::wstring p = s.cloud;
    CharLowerBuffW(p.data(), DWORD(p.size()));
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    uint64_t h = 1469598103934665603ull;  // FNV-1a
    for (wchar_t c : p) h = (h ^ uint64_t(c)) * 1099511628211ull;
    wchar_t name[64];
    std::swprintf(name, 64, L"Local\\StarForgeServerStop-%016llx", static_cast<unsigned long long>(h));
    return name;
}

bool port_open(unsigned port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(u_short(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
    closesocket(s);
    return ok;
}

struct Child {
    std::string name;
    HANDLE process = nullptr;
    DWORD pid = 0;
    bool running() const { return process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT; }
};

// Starts a console program with a console of its own and no window, so it can be sent Ctrl+C
// alone.
Child start(const char* name, const std::wstring& exe, const std::wstring& args, const std::wstring& cwd) {
    std::wstring cmd = L"\"" + exe + L"\"" + (args.empty() ? L"" : L" " + args);
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, cwd.c_str(), &si, &pi))
        throw std::runtime_error("could not start " + narrow(exe) + " (error " + std::to_string(GetLastError()) + ")");
    CloseHandle(pi.hThread);
    log("started %s, process %lu: %s", name, pi.dwProcessId, narrow(cmd).c_str());
    return Child{name, pi.hProcess, pi.dwProcessId};
}

// Ctrl+C to one process, as the client does: join its console, send, leave.
bool ctrl_c(DWORD pid) {
    FreeConsole();
    if (!AttachConsole(pid)) {
        log("could not attach to the console of process %lu (error %lu)", pid, GetLastError());
        return false;
    }
    SetConsoleCtrlHandler(nullptr, TRUE);  // not us
    const bool ok = GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
    if (!ok) log("could not send Ctrl+C to process %lu (error %lu)", pid, GetLastError());
    FreeConsole();
    SetConsoleCtrlHandler(nullptr, FALSE);
    return ok;
}

// Ctrl+C, then a wait; killed if it outlasts the wait.
void stop_child(Child& c, DWORD timeout_ms) {
    if (!c.running()) return;
    log("stopping %s (process %lu)", c.name.c_str(), c.pid);
    if (ctrl_c(c.pid) && WaitForSingleObject(c.process, timeout_ms) == WAIT_OBJECT_0) {
        log("%s stopped", c.name.c_str());
        return;
    }
    log("%s did not stop in %lu s; killing it", c.name.c_str(), timeout_ms / 1000);
    TerminateProcess(c.process, 1);
    WaitForSingleObject(c.process, 5000);
}

// Programs from this cloud folder still running (services Hub left behind), but not `except`.
std::vector<Child> leftovers(const Settings& s, DWORD except) {
    std::vector<Child> out;
    std::wstring prefix = s.cloud + L"\\";
    CharLowerBuffW(prefix.data(), DWORD(prefix.size()));
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W e{sizeof e};
    for (BOOL more = Process32FirstW(snap, &e); more; more = Process32NextW(snap, &e)) {
        if (e.th32ProcessID == except || e.th32ProcessID == GetCurrentProcessId()) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | PROCESS_TERMINATE, FALSE, e.th32ProcessID);
        if (!h) continue;
        wchar_t path[MAX_PATH * 2];
        DWORD n = MAX_PATH * 2;
        std::wstring p = QueryFullProcessImageNameW(h, 0, path, &n) ? std::wstring(path, n) : L"";
        CharLowerBuffW(p.data(), DWORD(p.size()));
        if (p.rfind(prefix, 0) == 0 && p.find(L'\\', prefix.size()) == std::wstring::npos) {
            out.push_back(Child{narrow(e.szExeFile), h, e.th32ProcessID});
        } else {
            CloseHandle(h);
        }
    }
    CloseHandle(snap);
    return out;
}

void write_state(const Settings& s, const Child& hub, const Child* mongod, time_t started) {
    char text[512];
    std::snprintf(text, sizeof text,
                  "{\"supervisor\": %lu, \"hub\": %lu, \"mongod\": %lu, \"mongoPort\": %u, \"started\": %lld, "
                  "\"heartbeat\": %lld}\n",
                  GetCurrentProcessId(), hub.pid, mongod ? mongod->pid : 0ul, s.mongo_port,
                  static_cast<long long>(started), static_cast<long long>(std::time(nullptr)));
    const std::wstring file = state_file(s), tmp = file + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text;
    }
    MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
}

HANDLE g_stop = nullptr;

BOOL WINAPI on_console_event(DWORD) {  // Ctrl+C in the supervisor's own console: a normal stop
    if (g_stop) SetEvent(g_stop);
    return TRUE;
}

// Waits for a port to accept connections while `c` keeps running.
bool wait_for_port(unsigned port, const Child& c, DWORD timeout_ms) {
    for (DWORD waited = 0; waited < timeout_ms; waited += 250) {
        if (port_open(port)) return true;
        if (!c.running()) return false;
        Sleep(250);
    }
    return port_open(port);
}

}  // namespace

std::optional<std::pair<std::string, unsigned>> mongo_host(const std::string& cloud_xml) {
    const std::string text = std::regex_replace(cloud_xml, std::regex("<!--[\\s\\S]*?-->"), "");
    std::smatch m;
    if (!std::regex_search(text, m, std::regex("name\\s*=\\s*\"db_mongoHosts\"\\s+value\\s*=\\s*\"([^\":,]+):(\\d+)")))
        return std::nullopt;
    return std::make_pair(m[1].str(), unsigned(std::stoul(m[2].str())));
}

Settings settings(const std::wstring& cloud_in) {
    Settings s;
    s.cloud = cloud_in;
    while (!s.cloud.empty() && (s.cloud.back() == L'\\' || s.cloud.back() == L'/')) s.cloud.pop_back();
    const auto xml = read_file(s.cloud + L"\\data\\cloud.xml");
    if (!xml) throw std::runtime_error(narrow(s.cloud) + "\\data\\cloud.xml is missing: is this the game's cloud folder?");
    if (auto host = mongo_host(*xml)) {
        s.mongo_port = host->second;
        s.mongo_local = host->first == "127.0.0.1" || host->first == "localhost";
    }
    wchar_t local[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) throw std::runtime_error("LOCALAPPDATA is not set");
    s.mongo_data = std::wstring(local) + L"\\Targem\\StarConflict\\cloud\\mongodb\\data";
    return s;
}

int run(const Settings& s) {
    CreateDirectoryW(root(s).c_str(), nullptr);
    CreateDirectoryW((root(s) + L"\\logs").c_str(), nullptr);
    g_log = root(s) + L"\\logs\\sc-server.log";
    g_stop = CreateEventW(nullptr, TRUE, FALSE, stop_event_name(s).c_str());
    if (!g_stop) return log("could not create the stop event (error %lu)", GetLastError()), 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) return log("a server is already running from %s", narrow(s.cloud).c_str()), 1;
    SetConsoleCtrlHandler(on_console_event, TRUE);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    log("starting the server in %s", narrow(s.cloud).c_str());

    Child mongod;
    bool own_mongo = false;
    int code = 0;
    try {
        if (!s.mongo_local) {
            log("MongoDB is on another machine (cloud.xml db_mongoHosts); not starting one");
        } else if (port_open(s.mongo_port)) {
            log("port %u is already taken; using the MongoDB that is there", s.mongo_port);
        } else {
            // The data folder, a level at a time.
            for (size_t i = 0; (i = s.mongo_data.find(L'\\', i + 1)) != std::wstring::npos;)
                CreateDirectoryW(s.mongo_data.substr(0, i).c_str(), nullptr);
            CreateDirectoryW(s.mongo_data.c_str(), nullptr);
            const std::wstring mongo_dir = s.cloud + L"\\mongodb";
            mongod = start("MongoDB", mongo_dir + L"\\mongod.exe",
                           L"--config \"config.cfg\" --dbpath \"" + s.mongo_data + L"\" --port " + std::to_wstring(s.mongo_port) +
                               L" --logpath \"" + root(s) + L"\\logs\\mongod.log\" --logappend",
                           mongo_dir);
            own_mongo = true;
            if (!wait_for_port(s.mongo_port, mongod, kMongoStartMs))
                throw std::runtime_error("MongoDB did not become ready on port " + std::to_string(s.mongo_port) +
                                         " (see starforge\\logs\\mongod.log)");
        }

        Child hub = start("Hub", s.cloud + L"\\Hub.exe", L"", s.cloud);
        if (WaitForSingleObject(hub.process, kHubCheckMs) == WAIT_OBJECT_0) throw std::runtime_error("Hub exited during startup");
        const time_t started = std::time(nullptr);
        write_state(s, hub, own_mongo ? &mongod : nullptr, started);
        log("server running");

        HANDLE waits[2] = {hub.process, g_stop};
        for (;;) {
            DWORD r = WaitForMultipleObjects(2, waits, FALSE, kHeartbeatMs);
            if (r == WAIT_TIMEOUT) {
                write_state(s, hub, own_mongo ? &mongod : nullptr, started);
                continue;
            }
            if (r == WAIT_OBJECT_0) {
                DWORD hub_code = 0;
                GetExitCodeProcess(hub.process, &hub_code);
                log("Hub exited by itself (code %lu)", hub_code);
                code = 1;
            } else {
                log("stop requested");
                stop_child(hub, kHubStopMs);
            }
            break;
        }
        for (Child& c : leftovers(s, own_mongo ? mongod.pid : 0)) {
            log("%s is still running after Hub", c.name.c_str());
            stop_child(c, kLeftoverStopMs);
            CloseHandle(c.process);
        }
        CloseHandle(hub.process);
    } catch (const std::exception& e) {
        log("%s", e.what());
        code = 1;
    }
    if (own_mongo) stop_child(mongod, kMongoStopMs);
    if (mongod.process) CloseHandle(mongod.process);
    DeleteFileW(state_file(s).c_str());
    log("server stopped");
    return code;
}

int stop(const Settings& s) {
    HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, stop_event_name(s).c_str());
    if (!ev) {
        std::printf("no server is running from %s\n", narrow(s.cloud).c_str());
        DeleteFileW(state_file(s).c_str());  // left by one that was killed
        return 0;
    }
    // The supervisor's pid, to wait for it.
    DWORD pid = 0;
    if (auto text = read_file(state_file(s))) {
        std::smatch m;
        if (std::regex_search(*text, m, std::regex("\"supervisor\":\\s*(\\d+)"))) pid = DWORD(std::stoul(m[1].str()));
    }
    HANDLE sup = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
    SetEvent(ev);
    CloseHandle(ev);
    std::printf("stopping the server in %s\n", narrow(s.cloud).c_str());
    if (!sup) return 0;
    const DWORD r = WaitForSingleObject(sup, kHubStopMs + kMongoStopMs + 60000);
    CloseHandle(sup);
    if (r != WAIT_OBJECT_0) return std::printf("the server is taking long to stop; see starforge\\logs\\sc-server.log\n"), 1;
    std::printf("server stopped\n");
    return 0;
}

}  // namespace sf::server
