#pragma once

#include <optional>
#include <string>

namespace sf {

// Windows paths and APIs are UTF-16 (std::wstring); files, logs and JSON are UTF-8 (std::string).
std::string utf8(const std::wstring& w);
std::wstring wide(const std::string& s);

// UTF-16 to the system's ANSI code page (what the game's ...A functions use, e.g. Windows-1252),
// or nullopt if some character has no ANSI equivalent (it would come out as '?').
std::optional<std::string> ansi(const std::wstring& w);

// Readable text for a Win32 error code (GetLastError()), e.g. "The system cannot find the file specified."
std::string win_error(unsigned long code);

}  // namespace sf
