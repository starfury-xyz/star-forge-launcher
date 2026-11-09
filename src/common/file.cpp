#include "common/file.hpp"

#include <windows.h>

namespace sf {

// The Win32 API rather than std::ifstream, so the error code survives.
unsigned long read_file(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    out.clear();
    char buf[64 * 1024];
    DWORD n = 0;
    DWORD err = 0;
    while (true) {
        if (!ReadFile(h, buf, sizeof buf, &n, nullptr)) {
            err = GetLastError();
            break;
        }
        if (n == 0) break;
        out.append(buf, n);
    }
    CloseHandle(h);
    return err;
}

}  // namespace sf
