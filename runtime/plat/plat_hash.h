/* MD5, SHA-1 and SHA-256 in plain C for plat_posix.c's CryptoAPI subset (no OpenSSL: one library less to install on Linux,
 * and Android has none). Streaming: ph_init, ph_update, ph_final (on a copy when the hash is read and kept going). */
#ifndef PLAT_HASH_H
#define PLAT_HASH_H
#include <stdint.h>
#include <string.h>

enum { PH_MD5, PH_SHA1, PH_SHA256 };
typedef struct { int alg; uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; } PlatHash;

static uint32_t ph_rol(uint32_t x, int r) { return x << r | x >> (32 - r); }
static uint32_t ph_ror(uint32_t x, int r) { return x >> r | x << (32 - r); }
static uint32_t ph_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static void ph_md5_block(uint32_t *h, const uint8_t *p) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const uint8_t S[16] = {7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21};
    uint32_t w[16], a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] | (uint32_t)p[4 * i + 1] << 8 | (uint32_t)p[4 * i + 2] << 16 | (uint32_t)p[4 * i + 3] << 24;
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
        else { f = c ^ (b | ~d); g = (7 * i) & 15; }
        uint32_t t = d;
        d = c; c = b;
        b = b + ph_rol(a + f + K[i] + w[g], S[(i >> 4) * 4 + (i & 3)]);
        a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
}
static void ph_sha1_block(uint32_t *h, const uint8_t *p) {
    uint32_t w[80], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 16; i++) w[i] = ph_be32(p + 4 * i);
    for (int i = 16; i < 80; i++) w[i] = ph_rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
        else { f = b ^ c ^ d; k = 0xca62c1d6; }
        uint32_t t = ph_rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ph_rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}
static void ph_sha256_block(uint32_t *h, const uint8_t *p) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64], s[8];
    for (int i = 0; i < 16; i++) w[i] = ph_be32(p + 4 * i);
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ph_ror(w[i - 15], 7) ^ ph_ror(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ph_ror(w[i - 2], 17) ^ ph_ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(s, h, sizeof s);
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ph_ror(s[4], 6) ^ ph_ror(s[4], 11) ^ ph_ror(s[4], 25), ch = (s[4] & s[5]) ^ (~s[4] & s[6]);
        uint32_t t1 = s[7] + S1 + ch + K[i] + w[i];
        uint32_t S0 = ph_ror(s[0], 2) ^ ph_ror(s[0], 13) ^ ph_ror(s[0], 22), maj = (s[0] & s[1]) ^ (s[0] & s[2]) ^ (s[1] & s[2]);
        memmove(s + 1, s, 7 * sizeof s[0]);
        s[4] += t1;
        s[0] = t1 + S0 + maj;
    }
    for (int i = 0; i < 8; i++) h[i] += s[i];
}
static size_t ph_size(int alg) { return alg == PH_MD5 ? 16 : alg == PH_SHA1 ? 20 : 32; }
static void ph_init(PlatHash *c, int alg) {
    static const uint32_t md5[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    static const uint32_t sha1[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    static const uint32_t sha256[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memset(c, 0, sizeof *c);
    c->alg = alg;
    memcpy(c->h, alg == PH_MD5 ? md5 : alg == PH_SHA1 ? sha1 : sha256, alg == PH_MD5 ? sizeof md5 : alg == PH_SHA1 ? sizeof sha1 : sizeof sha256);
}
static void ph_block(PlatHash *c, const uint8_t *p) {
    if (c->alg == PH_MD5) ph_md5_block(c->h, p); else if (c->alg == PH_SHA1) ph_sha1_block(c->h, p); else ph_sha256_block(c->h, p);
}
static void ph_update(PlatHash *c, const void *data, size_t n) {
    const uint8_t *p = data;
    c->len += n;
    if (c->n) {
        size_t k = 64 - c->n < n ? 64 - c->n : n;
        memcpy(c->buf + c->n, p, k); c->n += k; p += k; n -= k;
        if (c->n < 64) return;
        ph_block(c, c->buf); c->n = 0;
    }
    for (; n >= 64; p += 64, n -= 64) ph_block(c, p);
    memcpy(c->buf, p, n); c->n = n;
}
static void ph_final(PlatHash *c, uint8_t *out) {      /* c is used up */
    uint64_t bits = c->len * 8;
    uint8_t pad[72] = {0x80};
    size_t k = (c->n < 56 ? 56 : 120) - c->n;
    for (int i = 0; i < 8; i++) pad[k + i] = (uint8_t)(c->alg == PH_MD5 ? bits >> (8 * i) : bits >> (56 - 8 * i));
    ph_update(c, pad, k + 8);
    for (size_t i = 0; i < ph_size(c->alg) / 4; i++)
        for (int j = 0; j < 4; j++) out[4 * i + j] = (uint8_t)(c->alg == PH_MD5 ? c->h[i] >> (8 * j) : c->h[i] >> (24 - 8 * j));
}
#endif
