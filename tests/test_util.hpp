#pragma once

// Tiny helpers shared by the test exes. A test prints PASS/FAIL per check and exits non-zero if
// any failed, which is all ctest needs.

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace test {

inline int failures = 0;

// Call first in main(). A crash (e.g. a hook jumping into an unloaded DLL) then prints CRASH and
// exits with code 3 at once, instead of wine opening its debugger and the test hanging.
inline void exit_on_crash() {
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* e) -> LONG {
        std::printf("CRASH: exception 0x%08lx at %p (%d checks failed before it)\n", e->ExceptionRecord->ExceptionCode,
                    e->ExceptionRecord->ExceptionAddress, failures);
        std::fflush(stdout);
        ExitProcess(3);
    });
}

inline void check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

inline std::wstring dir_of(const std::wstring& p) { return p.substr(0, p.rfind(L'\\')); }

inline std::wstring exe_dir() {
    std::wstring exe(1024, L'\0');
    exe.resize(GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size())));
    return dir_of(exe);
}

inline std::string read_file(const std::wstring& path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

inline void write_file(const std::wstring& path, const std::string& text) {
    std::ofstream f(path.c_str(), std::ios::binary | std::ios::trunc);
    f << text;
}

// Fresh empty folder: whatever a previous run left there is deleted.
inline std::wstring fresh_dir(const std::wstring& path) {
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

inline bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace test
