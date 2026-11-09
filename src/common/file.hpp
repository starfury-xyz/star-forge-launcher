#pragma once

#include <string>

namespace sf {

// Reads a whole file into `out`. Returns 0, or the Win32 error code (e.g. ERROR_FILE_NOT_FOUND),
// so callers can tell a missing file from other failures (sf::win_error turns it into text).
unsigned long read_file(const std::wstring& path, std::string& out);

}  // namespace sf
