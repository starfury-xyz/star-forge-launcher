#pragma once

// Portable path and text helpers for the launcher (Linux and Windows).
//
// The launcher keeps all text as UTF-8 std::string, like Rust's String. Paths are
// std::filesystem::path, converted only through upath()/ustr(): on Windows, constructing a path
// from a std::string reads it in the ANSI code page, which garbles anything non-ASCII (a user
// folder like C:\Users\Jörg); u8path/u8string always mean UTF-8.

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

namespace sf::launch {

namespace fs = std::filesystem;

inline fs::path upath(const std::string& utf8) { return fs::u8path(utf8); }
inline std::string ustr(const fs::path& p) { return p.u8string(); }

// An environment variable as UTF-8; nullopt when unset.
std::optional<std::string> get_env(const char* name);
// Sets (or, with nullopt, removes) an environment variable of this process.
void set_env(const char* name, const std::optional<std::string>& value);

// A whole file as bytes; nullopt if it can't be read (`ec`, if given, then says why). On Windows
// `ec` is the Windows error (system_category), and a file another program has open is tried
// again for about a second first.
std::optional<std::string> read_text(const fs::path& path, std::error_code* ec = nullptr);

// "<why> (os error N)": a Rust io::Error printed on its own. On Windows a Windows error is
// worded in English, with what to try for the errors that keep a file from opening.
std::string io_msg(const std::error_code& ec);
// "<path>: <why> (os error N)", like Rust's "{path}: {e}" messages.
std::string io_error(const fs::path& path, const std::error_code& ec);
// read_text that throws std::runtime_error(io_error(...)).
std::string read_or_throw(const fs::path& path);
// Creates or replaces a file. Throws std::runtime_error(io_error(...)).
void write_or_throw(const fs::path& path, const std::string& data);
// fs::rename, but on Windows tried again for about a second while another program (a virus
// scanner, an indexer) holds the file being replaced.
void rename_replacing(const fs::path& from, const fs::path& to, std::error_code& ec);
// Whether `ec` means the file (or a folder on its path) doesn't exist.
bool not_found(const std::error_code& ec);

// Same as sc-launch's plain_part: true if `part` is one path component that means the same file
// on Linux and Windows: not empty, "." or "..", no characters Windows forbids (<>:"|?* and
// control characters), no trailing dot or space (Windows drops them, so "a.cfg." would write
// "a.cfg"), and no device name such as "con" or "nul.lua".
bool plain_part(const std::string& part);

}  // namespace sf::launch
