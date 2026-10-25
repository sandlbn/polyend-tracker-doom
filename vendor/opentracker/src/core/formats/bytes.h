// Little-endian byte helpers. All on-disk integers are LE (spec 02 preamble).
#pragma once

#include <cstdint>
#include <cstring>

namespace ot {

inline uint16_t rdU16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline int16_t rdI16(const uint8_t* p) { return int16_t(rdU16(p)); }
inline uint32_t rdU32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline float rdF32(const uint8_t* p) {
    uint32_t u = rdU32(p);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
inline void wrU16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}
inline void wrU32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16);
    p[3] = uint8_t(v >> 24);
}
inline void wrF32(uint8_t* p, float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    wrU32(p, u);
}

// Copy a C string into a fixed field of `size` bytes, zero-padded; at most
// size-1 characters so the field is always NUL-terminated.
inline void wrStr(uint8_t* p, uint32_t size, const char* s) {
    std::memset(p, 0, size);
    if (!s || size == 0) return;
    uint32_t n = 0;
    while (n + 1 < size && s[n]) ++n;
    std::memcpy(p, s, n);
}
// Read a fixed field of `size` bytes into dst[cap] (NUL-terminated).
inline void rdStr(const uint8_t* p, uint32_t size, char* dst, uint32_t cap) {
    if (cap == 0) return;
    uint32_t n = 0;
    while (n < size && n + 1 < cap && p[n]) {
        dst[n] = char(p[n]);
        ++n;
    }
    dst[n] = 0;
}
// Bounded string copy, always NUL-terminated.
inline void copyStr(char* dst, uint32_t cap, const char* s) {
    if (cap == 0) return;
    uint32_t n = 0;
    while (s && s[n] && n + 1 < cap) {
        dst[n] = s[n];
        ++n;
    }
    dst[n] = 0;
}

}  // namespace ot
