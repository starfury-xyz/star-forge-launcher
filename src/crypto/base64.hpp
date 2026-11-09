#pragma once

// Standard base64 (RFC 4648, with padding), as keys and signatures are written. Decoding is
// strict like the Rust base64 crate's STANDARD engine: padding is required, and no whitespace or
// stray bits are allowed, so each value has exactly one spelling. Portable.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sf::crypto {

std::string base64_encode(const uint8_t* data, size_t len);
// nullopt if `text` isn't canonical base64.
std::optional<std::vector<uint8_t>> base64_decode(const std::string& text);

}  // namespace sf::crypto
