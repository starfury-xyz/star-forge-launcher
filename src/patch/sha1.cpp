#include "patch/sha1.hpp"

#include <cstring>

#include "patch/patchset.hpp"

namespace sf::patch {

static inline uint32_t rol(uint32_t v, int n) { return v << n | v >> (32 - n); }

Sha1::Sha1() : h_{0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0} {}

void Sha1::block(const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 | uint32_t(p[i * 4 + 2]) << 8 | p[i * 4 + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = t;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
}

void Sha1::update(const void* data, size_t len) {
    auto p = static_cast<const uint8_t*>(data);
    total_ += len;
    while (len) {
        size_t n = 64 - buf_len_;
        if (n > len) n = len;
        std::memcpy(buf_ + buf_len_, p, n);
        buf_len_ += n;
        p += n;
        len -= n;
        if (buf_len_ == 64) {
            block(buf_);
            buf_len_ = 0;
        }
    }
}

std::string Sha1::hex() {
    uint64_t bits = total_ * 8;
    uint8_t pad = 0x80, zero = 0;
    update(&pad, 1);
    while (buf_len_ != 56) update(&zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = uint8_t(bits >> (56 - 8 * i));
    update(len, 8);
    uint8_t out[20];
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++) out[i * 4 + j] = uint8_t(h_[i] >> (24 - 8 * j));
    return to_hex(Bytes(out, out + 20));
}

std::string sha1_hex(const void* data, size_t len) {
    Sha1 s;
    s.update(data, len);
    return s.hex();
}

}  // namespace sf::patch
