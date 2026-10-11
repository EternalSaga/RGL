// SHA1 实现（FIPS 180-1）。逐字对照标准伪码，无 SIMD——本工具的 SHA1 只跑一次
// 全文件，瓶颈在 IO 不在哈希。
#include "sha1.h"

#include <cstring>

namespace RGL::meshpack {

namespace {

inline uint32_t rol(uint32_t v, int b)
{
    return (v << b) | (v >> (32 - b));
}

void compress(uint32_t state[5], const uint8_t block[64])
{
    uint32_t w[80];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) | (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
    for (int i = 16; i < 80; ++i)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        uint32_t tmp = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = tmp;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

}  // namespace

void Sha1::init()
{
    state[0] = 0x67452301; state[1] = 0xEFCDAB89; state[2] = 0x98BADCFE;
    state[3] = 0x10325476; state[4] = 0xC3D2E1F0;
    bitLen = 0; bufLen = 0;
}

void Sha1::update(const void* data, size_t len)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    bitLen += uint64_t(len) * 8;
    if (bufLen) {
        size_t need = 64 - bufLen;
        size_t take = len < need ? len : need;
        std::memcpy(buf + bufLen, p, take);
        bufLen += take; p += take; len -= take;
        if (bufLen == 64) { compress(state, buf); bufLen = 0; }
    }
    while (len >= 64) { compress(state, p); p += 64; len -= 64; }
    if (len) { std::memcpy(buf, p, len); bufLen = len; }
}

void Sha1::finish(uint8_t out[20])
{
    // 填充：0x80 + 0 + 64bit 大端总位长（可能跨两块，update 逐块走标准路径）
    const uint64_t bits = bitLen;
    uint8_t pad = 0x80;
    update(&pad, 1);
    pad = 0;
    while (bufLen != 56) update(&pad, 1);
    uint8_t lenBe[8];
    for (int i = 0; i < 8; ++i) lenBe[i] = uint8_t(bits >> (56 - i * 8));
    update(lenBe, 8);
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 4; ++j)
            out[i * 4 + j] = uint8_t(state[i] >> (24 - j * 8));
}

void sha1Buffer(const void* data, size_t len, uint8_t out[20])
{
    Sha1 s{};
    s.init();
    s.update(data, len);
    s.finish(out);
}

std::string sha1ToHex(const uint8_t digest[20])
{
    static const char* kHex = "0123456789abcdef";
    std::string out(40, '\0');
    for (int i = 0; i < 20; ++i) {
        out[size_t(i) * 2] = kHex[digest[i] >> 4];
        out[size_t(i) * 2 + 1] = kHex[digest[i] & 0xF];
    }
    return out;
}

}  // namespace RGL::meshpack
