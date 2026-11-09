// Stands in for game.exe in inject_test: writes what it was started with to
// <SC_PROFILE>\fake_game.txt (one "key=value" per line, args in order), then exits. A windowed
// program, like the game: a console one brings a console host process along, which wine counts
// in the injector's job.
//
//   --exit=N       exit with code N (default 0)
//   --leave-child  start a copy of itself that keeps running (like the game's local server),
//                  for the injector to clean up
//   --linger       (that copy) write its pid to <SC_PROFILE>\linger.pid, then wait a minute

#include <windows.h>

#include <shellapi.h>

#include <string>

#include "common/file.hpp"
#include "common/text.hpp"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    wchar_t buf[4096];
    DWORD n = GetEnvironmentVariableW(L"SC_PROFILE", buf, 4096);
    const std::wstring profile(buf, n < 4096 ? n : 0);

    if (argc > 1 && std::wstring(argv[1]) == L"--linger") {
        sf::write_file(profile + L"\\linger.pid", std::to_string(GetCurrentProcessId()));
        Sleep(60000);
        return 0;
    }

    std::string out = "profile_env=" + sf::utf8(profile) + "\n";
    n = GetCurrentDirectoryW(4096, buf);
    out += "cwd=" + sf::utf8(std::wstring(buf, n)) + "\n";
    int code = 0;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        out += "arg=" + sf::utf8(a) + "\n";
        if (a.rfind(L"--exit=", 0) == 0) code = std::stoi(a.substr(7));
        if (a == L"--leave-child") {
            wchar_t self[MAX_PATH];
            GetModuleFileNameW(nullptr, self, MAX_PATH);
            std::wstring cmd = L"\"" + std::wstring(self) + L"\" --linger";
            STARTUPINFOW si{sizeof si};
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                // Don't exit before the copy has started and said who it is.
                for (int t = 0; t < 100 && GetFileAttributesW((profile + L"\\linger.pid").c_str()) == INVALID_FILE_ATTRIBUTES; ++t)
                    Sleep(50);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }
    }
    sf::write_file(profile + L"\\fake_game.txt", out);
    LocalFree(argv);
    return code;
}
