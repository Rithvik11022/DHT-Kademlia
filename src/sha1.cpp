// Public-domain SHA-1 implementation
// Adapted for easy integration: returns raw bytes in std::string and hex string.
#include "sha1.h"
#include <cstdint>
#include <cstring>
#include <array>
#include <sstream>
#include <iomanip>

namespace {

// rotate left
inline uint32_t rol(uint32_t value, unsigned int bits) {
    return (value << bits) | (value >> (32 - bits));
}

void sha1_transform(uint32_t state[5], const unsigned char buffer[64]) {
    uint32_t a, b, c, d, e, t, w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = (uint32_t)buffer[i * 4] << 24;
        w[i] |= (uint32_t)buffer[i * 4 + 1] << 16;
        w[i] |= (uint32_t)buffer[i * 4 + 2] << 8;
        w[i] |= (uint32_t)buffer[i * 4 + 3];
    }
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];

    for (int i = 0; i < 80; ++i) {
        if (i < 20)
            t = ((b & c) | ((~b) & d)) + 0x5A827999;
        else if (i < 40)
            t = (b ^ c ^ d) + 0x6ED9EBA1;
        else if (i < 60)
            t = ((b & c) | (b & d) | (c & d)) + 0x8F1BBCDC;
        else
            t = (b ^ c ^ d) + 0xCA62C1D6;

        t += rol(a,5) + e + w[i];
        e = d;
        d = c;
        c = rol(b,30);
        b = a;
        a = t;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

std::string to_hex_lower(const unsigned char *buf, size_t len) {
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i) {
        ss << std::setw(2) << (int)buf[i];
    }
    return ss.str();
}

} // anonymous

namespace sha1 {

std::string hash_raw(const std::string &input) {
    // initialize
    uint32_t state[5] = {
        0x67452301u,
        0xEFCDAB89u,
        0x98BADCFEu,
        0x10325476u,
        0xC3D2E1F0u
    };

    uint64_t bitlen = (uint64_t)input.size() * 8;
    // process in 512-bit (64-byte) chunks
    size_t rem = input.size();
    const unsigned char *data = reinterpret_cast<const unsigned char*>(input.data());

    // process full 64-byte blocks
    while (rem >= 64) {
        sha1_transform(state, data);
        data += 64;
        rem -= 64;
    }

    // buffer remainder and padding
    unsigned char block[64];
    std::memset(block, 0, 64);
    if (rem > 0) std::memcpy(block, data, rem);
    block[rem] = 0x80; // append '1' bit

    if (rem >= 56) {
        // not enough room for length, process this block and prepare another
        sha1_transform(state, block);
        std::memset(block, 0, 64);
    }

    // append length in bits in big-endian
    block[56] = (unsigned char)((bitlen >> 56) & 0xFF);
    block[57] = (unsigned char)((bitlen >> 48) & 0xFF);
    block[58] = (unsigned char)((bitlen >> 40) & 0xFF);
    block[59] = (unsigned char)((bitlen >> 32) & 0xFF);
    block[60] = (unsigned char)((bitlen >> 24) & 0xFF);
    block[61] = (unsigned char)((bitlen >> 16) & 0xFF);
    block[62] = (unsigned char)((bitlen >> 8) & 0xFF);
    block[63] = (unsigned char)(bitlen & 0xFF);

    sha1_transform(state, block);

    // produce 20-byte digest big-endian
    unsigned char digest[20];
    for (int i = 0; i < 5; ++i) {
        digest[i*4 + 0] = (unsigned char)((state[i] >> 24) & 0xFF);
        digest[i*4 + 1] = (unsigned char)((state[i] >> 16) & 0xFF);
        digest[i*4 + 2] = (unsigned char)((state[i] >> 8) & 0xFF);
        digest[i*4 + 3] = (unsigned char)(state[i] & 0xFF);
    }
    // return as std::string (binary)
    return std::string(reinterpret_cast<char*>(digest), 20);
}

std::string hash_hex(const std::string &input) {
    std::string raw = hash_raw(input);
    // convert raw bytes to lowercase hex
    return to_hex_lower(reinterpret_cast<const unsigned char*>(raw.data()), raw.size());
}

} // namespace sha1
