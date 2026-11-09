#pragma once

// SHA-256 (FIPS 180-4): file lists, pack zips and signed manifests name files by it. Incremental,
// so a download can be hashed as it arrives. Portable.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace sf::crypto {

class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t len);
    std::array<uint8_t, 32> digest();  // finalizes
    std::string hex();                 // lower-case hex digest; finalizes

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    size_t buf_len_ = 0;
    uint64_t total_ = 0;
};

std::string sha256_hex(const void* data, size_t len);
// Lower-case hex of bytes.
std::string to_hex(const uint8_t* data, size_t len);

}  // namespace sf::crypto
