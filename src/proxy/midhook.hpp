#pragma once

// Installing midhooks (internal; hook authors use sfp_hook.hpp). Port of star-forge/patcher's.

#include <cstddef>
#include <cstdint>
#include <string>

#include "proxy/sfp_hook.hpp"

namespace sfp {

// Replaces `n` (>= 5) bytes at `site` with a jmp to a thunk calling `fn`; `stolen` are the
// original bytes, run from the thunk when fn returns 0. Returns "" or the error.
std::string install_midhook(uint32_t site, const uint8_t* stolen, size_t n, MidhookFn fn);

}  // namespace sfp
