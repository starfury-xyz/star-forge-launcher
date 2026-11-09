#pragma once

// SHA-1, used only to recognise a game build (patchsets name the builds they fit by SHA-1).
// Same algorithm as star-forge/patcher/src/core/sha1.cpp.

#include <cstddef>
#include <cstdint>
#include <string>

namespace sf::patch {

class Sha1 {
public:
    Sha1();
    void update(const void* data, size_t len);
    std::string hex();  // lower-case hex digest; finalizes

private:
    void block(const uint8_t* p);
    uint32_t h_[5];
    uint8_t buf_[64];
    size_t buf_len_ = 0;
    uint64_t total_ = 0;
};

// Lower-case hex SHA-1 of a buffer, as patchsets record it.
std::string sha1_hex(const void* data, size_t len);

}  // namespace sf::patch
