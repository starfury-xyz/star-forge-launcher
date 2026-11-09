#pragma once

// Bytes from the OS's secure random source (getrandom on Linux, BCryptGenRandom on Windows).
// Portable.

#include <cstddef>
#include <cstdint>

namespace sf::crypto {

// Throws std::runtime_error("no random source: ...") if the OS can't give any.
void random_bytes(uint8_t* out, size_t len);

}  // namespace sf::crypto
