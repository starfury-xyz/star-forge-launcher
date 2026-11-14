// sc-server.exe entry point; see server.hpp. A console program: Ctrl+C reaches the servers
// through consoles.

#include <windows.h>

#include <shellapi.h>

#include <cstdio>
#include <exception>
#include <string>

#include "server/server.hpp"

int wmain(int argc, wchar_t** argv) {
    const std::wstring cmd = argc == 3 ? argv[1] : L"";
    if (cmd != L"run" && cmd != L"stop") {
        std::fputs("usage: sc-server.exe run|stop <cloud dir>\n", stderr);
        return 2;
    }
    try {
        const sf::server::Settings s = sf::server::settings(argv[2]);
        return cmd == L"run" ? sf::server::run(s) : sf::server::stop(s);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "sc-server: %s\n", e.what());
        return 1;
    }
}
