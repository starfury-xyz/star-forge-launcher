// A stand-in for the server programs in server_test, by its own file name: mongod.exe listens on
// --port, hub.exe starts shard.exe beside it (a service Hub doesn't stop), shard.exe just runs.
// Each appends "up" and then "ctrl-c" (when it gets one) to <SF_FAKE_DIR>\<name>.txt, and exits
// on Ctrl+C.

#include <winsock2.h>
#include <windows.h>

#include <cstdio>
#include <string>

namespace {

std::wstring g_marker;
HANDLE g_done = nullptr;

void note(const char* what) {
    HANDLE f = CreateFileW(g_marker.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD n = 0;
    WriteFile(f, what, DWORD(strlen(what)), &n, nullptr);
    WriteFile(f, "\n", 1, &n, nullptr);
    CloseHandle(f);
}

BOOL WINAPI on_ctrl(DWORD type) {
    if (type != CTRL_C_EVENT) return FALSE;
    note("ctrl-c");
    SetEvent(g_done);
    return TRUE;
}

DWORD WINAPI serve(void* p) {
    const unsigned port = unsigned(reinterpret_cast<uintptr_t>(p));
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(u_short(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(s, 8) != 0) {
        note("listen failed");
        return 1;
    }
    for (;;) {
        SOCKET c = accept(s, nullptr, nullptr);
        if (c != INVALID_SOCKET) closesocket(c);
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring exe = self, name = exe.substr(exe.find_last_of(L'\\') + 1);
    CharLowerBuffW(name.data(), DWORD(name.size()));
    wchar_t dir[MAX_PATH];
    GetEnvironmentVariableW(L"SF_FAKE_DIR", dir, MAX_PATH);
    g_marker = std::wstring(dir) + L"\\" + name.substr(0, name.size() - 4) + L".txt";
    g_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(on_ctrl, TRUE);

    if (name == L"mongod.exe")
        for (int i = 1; i + 1 < argc; ++i)
            if (std::wstring(argv[i]) == L"--port")
                CreateThread(nullptr, 0, serve, reinterpret_cast<void*>(uintptr_t(_wtoi(argv[i + 1]))), 0, nullptr);
    if (name == L"hub.exe") {
        std::wstring shard = exe.substr(0, exe.find_last_of(L'\\') + 1) + L"Shard.exe";
        STARTUPINFOW si{sizeof si};
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(shard.c_str(), nullptr, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        } else {
            note("could not start shard");
        }
    }
    note("up");
    WaitForSingleObject(g_done, 60000);
    return 0;
}
