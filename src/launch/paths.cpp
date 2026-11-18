#include "launch/paths.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#endif

namespace sf::launch {

std::optional<std::string> get_env(const char* name) {
#ifdef _WIN32
    // The wide API, so the value isn't squeezed through the ANSI code page.
    const std::wstring wname = fs::path(name).wstring();
    DWORD n = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (n == 0) return std::nullopt;  // unset (an empty value also reads as 0: treated alike)
    std::wstring value(n, L'\0');
    value.resize(GetEnvironmentVariableW(wname.c_str(), value.data(), n));
    return ustr(fs::path(value));
#else
    const char* v = std::getenv(name);
    if (!v) return std::nullopt;
    return std::string(v);
#endif
}

void set_env(const char* name, const std::optional<std::string>& value) {
#ifdef _WIN32
    const std::wstring wname = fs::path(name).wstring();
    SetEnvironmentVariableW(wname.c_str(), value ? upath(*value).wstring().c_str() : nullptr);
#else
    if (value) setenv(name, value->c_str(), 1);
    else unsetenv(name);
#endif
}

#ifdef _WIN32
namespace {

std::error_code win_error(DWORD e) { return std::error_code(static_cast<int>(e), std::system_category()); }

// Another program (a virus scanner, a backup or sync tool, an editor) has the file open or
// locked: usually gone within a moment, so worth a short wait.
bool held_by_another_program(DWORD e) { return e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION; }

constexpr int kRetries = 10;      // tries after the first
constexpr DWORD kRetryMs = 100;  // so about a second in all

HANDLE open_retrying(const fs::path& path, DWORD access, DWORD share, DWORD disposition, DWORD& err) {
    for (int attempt = 0;; ++attempt) {
        HANDLE h = CreateFileW(path.c_str(), access, share, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) return h;
        err = GetLastError();
        if (!held_by_another_program(err) || attempt == kRetries) return INVALID_HANDLE_VALUE;
        Sleep(kRetryMs);
    }
}

// Windows' own text for an error, in English (else the system language), as UTF-8.
std::string win_message(DWORD e) {
    wchar_t* buf = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD n = FormatMessageW(flags, nullptr, e, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    if (n == 0) n = FormatMessageW(flags, nullptr, e, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    if (n == 0) return "Windows error " + std::to_string(e);
    std::wstring w(buf, n);
    LocalFree(buf);
    while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n' || w.back() == L' ' || w.back() == L'.')) w.pop_back();
    return ustr(fs::path(w));
}

// What a player can do about the Windows errors that keep a file from opening.
const char* win_hint(DWORD e) {
    switch (e) {
        case ERROR_ACCESS_DENIED:
            return "Windows refused access: check the folder's permissions, that the file isn't read-only, and that STAR FORGE "
                   "was never run as administrator for this folder";
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
            return "another program has the file open: close editors and the game, and check any virus scanner, backup or "
                   "sync tool (Resource Monitor > CPU > Associated Handles shows which program)";
        case ERROR_DELETE_PENDING:
            return "Windows is still deleting the file because a program has it open: close that program or restart Windows";
        default:
            return nullptr;
    }
}

}  // namespace
#endif

std::optional<std::string> read_text(const fs::path& path, std::error_code* ec) {
#ifdef _WIN32
    // CreateFileW rather than _wfopen: _wfopen folds most Windows errors into errno 13, which
    // can't tell "access denied" from "another program has it open".
    DWORD err = 0;
    HANDLE h = open_retrying(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING, err);
    if (h == INVALID_HANDLE_VALUE) {
        if (ec) *ec = win_error(err);
        return std::nullopt;
    }
    std::string out;
    char buf[64 * 1024];
    DWORD n = 0;
    for (;;) {
        if (!ReadFile(h, buf, sizeof buf, &n, nullptr)) {
            err = GetLastError();
            CloseHandle(h);
            if (ec) *ec = win_error(err);
            return std::nullopt;
        }
        if (n == 0) break;
        out.append(buf, n);
    }
    CloseHandle(h);
#else
    // fopen rather than ifstream: it leaves the reason in errno.
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (ec) *ec = std::error_code(errno, std::generic_category());
        return std::nullopt;
    }
    std::string out;
    char buf[64 * 1024];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    bool bad = std::ferror(f);
    std::fclose(f);
    if (bad) {
        if (ec) *ec = std::make_error_code(std::errc::io_error);
        return std::nullopt;
    }
#endif
    if (ec) ec->clear();
    return out;
}

std::string io_msg(const std::error_code& ec) {
#ifdef _WIN32
    if (ec.category() == std::system_category()) {
        const DWORD e = static_cast<DWORD>(ec.value());
        std::string out = win_message(e) + " (os error " + std::to_string(ec.value()) + ")";
        if (const char* hint = win_hint(e)) out += ": " + std::string(hint);
        return out;
    }
#endif
    return ec.message() + " (os error " + std::to_string(ec.value()) + ")";
}

std::string io_error(const fs::path& path, const std::error_code& ec) { return ustr(path) + ": " + io_msg(ec); }

std::string read_or_throw(const fs::path& path) {
    std::error_code ec;
    auto text = read_text(path, &ec);
    if (!text) throw std::runtime_error(io_error(path, ec));
    return *text;
}

void write_or_throw(const fs::path& path, const std::string& data) {
#ifdef _WIN32
    DWORD err = 0;
    HANDLE h = open_retrying(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, CREATE_ALWAYS, err);
    if (h == INVALID_HANDLE_VALUE) throw std::runtime_error(io_error(path, win_error(err)));
    size_t done = 0;
    while (done < data.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(data.size() - done, 1u << 30));
        DWORD n = 0;
        if (!WriteFile(h, data.data() + done, chunk, &n, nullptr)) {
            err = GetLastError();
            CloseHandle(h);
            throw std::runtime_error(io_error(path, win_error(err)));
        }
        done += n;
    }
    if (!CloseHandle(h)) throw std::runtime_error(io_error(path, win_error(GetLastError())));
#else
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error(io_error(path, std::error_code(errno, std::generic_category())));
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = std::fclose(f) == 0 && ok;
    if (!ok) throw std::runtime_error(io_error(path, std::make_error_code(std::errc::io_error)));
#endif
}

void rename_replacing(const fs::path& from, const fs::path& to, std::error_code& ec) {
    for (int attempt = 0;; ++attempt) {
        fs::rename(from, to, ec);
#ifdef _WIN32
        // Replacing a file a scanner or indexer has open fails with access denied or a
        // sharing violation until it lets go.
        const bool busy = ec && ec.category() == std::system_category() &&
                          (ec.value() == ERROR_ACCESS_DENIED || held_by_another_program(static_cast<DWORD>(ec.value())));
        if (busy && attempt < kRetries) {
            Sleep(kRetryMs);
            continue;
        }
#else
        (void)attempt;
#endif
        return;
    }
}

bool not_found(const std::error_code& ec) {
#ifdef _WIN32
    if (ec.category() == std::system_category() && (ec.value() == ERROR_FILE_NOT_FOUND || ec.value() == ERROR_PATH_NOT_FOUND))
        return true;
#endif
    return ec == std::errc::no_such_file_or_directory;
}

bool plain_part(const std::string& part) {
    if (part.empty() || part == "." || part == "..") return false;
    if (part.back() == '.' || part.back() == ' ') return false;
    for (size_t i = 0; i < part.size(); ++i) {
        unsigned char c = part[i];
        if (c < 0x20 || c == 0x7f) return false;
        // U+0080..U+009F (C1 controls) are UTF-8 C2 80..C2 9F.
        if (c == 0xc2 && i + 1 < part.size() && static_cast<unsigned char>(part[i + 1]) <= 0x9f) return false;
        if (std::string("<>:\"|?*").find(static_cast<char>(c)) != std::string::npos) return false;
    }
    // Device names: the part before the first dot, trailing spaces dropped, any case.
    std::string stem = part.substr(0, part.find('.'));
    while (!stem.empty() && (stem.back() == ' ' || stem.back() == '\t')) stem.pop_back();
    for (auto& c : stem)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul") return false;
    if (stem.size() == 4 && (stem.rfind("com", 0) == 0 || stem.rfind("lpt", 0) == 0) && stem[3] >= '0' && stem[3] <= '9')
        return false;
    return true;
}

}  // namespace sf::launch
