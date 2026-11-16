/*
 * File Manager — CRC32, MD5 and SHA-1 (tools overlay). Plain C, no firmware calls: the same file
 * builds on a PC (tools/test_tools.sh) to check it against known digests.
 */
#include <stdint.h>
#include <string.h>
#include "hash.h"

/* ================================================================ CRC32 (zip, No-Intro) */

static uint32_t crc_table[256];

void crc32_init(uint32_t *crc)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    *crc = 0xFFFFFFFFu;
}

void crc32_update(uint32_t *crc, const uint8_t *p, size_t n)
{
    uint32_t c = *crc;
    while (n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    *crc = c;
}

uint32_t crc32_final(const uint32_t *crc) { return *crc ^ 0xFFFFFFFFu; }

/* ================================================================ MD5 (RFC 1321) */

static const uint32_t md5_k[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};
static const uint8_t md5_r[16] = { 7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21 };

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void md5_block(md5_t *m, const uint8_t *b)
{
    uint32_t w[16];
    for (int i = 0; i < 16; i++)
        w[i] = b[i * 4] | b[i * 4 + 1] << 8 | b[i * 4 + 2] << 16 | (uint32_t)b[i * 4 + 3] << 24;
    uint32_t a = m->h[0], bb = m->h[1], c = m->h[2], d = m->h[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16)      { f = (bb & c) | (~bb & d);  g = i; }
        else if (i < 32) { f = (d & bb) | (~d & c);   g = (5 * i + 1) & 15; }
        else if (i < 48) { f = bb ^ c ^ d;            g = (3 * i + 5) & 15; }
        else             { f = c ^ (bb | ~d);         g = (7 * i) & 15; }
        uint32_t t = d;
        d = c;
        c = bb;
        bb = bb + ROL(a + f + md5_k[i] + w[g], md5_r[(i >> 4) * 4 + (i & 3)]);
        a = t;
    }
    m->h[0] += a; m->h[1] += bb; m->h[2] += c; m->h[3] += d;
}

void md5_init(md5_t *m)
{
    m->h[0] = 0x67452301; m->h[1] = 0xefcdab89; m->h[2] = 0x98badcfe; m->h[3] = 0x10325476;
    m->len = 0;
}

void md5_update(md5_t *m, const uint8_t *p, size_t n)
{
    size_t have = (size_t)(m->len & 63);
    m->len += n;
    if (have) {
        size_t take = 64 - have < n ? 64 - have : n;
        memcpy(m->buf + have, p, take);
        p += take; n -= take; have += take;
        if (have < 64) return;
        md5_block(m, m->buf);
    }
    for (; n >= 64; p += 64, n -= 64) md5_block(m, p);
    memcpy(m->buf, p, n);
}

void md5_final(md5_t *m, uint8_t out[16])
{
    uint64_t bits = m->len * 8;
    uint8_t pad[72] = { 0x80 };
    size_t have = (size_t)(m->len & 63), padn = (have < 56 ? 56 : 120) - have;
    for (int i = 0; i < 8; i++) pad[padn + i] = (uint8_t)(bits >> (8 * i));
    md5_update(m, pad, padn + 8);
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 4; k++) out[i * 4 + k] = (uint8_t)(m->h[i] >> (8 * k));
}

/* ================================================================ SHA-1 (FIPS 180) */

static void sha1_block(sha1_t *s, const uint8_t *b)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)b[i * 4] << 24 | b[i * 4 + 1] << 16 | b[i * 4 + 2] << 8 | b[i * 4 + 3];
    for (int i = 16; i < 80; i++) w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = s->h[0], bb = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (bb & c) | (~bb & d);           k = 0x5A827999; }
        else if (i < 40) { f = bb ^ c ^ d;                     k = 0x6ED9EBA1; }
        else if (i < 60) { f = (bb & c) | (bb & d) | (c & d);  k = 0x8F1BBCDC; }
        else             { f = bb ^ c ^ d;                     k = 0xCA62C1D6; }
        uint32_t t = ROL(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROL(bb, 30); bb = a; a = t;
    }
    s->h[0] += a; s->h[1] += bb; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

void sha1_init(sha1_t *s)
{
    s->h[0] = 0x67452301; s->h[1] = 0xEFCDAB89; s->h[2] = 0x98BADCFE; s->h[3] = 0x10325476;
    s->h[4] = 0xC3D2E1F0;
    s->len = 0;
}

void sha1_update(sha1_t *s, const uint8_t *p, size_t n)
{
    size_t have = (size_t)(s->len & 63);
    s->len += n;
    if (have) {
        size_t take = 64 - have < n ? 64 - have : n;
        memcpy(s->buf + have, p, take);
        p += take; n -= take; have += take;
        if (have < 64) return;
        sha1_block(s, s->buf);
    }
    for (; n >= 64; p += 64, n -= 64) sha1_block(s, p);
    memcpy(s->buf, p, n);
}

void sha1_final(sha1_t *s, uint8_t out[20])
{
    uint64_t bits = s->len * 8;
    uint8_t pad[72] = { 0x80 };
    size_t have = (size_t)(s->len & 63), padn = (have < 56 ? 56 : 120) - have;
    for (int i = 0; i < 8; i++) pad[padn + i] = (uint8_t)(bits >> (8 * (7 - i)));
    sha1_update(s, pad, padn + 8);
    for (int i = 0; i < 5; i++)
        for (int k = 0; k < 4; k++) out[i * 4 + k] = (uint8_t)(s->h[i] >> (8 * (3 - k)));
}

void hex_str(char *out, const uint8_t *p, size_t n)
{
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[i * 2] = hx[p[i] >> 4]; out[i * 2 + 1] = hx[p[i] & 15]; }
    out[n * 2] = '\0';
}
