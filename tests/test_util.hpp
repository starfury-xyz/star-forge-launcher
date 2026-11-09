#pragma once

// Tiny helpers shared by the test exes. A test prints PASS/FAIL per check and exits non-zero if
// any failed, which is all ctest needs.

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace test {

inline int failures = 0;

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

// Fresh empty folder (removes the files a previous run left in it; tests don't nest folders).
inline std::wstring fresh_dir(const std::wstring& path) {
    CreateDirectoryW(path.c_str(), nullptr);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((path + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((path + L"\\" + fd.cFileName).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return path;
}

inline bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace test
