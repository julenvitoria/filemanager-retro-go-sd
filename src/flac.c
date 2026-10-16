/*
 * File Manager — small FLAC decoder (own code, MIT like the rest of the project).
 *
 * Mono/stereo, 4..32 bits per sample, blocks of up to FLAC_MAX_BLOCK samples: everything a
 * normal encoder writes. Frames are found by their sync code and checked with their CRC-8 and
 * CRC-16, so decoding can start anywhere in the file (that is how seeking works).
 * The output is mono 16-bit (the console has one speaker).
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "flac.h"

/* ---------------------------------------------------------------- CRCs */

static uint8_t  crc8_t[256];
static uint16_t crc16_t[256];

static void crc_init(void)
{
    if (crc16_t[1]) return;
    for (int i = 0; i < 256; i++) {
        uint8_t c = (uint8_t)i;
        for (int b = 0; b < 8; b++) c = (uint8_t)((c & 0x80) ? (c << 1) ^ 0x07 : c << 1);
        crc8_t[i] = c;
        uint16_t d = (uint16_t)(i << 8);
        for (int b = 0; b < 8; b++) d = (uint16_t)((d & 0x8000) ? (d << 1) ^ 0x8005 : d << 1);
        crc16_t[i] = d;
    }
}
static uint8_t crc8(const uint8_t *p, uint32_t n) { uint8_t c = 0; while (n--) c = crc8_t[c ^ *p++]; return c; }
static uint16_t crc16(const uint8_t *p, uint32_t n)
{
    uint16_t c = 0;
    while (n--) c = (uint16_t)((c << 8) ^ crc16_t[(c >> 8) ^ *p++]);
    return c;
}

/* ---------------------------------------------------------------- bit reader */

static const uint8_t *br_p, *br_end;
static uint64_t br_cache;          /* next bits at the top */
static int br_bits;
static int br_over;                /* bytes read past the end (broken frame) */

static void br_start(const uint8_t *p, const uint8_t *end) { br_p = p; br_end = end; br_cache = 0; br_bits = 0; br_over = 0; }
static inline void br_fill(void)
{
    while (br_bits <= 56) {
        uint64_t b;
        if (br_p < br_end) b = *br_p++;
        else { b = 0; br_over++; }
        br_cache |= b << (56 - br_bits);
        br_bits += 8;
    }
}
static inline uint32_t br_get(int n)                /* 0..32 bits */
{
    if (!n) return 0;
    br_fill();
    uint32_t v = (uint32_t)(br_cache >> (64 - n));
    br_cache <<= n;
    br_bits -= n;
    return v;
}
static inline int32_t br_sget(int n)
{
    if (!n) return 0;
    uint32_t v = br_get(n);
    if (n < 32 && (v & (1u << (n - 1)))) v |= ~0u << n;
    return (int32_t)v;
}
static inline uint32_t br_unary(void)
{
    uint32_t n = 0;
    for (;;) {
        br_fill();
        if (br_cache) {
            int z = __builtin_clzll(br_cache);
            if (z < br_bits) {
                n += (uint32_t)z;
                br_cache = z == 63 ? 0 : br_cache << (z + 1);
                br_bits -= z + 1;
                return n;
            }
        }
        n += (uint32_t)br_bits;
        br_cache = 0;
        br_bits = 0;
        if (br_over > 8) return n;
    }
}
static const uint8_t *br_pos(void) { return br_p + br_over - br_bits / 8; }    /* next whole byte */
static void br_align(void) { int r = br_bits & 7; br_cache <<= r; br_bits -= r; }

/* ---------------------------------------------------------------- stream info */

static uint32_t be(const uint8_t *p, int n) { uint32_t v = 0; while (n--) v = v << 8 | *p++; return v; }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void comment(char *out, size_t size, const char *v, uint32_t n)
{
    if (out[0] || !size) return;
    if (n >= size) n = (uint32_t)size - 1;
    memcpy(out, v, n);
    out[n] = 0;
}

int flac_open(FILE *f, uint32_t file_size, flac_info_t *in, uint8_t *tmp, uint32_t tmp_size)
{
    crc_init();
    memset(in, 0, sizeof *in);
    uint8_t h[10];
    uint32_t pos = 0;
    if (fseek(f, 0, SEEK_SET) || fread(h, 1, 10, f) != 10) return -1;
    if (!memcmp(h, "ID3", 3))                                          /* ID3v2 in front (rare) */
        pos = 10 + ((h[6] & 0x7F) << 21 | (h[7] & 0x7F) << 14 | (h[8] & 0x7F) << 7 | (h[9] & 0x7F));
    if (fseek(f, (long)pos, SEEK_SET) || fread(h, 1, 4, f) != 4 || memcmp(h, "fLaC", 4)) return -1;
    pos += 4;
    bool have_info = false;
    for (;;) {
        if (fseek(f, (long)pos, SEEK_SET) || fread(h, 1, 4, f) != 4) return -1;
        int type = h[0] & 0x7F;
        uint32_t len = be(h + 1, 3);
        bool last = h[0] & 0x80;
        if (type == 0 && len >= 34) {                                   /* STREAMINFO */
            uint8_t s[34];
            if (fread(s, 1, 34, f) != 34) return -1;
            in->max_block = be(s + 2, 2);
            in->max_frame = be(s + 7, 3);
            in->rate = be(s + 10, 3) >> 4;
            in->channels = ((s[12] >> 1) & 7) + 1;
            in->bps = ((s[12] & 1) << 4 | s[13] >> 4) + 1;
            in->total = (uint64_t)(s[13] & 0x0F) << 32 | be(s + 14, 4);
            have_info = true;
        } else if (type == 4 && len >= 8) {                             /* VORBIS_COMMENT */
            uint32_t n = len < tmp_size ? len : tmp_size;
            if (fread(tmp, 1, n, f) == n) {
                uint32_t i = 4 + le32(tmp), count;
                if (i + 4 <= n) {
                    count = le32(tmp + i); i += 4;
                    while (count-- && i + 4 <= n) {
                        uint32_t l = le32(tmp + i); i += 4;
                        if (l > n - i) break;
                        const char *c = (const char *)tmp + i;
                        if (l > 6 && !strncasecmp(c, "TITLE=", 6)) comment(in->title, sizeof in->title, c + 6, l - 6);
                        if (l > 7 && !strncasecmp(c, "ARTIST=", 7)) comment(in->artist, sizeof in->artist, c + 7, l - 7);
                        i += l;
                    }
                }
            }
        }
        pos += 4 + len;
        if (last) break;
        if (pos >= file_size) return -1;
    }
    if (!have_info || !in->rate) return -1;
    in->first_frame = pos;
    if (pos >= file_size) return -3;
    if (in->channels > 2 || in->bps < 4 || in->bps > 32 || in->max_block > FLAC_MAX_BLOCK) return -2;
    return 0;
}

/* ---------------------------------------------------------------- frame decoder */

static struct {
    FILE *f;
    uint32_t end, need;                /* end of the audio; bytes wanted in the window */
    uint8_t *buf; uint32_t size, len, at;
    uint32_t file_pos;                 /* file offset of buf[len] */
    bool eof;
    int32_t *ch[2];
    const flac_info_t *in;
} D;

void flac_start(FILE *f, const flac_info_t *in, uint32_t pos, uint32_t end,
                uint8_t *buf, uint32_t buf_size, int32_t *ch0, int32_t *ch1)
{
    D.f = f; D.in = in; D.end = end;
    D.buf = buf; D.size = buf_size; D.len = D.at = 0;
    D.file_pos = pos;
    D.eof = false;
    D.ch[0] = ch0; D.ch[1] = ch1;
    D.need = in->max_frame ? in->max_frame + 32 : buf_size - 4096;
    if (D.need > buf_size - 4096) D.need = buf_size - 4096;
    fseek(f, (long)pos, SEEK_SET);
}

static void refill(void)
{
    if (D.eof || D.len - D.at >= D.need) return;
    uint32_t keep = D.len - D.at;
    memmove(D.buf, D.buf + D.at, keep);
    D.len = keep; D.at = 0;
    uint32_t want = D.size - keep;
    if (D.file_pos + want > D.end) want = D.file_pos < D.end ? D.end - D.file_pos : 0;
    size_t got = want ? fread(D.buf + keep, 1, want, D.f) : 0;
    D.len += (uint32_t)got;
    D.file_pos += (uint32_t)got;
    if (got < want || !want) D.eof = true;
}

static const uint32_t RATES[12] = { 0, 88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100, 48000, 96000 };
static const int SIZES[8] = { 0, 8, 12, 0, 16, 20, 24, 32 };

typedef struct { uint32_t block, rate; int chan, bps; uint64_t number; bool variable; uint32_t hlen; } fhdr_t;

/* frame header at p (n bytes available); false if it is not one */
static bool header(const uint8_t *p, uint32_t n, fhdr_t *h)
{
    if (n < 16 || p[0] != 0xFF || (p[1] & 0xFE) != 0xF8) return false;
    h->variable = p[1] & 1;
    int bc = p[2] >> 4, rc = p[2] & 15, cc = p[3] >> 4, sc = (p[3] >> 1) & 7;
    if (!bc || rc == 15 || cc > 10 || sc == 3 || (p[3] & 1)) return false;
    uint32_t i = 4;
    /* "UTF-8" coded frame / sample number */
    uint64_t v = p[i];
    int extra = 0;
    if (v < 0x80) extra = 0;
    else if ((v & 0xE0) == 0xC0) { extra = 1; v &= 0x1F; }
    else if ((v & 0xF0) == 0xE0) { extra = 2; v &= 0x0F; }
    else if ((v & 0xF8) == 0xF0) { extra = 3; v &= 0x07; }
    else if ((v & 0xFC) == 0xF8) { extra = 4; v &= 0x03; }
    else if ((v & 0xFE) == 0xFC) { extra = 5; v &= 0x01; }
    else if (v == 0xFE) { extra = 6; v = 0; }
    else return false;
    i++;
    for (int k = 0; k < extra; k++, i++) {
        if ((p[i] & 0xC0) != 0x80) return false;
        v = v << 6 | (p[i] & 0x3F);
    }
    h->number = v;
    if (bc == 1) h->block = 192;
    else if (bc <= 5) h->block = 576u << (bc - 2);
    else if (bc == 6) h->block = p[i++] + 1u;
    else if (bc == 7) { h->block = (p[i] << 8 | p[i + 1]) + 1u; i += 2; }
    else h->block = 256u << (bc - 8);
    if (rc == 0) h->rate = D.in->rate;
    else if (rc < 12) h->rate = RATES[rc];
    else if (rc == 12) h->rate = p[i++] * 1000u;
    else if (rc == 13) { h->rate = (uint32_t)(p[i] << 8 | p[i + 1]); i += 2; }
    else { h->rate = (uint32_t)(p[i] << 8 | p[i + 1]) * 10u; i += 2; }
    if (crc8(p, i) != p[i]) return false;
    h->hlen = i + 1;
    h->chan = cc;
    h->bps = sc ? SIZES[sc] : D.in->bps;
    int nch = cc < 8 ? cc + 1 : 2;
    if (nch != D.in->channels || h->bps != D.in->bps || h->rate != D.in->rate) return false;
    if (h->block > FLAC_MAX_BLOCK) return false;
    return true;
}

static bool residual(int32_t *r, uint32_t block, int order)
{
    int method = (int)br_get(2);
    if (method > 1) return false;
    int pbits = method ? 5 : 4, esc = method ? 31 : 15;
    int po = (int)br_get(4);
    uint32_t parts = 1u << po;
    if ((block >> po) < (uint32_t)order || (block & (parts - 1))) return false;
    uint32_t k = (uint32_t)order;
    for (uint32_t p = 0; p < parts; p++) {
        uint32_t n = (block >> po) - (p == 0 ? (uint32_t)order : 0);
        int param = (int)br_get(pbits);
        if (param == esc) {
            int bits = (int)br_get(5);
            for (uint32_t j = 0; j < n; j++) r[k++] = br_sget(bits);
        } else {
            for (uint32_t j = 0; j < n; j++) {
                uint32_t q = br_unary();
                uint32_t u = (q << param) | br_get(param);
                r[k++] = (int32_t)(u >> 1) ^ -(int32_t)(u & 1);
            }
        }
        if (br_over > 8) return false;
    }
    return true;
}

static bool subframe(int32_t *s, uint32_t block, int bps)
{
    if (br_get(1)) return false;
    int type = (int)br_get(6);
    int wasted = 0;
    if (br_get(1)) wasted = (int)br_unary() + 1;
    bps -= wasted;
    if (bps <= 0 || bps > 33) return false;
    if (bps == 33) return false;                       /* 32-bit side channels: not handled */
    if (type == 0) {
        int32_t v = br_sget(bps);
        for (uint32_t i = 0; i < block; i++) s[i] = v;
    } else if (type == 1) {
        for (uint32_t i = 0; i < block; i++) s[i] = br_sget(bps);
    } else if (type >= 8 && type <= 12) {
        int order = type - 8;
        if ((uint32_t)order > block) return false;
        for (int i = 0; i < order; i++) s[i] = br_sget(bps);
        if (!residual(s, block, order)) return false;
        for (uint32_t i = (uint32_t)order; i < block; i++) {
            int64_t p;
            switch (order) {
                case 0: p = 0; break;
                case 1: p = s[i - 1]; break;
                case 2: p = 2 * (int64_t)s[i - 1] - s[i - 2]; break;
                case 3: p = 3 * (int64_t)s[i - 1] - 3 * (int64_t)s[i - 2] + s[i - 3]; break;
                default: p = 4 * (int64_t)s[i - 1] - 6 * (int64_t)s[i - 2] + 4 * (int64_t)s[i - 3] - s[i - 4]; break;
            }
            s[i] = (int32_t)(p + s[i]);
        }
    } else if (type >= 32) {
        int order = type - 31;
        if ((uint32_t)order > block) return false;
        for (int i = 0; i < order; i++) s[i] = br_sget(bps);
        int prec = (int)br_get(4) + 1;
        if (prec == 16) return false;
        int shift = br_sget(5);
        if (shift < 0) return false;
        int32_t c[32];
        for (int i = 0; i < order; i++) c[i] = br_sget(prec);
        if (!residual(s, block, order)) return false;
        for (uint32_t i = (uint32_t)order; i < block; i++) {
            int64_t sum = 0;
            const int32_t *h = s + i;
            for (int j = 0; j < order; j++) sum += (int64_t)c[j] * h[-1 - j];
            s[i] += (int32_t)(sum >> shift);
        }
    } else return false;
    if (wasted) for (uint32_t i = 0; i < block; i++) s[i] = (int32_t)((uint32_t)s[i] << wasted);   /* two's complement */
    return br_over <= 8;
}

/* one frame at buf[at] (header already checked); false if broken */
static bool frame(const fhdr_t *h, int16_t *out)
{
    const uint8_t *p = D.buf + D.at;
    br_start(p + h->hlen, D.buf + D.len);
    int nch = h->chan < 8 ? h->chan + 1 : 2;
    for (int c = 0; c < nch; c++) {
        int bps = h->bps;
        if ((h->chan == 8 || h->chan == 10) && c == 1) bps++;
        if (h->chan == 9 && c == 0) bps++;
        if (!subframe(D.ch[c], h->block, bps)) return false;
    }
    br_align();
    const uint8_t *e = br_pos();
    if (e + 2 > D.buf + D.len) return false;
    if (crc16(p, (uint32_t)(e - p)) != (uint16_t)(e[0] << 8 | e[1])) return false;
    D.at = (uint32_t)(e + 2 - D.buf);

    int32_t *a = D.ch[0], *b = D.ch[1];
    int sh = h->bps - 16;
    for (uint32_t i = 0; i < h->block; i++) {
        int64_t l, r;
        switch (h->chan) {
            case 0:  l = r = a[i]; break;
            case 8:  l = a[i]; r = (int64_t)a[i] - b[i]; break;                 /* left / side */
            case 9:  r = b[i]; l = (int64_t)a[i] + b[i]; break;                 /* side / right */
            case 10: { int64_t m = (int64_t)a[i] * 2 + (b[i] & 1);              /* mid / side */
                       l = (m + b[i]) >> 1; r = (m - b[i]) >> 1; break; }
            default: l = a[i]; r = b[i]; break;
        }
        int64_t m = (l + r) >> 1;
        m = sh > 0 ? m >> sh : m * (1 << -sh);
        if (m > 32767) m = 32767; else if (m < -32768) m = -32768;
        out[i] = (int16_t)m;
    }
    return true;
}

int flac_decode(int16_t *out, uint64_t *first_sample)
{
    for (;;) {
        refill();
        if (D.at + 16 > D.len) return 0;
        uint8_t *p = D.buf + D.at;
        fhdr_t h;
        if (p[0] == 0xFF && header(p, D.len - D.at, &h)) {
            uint32_t at = D.at;
            if (frame(&h, out)) {
                *first_sample = h.variable ? h.number : h.number * (D.in->max_block ? D.in->max_block : h.block);
                return (int)h.block;
            }
            D.at = at;
        }
        /* not a frame here: look for the next sync code */
        uint8_t *q = memchr(p + 1, 0xFF, D.len - D.at - 1);
        D.at = q ? (uint32_t)(q - D.buf) : D.len - 1;
    }
}
