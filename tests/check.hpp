#pragma once

// The portable part of the test helpers (Linux and Windows): a test prints PASS/FAIL per check
// and exits non-zero if any failed, which is all ctest needs. Windows-only helpers are in
// test_util.hpp.

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace test {

inline int failures = 0;

// Call first in main(). On Windows a crash (e.g. a hook jumping into an unloaded DLL) then prints
// CRASH and exits with code 3 at once, instead of wine opening its debugger and the test hanging.
// Elsewhere a crash already just ends the process.
inline void exit_on_crash() {
#ifdef _WIN32
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* e) -> LONG {
        std::printf("CRASH: exception 0x%08lx at %p (%d checks failed before it)\n", e->ExceptionRecord->ExceptionCode,
                    e->ExceptionRecord->ExceptionAddress, failures);
        std::fflush(stdout);
        ExitProcess(3);
    });
#endif
}

inline void check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}
inline void check(bool ok, const std::string& what) { check(ok, what.c_str()); }

inline bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

inline std::string read_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

inline void write_file(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << text;
}

// Fresh empty folder: whatever a previous run left there is deleted.
inline std::filesystem::path fresh_dir(const std::filesystem::path& path) {
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

}  // namespace test
