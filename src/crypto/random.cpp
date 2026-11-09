#include "crypto/random.hpp"

#include <cerrno>
#include <stdexcept>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <windows.h>

#include <bcrypt.h>
#else
#include <sys/random.h>
#endif

namespace sf::crypto {

void random_bytes(uint8_t* out, size_t len) {
#ifdef _WIN32
    NTSTATUS st = BCryptGenRandom(nullptr, out, static_cast<ULONG>(len), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (st != 0) throw std::runtime_error("no random source: BCryptGenRandom failed (" + std::to_string(st) + ")");
#else
    while (len > 0) {
        ssize_t n = getrandom(out, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("no random source: " + std::generic_category().message(errno));
        }
        out += n;
        len -= static_cast<size_t>(n);
    }
#endif
}

}  // namespace sf::crypto
