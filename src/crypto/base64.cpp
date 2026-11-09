#include "crypto/base64.hpp"

namespace sf::crypto {
namespace {

const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

}  // namespace

std::string base64_encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = uint32_t(data[i]) << 16;
        if (i + 1 < len) n |= uint32_t(data[i + 1]) << 8;
        if (i + 2 < len) n |= data[i + 2];
        out += kAlphabet[n >> 18];
        out += kAlphabet[(n >> 12) & 63];
        out += i + 1 < len ? kAlphabet[(n >> 6) & 63] : '=';
        out += i + 2 < len ? kAlphabet[n & 63] : '=';
    }
    return out;
}

std::optional<std::vector<uint8_t>> base64_decode(const std::string& text) {
    if (text.size() % 4 != 0) return std::nullopt;
    std::vector<uint8_t> out;
    out.reserve(text.size() / 4 * 3);
    for (size_t i = 0; i < text.size(); i += 4) {
        const bool last = i + 4 == text.size();
        // Padding only at the very end: "xx==" or "xxx=".
        const int pad = last ? (text[i + 3] == '=') + (text[i + 2] == '=' && text[i + 3] == '=') : 0;
        int v[4];
        for (int k = 0; k < 4 - pad; ++k)
            if ((v[k] = value(text[i + k])) < 0) return std::nullopt;
        for (int k = 4 - pad; k < 4; ++k) v[k] = 0;
        uint32_t n = uint32_t(v[0]) << 18 | uint32_t(v[1]) << 12 | uint32_t(v[2]) << 6 | uint32_t(v[3]);
        // The bits after the last whole byte must be zero (else two spellings decode the same).
        if ((pad == 1 && (n & 0xff)) || (pad == 2 && (n & 0xffff))) return std::nullopt;
        out.push_back(static_cast<uint8_t>(n >> 16));
        if (pad < 2) out.push_back(static_cast<uint8_t>(n >> 8));
        if (pad < 1) out.push_back(static_cast<uint8_t>(n));
    }
    return out;
}

}  // namespace sf::crypto
