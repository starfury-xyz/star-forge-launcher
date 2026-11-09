#pragma once

#include <string>

namespace sf {

// Windows paths and APIs are UTF-16 (std::wstring); files, logs and JSON are UTF-8 (std::string).
std::string utf8(const std::wstring& w);
std::wstring wide(const std::string& s);

// Readable text for a Win32 error code (GetLastError()), e.g. "The system cannot find the file specified."
std::string win_error(unsigned long code);

}  // namespace sf
