// sc-inject.exe entry point.
//
// A windowed program, not a console one: started from the launcher, a console program gets its
// own terminal window (Proton opens one, and the game's taskbar entry groups under it). Everything
// it says goes to the profile's inject.log; a parent that passes its own stdout still gets it.

#include <windows.h>

#include <shellapi.h>

#include "inject/inject.hpp"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    for (int i = 1; argv && i < argc; ++i) args.emplace_back(argv[i]);
    LocalFree(argv);
    return sf::inject::run(args);
}
