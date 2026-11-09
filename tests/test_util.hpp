#pragma once

// Windows-only test helpers (paths as std::wstring, as the agent uses them), on top of the
// portable ones in check.hpp.

#include <windows.h>

#include "check.hpp"

namespace test {

inline std::wstring dir_of(const std::wstring& p) { return p.substr(0, p.rfind(L'\\')); }

inline std::wstring exe_dir() {
    std::wstring exe(1024, L'\0');
    exe.resize(GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size())));
    return dir_of(exe);
}

}  // namespace test
