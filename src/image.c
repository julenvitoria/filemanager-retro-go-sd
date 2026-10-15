/*
 * File Manager — image viewer: JPEG, PNG, GIF (animated too) and BMP.
 *
 * Every decoder streams the picture through one "sink" that scales it to fit the 320x240 LCD
 * (never more than the screen, small pictures enlarged by a whole factor), so the image never
 * has to be held in memory at full size:
 *   - JPEG: picojpeg, one MCU row at a time; pictures wider than 2560 px are decoded at 1/8
 *     (DC only, very fast). Progressive JPEGs are not supported by picojpeg.
 *   - PNG: miniz's inflate (32 KB window) + PNG row filters; all colour types and bit depths,
 *     interlaced too; alpha is blended over black.
 *   - GIF: own LZW decoder; frames are composed on a 320x240 canvas already at screen scale,
 *     so animations of any size play.
 *   - BMP: 8/16/24/32 bpp, uncompressed or BI_BITFIELDS.
 * Working memory: fm_scratch (192 KB) and, for GIF, the dialog background buffer as canvas.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "fm.h"
#include "third_party/picojpeg/picojpeg.h"
#include "third_party/miniz/miniz.h"

extern void wdog_refresh(void);
extern uint32_t HAL_GetTick(void);

/* ================================================================ helpers */

static bool ext_in(const char *name, const char *const *list)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    for (; *list; list++)
        if (strcasecmp(dot + 1, *list) == 0) return true;
    return false;
}

/* macOS writes a "._name" companion (AppleDouble metadata, not a picture) for every file it
 * copies to a FAT card: never treat those as images or videos. */
bool is_macos_meta(const char *name)
{
    const char *b = strrchr(name, '/');
    b = b ? b + 1 : name;
    return b[0] == '.' && b[1] == '_';
}

bool is_image_ext(const char *name)
{
    if (is_macos_meta(name)) return false;
    static const char *const L[] = { "jpg", "jpeg", "jpe", "png", "gif", "bmp", NULL };
    return ext_in(name, L);
}

static inline uint16_t rgb565(unsigned r, unsigned g, unsigned b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static uint32_t rd32le(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16le(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32be(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* buffered reader over a FILE */
typedef struct { FILE *f; uint8_t *buf; size_t size, pos, len; bool eof; } reader_t;

static void rd_init(reader_t *r, FILE *f, uint8_t *buf, size_t size)
{
    r->f = f; r->buf = buf; r->size = size; r->pos = r->len = 0; r->eof = false;
}
static bool rd_fill(reader_t *r)
{
    if (r->eof) return false;
    r->len = fread(r->buf, 1, r->size, r->f);
    r->pos = 0;
    if (!r->len) { r->eof = true; return false; }
    return true;
}
static int rd_byte(reader_t *r)
{
    if (r->pos >= r->len && !rd_fill(r)) return -1;
    return r->buf[r->pos++];
}
static bool rd_bytes(reader_t *r, void *dst, size_t n)
{
    uint8_t *d = dst;
    while (n) {
        if (r->pos >= r->len && !rd_fill(r)) return false;
        size_t k = r->len - r->pos;
        if (k > n) k = n;
        memcpy(d, r->buf + r->pos, k);
        r->pos += k; d += k; n -= k;
    }
    return true;
}
static bool rd_skip(reader_t *r, size_t n)
{
    while (n) {
        if (r->pos >= r->len && !rd_fill(r)) return false;
        size_t k = r->len - r->pos;
        if (k > n) k = n;
        r->pos += k; n -= k;
    }
    return true;
}

/* ================================================================ scaling sink */

static struct {
    int sw, sh;            /* source size */
    int dw, dh, ox, oy;    /* drawn size and position on the LCD */
    uint16_t *fb;          /* 320x240 target (LCD back buffer or GIF canvas) */
    uint16_t xmap[FM_W];   /* source x for each drawn x */
} S;

static void sink_begin(int w, int h, uint16_t *fb)
{
    S.sw = w; S.sh = h; S.fb = fb;
    int dw, dh;
    if (w * 2 <= FM_W && h * 2 <= FM_H) {               /* small: enlarge by a whole factor */
        int k = FM_W / w < FM_H / h ? FM_W / w : FM_H / h;
        dw = w * k; dh = h * k;
    } else {                                             /* fit, keeping the aspect ratio */
        dw = w; dh = h;
        if (dw > FM_W) { dh = (int)((int64_t)dh * FM_W / dw); dw = FM_W; }
        if (dh > FM_H) { dw = (int)((int64_t)dw * FM_H / dh); dh = FM_H; }
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
    }
    S.dw = dw; S.dh = dh;
    S.ox = (FM_W - dw) / 2; S.oy = (FM_H - dh) / 2;
    for (int x = 0; x < dw; x++)
        S.xmap[x] = (uint16_t)((int64_t)x * w / dw);
}

/* drawn rows covered by source row y: [*y0, *y1) */
static inline void sink_rows(int y, int *y0, int *y1)
{
    *y0 = (int)((int64_t)y * S.dh / S.sh);
    *y1 = (int)((int64_t)(y + 1) * S.dh / S.sh);
}

/* a full source row, already in RGB565 */
static void sink_row(int y, const uint16_t *row)
{
    int y0, y1;
    sink_rows(y, &y0, &y1);
    if (y0 >= y1) return;
    uint16_t *d = S.fb + (S.oy + y0) * FM_W + S.ox;
    for (int x = 0; x < S.dw; x++) d[x] = row[S.xmap[x]];
    for (int yy = y0 + 1; yy < y1; yy++)
        memcpy(S.fb + (S.oy + yy) * FM_W + S.ox, d, (size_t)S.dw * 2);
}

/* one source pixel (interlaced PNG) */
static void sink_px(int x, int y, uint16_t c)
{
    int y0, y1;
    sink_rows(y, &y0, &y1);
    int x0 = (int)((int64_t)x * S.dw / S.sw), x1 = (int)((int64_t)(x + 1) * S.dw / S.sw);
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++)
            S.fb[(S.oy + yy) * FM_W + S.ox + xx] = c;
}

/* ================================================================ JPEG (picojpeg) */

static reader_t *g_jr;

static unsigned char jpeg_need_bytes(unsigned char *buf, unsigned char size, unsigned char *got, void *data)
{
    (void)data;
    size_t n = 0;
    while (n < size) {
        int c = rd_byte(g_jr);
        if (c < 0) break;
        buf[n++] = (unsigned char)c;
    }
    *got = (unsigned char)n;
    return 0;
}

static img_result_t decode_jpeg(FILE *f)
{
    reader_t r;
    rd_init(&r, f, fm_scratch, 4096);
    g_jr = &r;

    /* Peek at the size first (decode_init parses the headers) to pick full or 1/8 decoding */
    pjpeg_image_info_t info;
    unsigned char st = pjpeg_decode_init(&info, jpeg_need_bytes, NULL, 0);
    if (st == PJPG_UNSUPPORTED_MODE) return IMG_PROGRESSIVE;
    if (st) return st == PJPG_NOT_JPEG ? IMG_ERROR : IMG_UNSUPPORTED;
    bool reduce = info.m_width > 2560;
    if (reduce) {
        fseek(f, 0, SEEK_SET);
        rd_init(&r, f, fm_scratch, 4096);
        st = pjpeg_decode_init(&info, jpeg_need_bytes, NULL, 1);
        if (st) return IMG_UNSUPPORTED;
    }

    int bw = reduce ? info.m_MCUWidth / 8 : info.m_MCUWidth;     /* pixels per MCU */
    int bh = reduce ? info.m_MCUHeight / 8 : info.m_MCUHeight;
    int w = reduce ? info.m_MCUSPerRow * bw : info.m_width;
    int h = reduce ? info.m_MCUSPerCol * bh : info.m_height;
    int band_w = info.m_MCUSPerRow * bw;
    size_t band_bytes = (size_t)band_w * bh * 2;
    if (4096 + band_bytes > FM_SCRATCH_SIZE) return IMG_TOO_BIG;
    uint16_t *band = (uint16_t *)(fm_scratch + 4096);

    sink_begin(w, h, ui_buffer());
    bool gray = info.m_scanType == PJPG_GRAYSCALE;
    for (int my = 0; my < info.m_MCUSPerCol; my++) {
        for (int mx = 0; mx < info.m_MCUSPerRow; mx++) {
            st = pjpeg_decode_mcu();
            if (st) return st == PJPG_NO_MORE_BLOCKS ? IMG_OK : IMG_ERROR;
            uint16_t *dst = band + mx * bw;
            if (reduce) {
                for (int by = 0; by < bh; by++)
                    for (int bx = 0; bx < bw; bx++) {
                        int o = by * 128 + bx * 64;
                        uint8_t R = info.m_pMCUBufR[gray ? 0 : o];
                        dst[by * band_w + bx] = gray ? rgb565(R, R, R)
                            : rgb565(R, info.m_pMCUBufG[o], info.m_pMCUBufB[o]);
                    }
            } else {
                for (int y = 0; y < bh; y++)
                    for (int x = 0; x < bw; x++) {
                        int o = ((y >> 3) * 128) + ((x >> 3) * 64) + (y & 7) * 8 + (x & 7);
                        uint8_t R = info.m_pMCUBufR[o];
                        dst[y * band_w + x] = gray ? rgb565(R, R, R)
                            : rgb565(R, info.m_pMCUBufG[o], info.m_pMCUBufB[o]);
                    }
            }
        }
        for (int y = 0; y < bh; y++) {
            int sy = my * bh + y;
            if (sy < h) sink_row(sy, band + y * band_w);
        }
        if ((my & 7) == 0) wdog_refresh();
    }
    return IMG_OK;
}

/* ================================================================ PNG (miniz inflate) */

typedef struct {
    reader_t r;
    uint32_t idat_left;    /* bytes left in the current IDAT chunk */
    bool done;
    tinfl_decompressor *inf;
    uint8_t *dict;         /* 32 KB inflate window, also the output buffer */
    size_t dict_ofs, avail_ofs, avail;   /* decompressed bytes not yet consumed */
    uint8_t *in; size_t in_len, in_pos;
    bool inflate_done, no_more_input;
} png_t;

/* next IDAT bytes into p->in */
static bool png_feed(png_t *p)
{
    while (p->idat_left == 0) {
        if (p->done) return false;
        uint8_t hdr[12];
        if (!rd_skip(&p->r, 4)) return false;                 /* CRC of the previous chunk */
        if (!rd_bytes(&p->r, hdr, 8)) return false;
        p->idat_left = rd32be(hdr);
        if (memcmp(hdr + 4, "IDAT", 4) != 0) { p->done = true; p->idat_left = 0; return false; }
    }
    size_t n = p->idat_left < 4096 ? p->idat_left : 4096;
    if (!rd_bytes(&p->r, p->in, n)) return false;
    p->idat_left -= (uint32_t)n;
    p->in_len = n; p->in_pos = 0;
    return true;
}

/* n decompressed bytes into dst; false at the end of the data or on error */
static bool png_read(png_t *p, uint8_t *dst, size_t n)
{
    while (n) {
        if (p->avail) {
            size_t k = p->avail < n ? p->avail : n;
            memcpy(dst, p->dict + p->avail_ofs, k);
            p->avail_ofs += k; p->avail -= k; dst += k; n -= k;
            continue;
        }
        if (p->inflate_done) return false;
        if (p->in_pos >= p->in_len && !p->no_more_input && !png_feed(p))
            p->no_more_input = true;
        size_t in_size = p->in_len - p->in_pos, out_size = TINFL_LZ_DICT_SIZE - p->dict_ofs;
        mz_uint32 flags = TINFL_FLAG_PARSE_ZLIB_HEADER | (p->no_more_input ? 0 : TINFL_FLAG_HAS_MORE_INPUT);
        tinfl_status st = tinfl_decompress(p->inf, p->in + p->in_pos, &in_size, p->dict, p->dict + p->dict_ofs,
                                           &out_size, flags);
        p->in_pos += in_size;
        p->avail_ofs = p->dict_ofs; p->avail = out_size;
        p->dict_ofs = (p->dict_ofs + out_size) & (TINFL_LZ_DICT_SIZE - 1);
        if (st == TINFL_STATUS_DONE) p->inflate_done = true;
        else if (st < 0) return false;
        if (!out_size && !in_size && p->no_more_input && st != TINFL_STATUS_HAS_MORE_OUTPUT) return false;
    }
    return true;
}

static void png_unfilter(uint8_t ft, uint8_t *cur, const uint8_t *prev, size_t len, int bpp)
{
    switch (ft) {
        case 1: for (size_t i = bpp; i < len; i++) cur[i] += cur[i - bpp]; break;
        case 2: for (size_t i = 0; i < len; i++) cur[i] += prev[i]; break;
        case 3:
            for (size_t i = 0; i < len; i++)
                cur[i] += (uint8_t)(((i >= (size_t)bpp ? cur[i - bpp] : 0) + prev[i]) >> 1);
            break;
        case 4:
            for (size_t i = 0; i < len; i++) {
                int a = i >= (size_t)bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= (size_t)bpp ? prev[i - bpp] : 0;
                int pp = a + b - c, pa = pp > a ? pp - a : a - pp, pb = pp > b ? pp - b : b - pp, pc = pp > c ? pp - c : c - pp;
                cur[i] += (uint8_t)((pa <= pb && pa <= pc) ? a : (pb <= pc) ? b : c);
            }
            break;
    }
}

static struct {
    int ctype, depth;
    uint16_t pal[256]; uint8_t pal_a[256];
    bool has_trns; uint16_t trns[3];
} P;

static inline uint16_t blend(unsigned r, unsigned g, unsigned b, unsigned a)
{
    return rgb565(r * a / 255, g * a / 255, b * a / 255);    /* over black */
}

/* sample i of a row with sub-byte depths */
static inline unsigned px_bits(const uint8_t *row, int i, int depth)
{
    int per = 8 / depth;
    return (row[i / per] >> ((per - 1 - i % per) * depth)) & ((1 << depth) - 1);
}

static uint16_t png_pixel(const uint8_t *row, int i)
{
    int d = P.depth;
    switch (P.ctype) {
        case 3: {
            unsigned idx = d < 8 ? px_bits(row, i, d) : row[i];
            uint16_t c = P.pal[idx];
            if (P.pal_a[idx] != 255) {
                unsigned r = (c >> 11) << 3, g = ((c >> 5) & 63) << 2, b = (c & 31) << 3;
                return blend(r, g, b, P.pal_a[idx]);
            }
            return c;
        }
        case 0: {
            unsigned v, raw;
            if (d == 16) { raw = (unsigned)(row[2 * i] << 8 | row[2 * i + 1]); v = row[2 * i]; }
            else if (d == 8) { raw = row[i]; v = raw; }
            else { raw = px_bits(row, i, d); v = raw * 255 / ((1 << d) - 1); }
            if (P.has_trns && raw == P.trns[0]) return 0;
            return rgb565(v, v, v);
        }
        case 2: {
            if (d == 16) {
                const uint8_t *q = row + 6 * i;
                if (P.has_trns && (q[0] << 8 | q[1]) == P.trns[0] && (q[2] << 8 | q[3]) == P.trns[1] && (q[4] << 8 | q[5]) == P.trns[2]) return 0;
                return rgb565(q[0], q[2], q[4]);
            }
            const uint8_t *q = row + 3 * i;
            if (P.has_trns && q[0] == P.trns[0] && q[1] == P.trns[1] && q[2] == P.trns[2]) return 0;
            return rgb565(q[0], q[1], q[2]);
        }
        case 4:
            if (d == 16) { const uint8_t *q = row + 4 * i; return blend(q[0], q[0], q[0], q[2]); }
            return blend(row[2 * i], row[2 * i], row[2 * i], row[2 * i + 1]);
        case 6:
            if (d == 16) { const uint8_t *q = row + 8 * i; return blend(q[0], q[2], q[4], q[6]); }
            return blend(row[4 * i], row[4 * i + 1], row[4 * i + 2], row[4 * i + 3]);
    }
    return 0;
}

static img_result_t decode_png(FILE *f)
{
    /* scratch: [reader 4K][in 4K][dict 32K][inflator][prev row][cur row][565 row] */
    uint8_t *s = fm_scratch;
    png_t p;
    memset(&p, 0, sizeof p);
    rd_init(&p.r, f, s, 4096);
    p.in = s + 4096;
    p.dict = s + 8192;
    p.inf = (tinfl_decompressor *)(s + 8192 + TINFL_LZ_DICT_SIZE);
    size_t used = 8192 + TINFL_LZ_DICT_SIZE + ((sizeof(tinfl_decompressor) + 7) & ~7u);

    uint8_t hdr[29];                      /* signature + IHDR (its CRC is skipped below) */
    if (!rd_bytes(&p.r, hdr, 29) || memcmp(hdr, "\x89PNG\r\n\x1a\n", 8) || memcmp(hdr + 12, "IHDR", 4))
        return IMG_ERROR;
    int w = (int)rd32be(hdr + 16), h = (int)rd32be(hdr + 20);
    P.depth = hdr[24]; P.ctype = hdr[25];
    int interlace = hdr[28];
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return IMG_TOO_BIG;
    int ch = P.ctype == 0 ? 1 : P.ctype == 2 ? 3 : P.ctype == 3 ? 1 : P.ctype == 4 ? 2 : P.ctype == 6 ? 4 : 0;
    if (!ch || (P.depth != 1 && P.depth != 2 && P.depth != 4 && P.depth != 8 && P.depth != 16)) return IMG_UNSUPPORTED;
    int bits = ch * P.depth;
    int bpp = bits >= 8 ? bits / 8 : 1;
    size_t rowbytes = ((size_t)w * bits + 7) / 8;
    if (used + 2 * (rowbytes + 8) + (size_t)w * 2 > FM_SCRATCH_SIZE) return IMG_TOO_BIG;
    uint8_t *prev = s + used, *cur = prev + rowbytes + 8;
    uint16_t *out = (uint16_t *)(cur + rowbytes + 8);

    for (int i = 0; i < 256; i++) { P.pal[i] = 0; P.pal_a[i] = 255; }
    P.has_trns = false;

    /* chunks up to the first IDAT (the reader is right after IHDR's data; its CRC is next) */
    for (;;) {
        uint8_t ck[8];
        if (!rd_skip(&p.r, 4) || !rd_bytes(&p.r, ck, 8)) return IMG_ERROR;
        uint32_t len = rd32be(ck);
        if (!memcmp(ck + 4, "IDAT", 4)) { p.idat_left = len; break; }
        if (!memcmp(ck + 4, "IEND", 4)) return IMG_ERROR;
        if (!memcmp(ck + 4, "PLTE", 4) && len <= 768) {
            uint8_t pl[768];
            if (!rd_bytes(&p.r, pl, len)) return IMG_ERROR;
            for (uint32_t i = 0; i < len / 3; i++) P.pal[i] = rgb565(pl[3 * i], pl[3 * i + 1], pl[3 * i + 2]);
        } else if (!memcmp(ck + 4, "tRNS", 4) && len <= 256) {
            uint8_t t[256];
            if (!rd_bytes(&p.r, t, len)) return IMG_ERROR;
            if (P.ctype == 3) for (uint32_t i = 0; i < len; i++) P.pal_a[i] = t[i];
            else if (P.ctype == 0 && len >= 2) { P.trns[0] = (uint16_t)(t[0] << 8 | t[1]); P.has_trns = true; }
            else if (P.ctype == 2 && len >= 6) {
                for (int i = 0; i < 3; i++) P.trns[i] = (uint16_t)(t[2 * i] << 8 | t[2 * i + 1]);
                if (P.depth == 8) for (int i = 0; i < 3; i++) P.trns[i] &= 0xFF;
                P.has_trns = true;
            }
        } else if (!rd_skip(&p.r, len)) return IMG_ERROR;
    }
    tinfl_init(p.inf);

    sink_begin(w, h, ui_buffer());
    static const uint8_t A7[7][4] = { {0,0,8,8}, {4,0,8,8}, {0,4,4,8}, {2,0,4,4}, {0,2,2,4}, {1,0,2,2}, {0,1,1,2} };
    int passes = interlace ? 7 : 1;
    for (int ps = 0; ps < passes; ps++) {
        int x0 = interlace ? A7[ps][0] : 0, y0 = interlace ? A7[ps][1] : 0;
        int dx = interlace ? A7[ps][2] : 1, dy = interlace ? A7[ps][3] : 1;
        int pw = (w - x0 + dx - 1) / dx, ph = (h - y0 + dy - 1) / dy;
        if (pw <= 0 || ph <= 0) continue;
        size_t rb = ((size_t)pw * bits + 7) / 8;
        memset(prev, 0, rb);
        for (int y = 0; y < ph; y++) {
            uint8_t ft;
            if (!png_read(&p, &ft, 1) || !png_read(&p, cur, rb))
                return y || ps ? IMG_OK : IMG_ERROR;          /* truncated: show what we have */
            if (ft > 4) return IMG_ERROR;
            png_unfilter(ft, cur, prev, rb, bpp);
            int sy = y0 + y * dy;
            if (!interlace) {
                int a, b;
                sink_rows(sy, &a, &b);
                if (a < b) {
                    for (int x = 0; x < S.dw; x++) out[x] = png_pixel(cur, S.xmap[x]);
                    /* out[] holds drawn pixels: copy them straight in */
                    for (int yy = a; yy < b; yy++)
                        memcpy(S.fb + (S.oy + yy) * FM_W + S.ox, out, (size_t)S.dw * 2);
                }
            } else {
                for (int x = 0; x < pw; x++) sink_px(x0 + x * dx, sy, png_pixel(cur, x));
            }
            uint8_t *t = prev; prev = cur; cur = t;
            if ((y & 31) == 0) wdog_refresh();
        }
    }
    return IMG_OK;
}

/* ================================================================ BMP */

static int mask_shift(uint32_t m) { int s = 0; if (!m) return 0; while (!(m & 1)) { m >>= 1; s++; } return s; }
static int mask_bits(uint32_t m)  { int b = 0; m >>= mask_shift(m); while (m & 1) { m >>= 1; b++; } return b; }

static img_result_t decode_bmp(FILE *f)
{
    uint8_t h[138];
    memset(h, 0, sizeof h);
    size_t got = fread(h, 1, sizeof h, f);
    if (got < 54 || h[0] != 'B' || h[1] != 'M') return IMG_ERROR;
    uint32_t data_off = rd32le(h + 10), dib = rd32le(h + 14);
    int32_t w = (int32_t)rd32le(h + 18), hgt = (int32_t)rd32le(h + 22);
    uint16_t bpp = rd16le(h + 28);
    uint32_t comp = rd32le(h + 30), ncolors = rd32le(h + 46);
    bool top_down = hgt < 0;
    if (top_down) hgt = -hgt;
    if (w <= 0 || hgt <= 0 || w > 8192 || hgt > 8192 || (comp != 0 && comp != 3) ||
        (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32))
        return IMG_UNSUPPORTED;
    uint32_t rm, gm, bm;
    if (comp == 3) { rm = rd32le(h + 54); gm = rd32le(h + 58); bm = rd32le(h + 62); }
    else if (bpp == 16) { rm = 0x7C00; gm = 0x03E0; bm = 0x001F; }
    else { rm = 0xFF0000; gm = 0x00FF00; bm = 0x0000FF; }
    int rs = mask_shift(rm), gs = mask_shift(gm), bs = mask_shift(bm);
    int rb = mask_bits(rm), gb = mask_bits(gm), bb = mask_bits(bm);

    static uint16_t pal[256];
    if (bpp == 8) {
        if (!ncolors || ncolors > 256) ncolors = 256;
        uint8_t q[4 * 256];
        fseek(f, 14 + (long)dib, SEEK_SET);
        if (fread(q, 4, ncolors, f) != ncolors) return IMG_ERROR;
        for (uint32_t i = 0; i < ncolors; i++) pal[i] = rgb565(q[4 * i + 2], q[4 * i + 1], q[4 * i]);
    }
    uint32_t stride = ((uint32_t)w * bpp / 8 + 3) & ~3u;
    if (stride + (uint32_t)w * 2 > FM_SCRATCH_SIZE) return IMG_TOO_BIG;
    uint8_t *line = fm_scratch;
    uint16_t *row = (uint16_t *)(fm_scratch + ((stride + 3) & ~3u));

    sink_begin(w, hgt, ui_buffer());
    for (int sy = 0; sy < hgt; sy++) {
        int a, b;
        sink_rows(sy, &a, &b);
        if (a >= b) continue;                        /* row not shown at this scale: skip reading it */
        int file_row = top_down ? sy : hgt - 1 - sy;
        fseek(f, (long)(data_off + (uint64_t)file_row * stride), SEEK_SET);
        if (fread(line, 1, stride, f) != stride) break;
        for (int x = 0; x < S.dw; x++) {
            int sx = S.xmap[x];
            uint16_t px;
            if (bpp == 8) px = pal[line[sx]];
            else {
                const uint8_t *q = line + (uint32_t)sx * (bpp / 8);
                uint32_t v = (bpp == 16) ? rd16le(q) : (bpp == 24) ? (uint32_t)(q[0] | q[1] << 8 | q[2] << 16) : rd32le(q);
                uint32_t r = (v & rm) >> rs, g = (v & gm) >> gs, bl = (v & bm) >> bs;
                r = rb >= 5 ? r >> (rb - 5) : r << (5 - rb);
                g = gb >= 6 ? g >> (gb - 6) : g << (6 - gb);
                bl = bb >= 5 ? bl >> (bb - 5) : bl << (5 - bb);
                px = (uint16_t)((r << 11) | (g << 5) | bl);
            }
            row[x] = px;
        }
        for (int yy = a; yy < b; yy++)
            memcpy(S.fb + (S.oy + yy) * FM_W + S.ox, row, (size_t)S.dw * 2);
        if ((sy & 15) == 0) wdog_refresh();
    }
    return IMG_OK;
}

/* ================================================================ GIF */

typedef struct {
    reader_t r;
    int w, h;
    uint16_t gpal[256]; bool has_gct; int bg;
    long first_frame;              /* file offset of the first block after the header */
    /* per frame */
    int delay_cs, disposal, transp;
    int fx, fy, fw, fh;
    /* LZW */
    uint16_t *prefix; uint8_t *suffix, *stack;
    uint8_t *idx_row;
} gif_t;

/* one decoded frame row (y within the frame) onto the canvas */
static void gif_row(gif_t *g, int y, const uint16_t *pal)
{
    int sy = g->fy + y;
    if (sy < 0 || sy >= g->h) return;
    int a, b;
    sink_rows(sy, &a, &b);
    if (a >= b) return;
    uint16_t *d = S.fb + (S.oy + a) * FM_W + S.ox;
    for (int x = 0; x < S.dw; x++) {
        int sx = S.xmap[x] - g->fx;
        if (sx < 0 || sx >= g->fw) continue;
        int i = g->idx_row[sx];
        if (i != g->transp) d[x] = pal[i];
    }
    for (int yy = a + 1; yy < b; yy++)
        memcpy(S.fb + (S.oy + yy) * FM_W + S.ox, d, (size_t)S.dw * 2);
}

/* LZW image data of one frame; rows go to gif_row() in display order */
static bool gif_lzw(gif_t *g, bool interlaced, const uint16_t *pal)
{
    int min_size = rd_byte(&g->r);
    if (min_size < 2 || min_size > 11) return false;
    int clear = 1 << min_size, eoi = clear + 1;
    int size = min_size + 1, next = eoi + 1, old = -1;
    uint8_t first = 0;
    uint32_t acc = 0; int nbits = 0, block_left = 0;
    int x = 0, y = 0, pass = 0, row_index = 0;
    static const int ISTART[4] = { 0, 4, 2, 1 }, ISTEP[4] = { 8, 8, 4, 2 };
    bool ended = false;

    for (;;) {
        while (nbits < size) {
            if (block_left == 0) {
                block_left = rd_byte(&g->r);
                if (block_left <= 0) { ended = true; break; }
            }
            int c = rd_byte(&g->r);
            if (c < 0) return false;
            block_left--;
            acc |= (uint32_t)c << nbits;
            nbits += 8;
        }
        if (ended) break;
        int code = acc & ((1 << size) - 1);
        acc >>= size; nbits -= size;
        if (code == clear) { size = min_size + 1; next = eoi + 1; old = -1; continue; }
        if (code == eoi) break;

        int sp = 0, c = code;
        if (old < 0) {
            if (code >= clear) return false;
            g->stack[sp++] = (uint8_t)code;
            first = (uint8_t)code;
        } else {
            if (code > next) return false;
            if (code == next) { g->stack[sp++] = first; c = old; }
            while (c >= clear) { g->stack[sp++] = g->suffix[c]; c = g->prefix[c]; if (sp >= 4096) return false; }
            g->stack[sp++] = (uint8_t)c;
            first = (uint8_t)c;
            if (next < 4096) {
                g->prefix[next] = (uint16_t)old;
                g->suffix[next] = first;
                next++;
                if (next == (1 << size) && size < 12) size++;
            }
        }
        old = code;
        while (sp) {
            if (y < g->fh) g->idx_row[x] = g->stack[--sp]; else sp--;
            if (++x == g->fw) {
                x = 0;
                if (y < g->fh) gif_row(g, y, pal);
                row_index++;
                if (interlaced) {
                    y += ISTEP[pass];
                    while (y >= g->fh && pass < 3) { pass++; y = ISTART[pass]; }
                } else y++;
                if (row_index >= g->fh) y = g->fh;   /* done: ignore the rest */
                if ((row_index & 31) == 0) wdog_refresh();
            }
        }
    }
    /* skip what is left of the data sub-blocks */
    while (block_left > 0) { rd_skip(&g->r, (size_t)block_left); block_left = rd_byte(&g->r); }
    if (!ended) { int n; while ((n = rd_byte(&g->r)) > 0) rd_skip(&g->r, (size_t)n); }
    return true;
}

static void skip_sub_blocks(reader_t *r)
{
    int n;
    while ((n = rd_byte(r)) > 0) rd_skip(r, (size_t)n);
}

/* decode up to the next frame; 1 = frame drawn, 0 = end of file, -1 = error */
static int gif_next_frame(gif_t *g)
{
    for (;;) {
        int b = rd_byte(&g->r);
        if (b < 0 || b == 0x3B) return 0;
        if (b == 0x21) {
            int label = rd_byte(&g->r);
            if (label == 0xF9) {
                uint8_t e[6];
                if (!rd_bytes(&g->r, e, 6)) return -1;          /* size, flags, delay, transp, 0 */
                g->disposal = (e[1] >> 2) & 7;
                g->delay_cs = e[2] | e[3] << 8;
                g->transp = (e[1] & 1) ? e[4] : -1;
            } else skip_sub_blocks(&g->r);
            continue;
        }
        if (b != 0x2C) return -1;
        uint8_t d[9];
        if (!rd_bytes(&g->r, d, 9)) return -1;
        g->fx = rd16le(d); g->fy = rd16le(d + 2); g->fw = rd16le(d + 4); g->fh = rd16le(d + 6);
        if (g->fw <= 0 || g->fh <= 0 || g->fw > 8192) return -1;
        uint16_t lpal[256];
        const uint16_t *pal = g->gpal;
        if (d[8] & 0x80) {
            int n = 2 << (d[8] & 7);
            for (int i = 0; i < n; i++) {
                uint8_t c[3];
                if (!rd_bytes(&g->r, c, 3)) return -1;
                lpal[i] = rgb565(c[0], c[1], c[2]);
            }
            pal = lpal;
        }
        if (!gif_lzw(g, (d[8] & 0x40) != 0, pal)) return -1;
        return 1;
    }
}

static void canvas_show(uint16_t *canvas)
{
    ui_begin();
    memcpy(ui_buffer(), canvas, FM_W * FM_H * 2);
    ui_end();
}

/* plays the GIF until a key is pressed; returns the key (0 after a firmware menu) */
static uint32_t play_gif(FILE *f, img_result_t *res)
{
    static gif_t g;
    memset(&g, 0, sizeof g);
    uint8_t *s = fm_scratch;
    rd_init(&g.r, f, s, 8192);
    g.prefix = (uint16_t *)(s + 8192);
    g.suffix = s + 8192 + 8192;
    g.stack = g.suffix + 4096;
    g.idx_row = g.stack + 4100;
    uint8_t h[13];
    *res = IMG_ERROR;
    if (!rd_bytes(&g.r, h, 13) || memcmp(h, "GIF", 3)) return 0;
    g.w = rd16le(h + 6); g.h = rd16le(h + 8);
    if (g.w <= 0 || g.h <= 0) return 0;
    if (h[10] & 0x80) {
        int n = 2 << (h[10] & 7);
        for (int i = 0; i < n; i++) {
            uint8_t c[3];
            if (!rd_bytes(&g.r, c, 3)) return 0;
            g.gpal[i] = rgb565(c[0], c[1], c[2]);
        }
    }
    g.first_frame = ftell(f) - (long)(g.r.len - g.r.pos);
    uint16_t *canvas = fm_canvas();
    sink_begin(g.w, g.h, canvas);
    memset(canvas, 0, FM_W * FM_H * 2);

    int frames = 0;
    for (;;) {
        g.delay_cs = 0; g.disposal = 0; g.transp = -1;
        int r = gif_next_frame(&g);
        if (r < 0 && frames == 0) return 0;
        if (r <= 0) {                                   /* end (or a broken tail): loop */
            if (frames <= 1) {                          /* still image: just wait */
                *res = IMG_OK;
                return input_wait();
            }
            fseek(f, g.first_frame, SEEK_SET);
            rd_init(&g.r, f, s, 8192);
            memset(canvas, 0, FM_W * FM_H * 2);
            frames = 0;
            continue;
        }
        frames++;
        *res = IMG_OK;
        canvas_show(canvas);
        /* frame delay (browsers treat < 2 as 10), watching the keys meanwhile */
        uint32_t wait_ms = (uint32_t)(g.delay_cs < 2 ? 10 : g.delay_cs) * 10;
        uint32_t t0 = HAL_GetTick();
        do {
            uint32_t k = input_poll_pressed();
            if (k) return k;
            HAL_Delay(5);
        } while (HAL_GetTick() - t0 < wait_ms);
        /* disposal 2: clear the frame's area for the next one ("restore to previous" is
         * treated the same: there is no room for a second canvas) */
        if (g.disposal == 2 || g.disposal == 3) {
            for (int y = g.fy; y < g.fy + g.fh && y < g.h; y++) {
                int a, b;
                sink_rows(y, &a, &b);
                for (int yy = a; yy < b; yy++)
                    for (int x = 0; x < S.dw; x++) {
                        int sx = S.xmap[x];
                        if (sx >= g.fx && sx < g.fx + g.fw)
                            canvas[(S.oy + yy) * FM_W + S.ox + x] = 0;
                    }
            }
        }
    }
}

/* ================================================================ viewer */

static bool has_ext(const char *p, const char *a, const char *b)
{
    const char *dot = strrchr(p, '.');
    return dot && (strcasecmp(dot + 1, a) == 0 || (b && strcasecmp(dot + 1, b) == 0));
}

/* Shows one image; returns the key that closed it (K_LEFT / K_RIGHT browse the folder). */
uint32_t view_image(const char *path, img_result_t *result)
{
    FILE *f = fopen(path, "rb");
    if (!f) { *result = IMG_ERROR; return 0; }
    uint8_t magic[4] = { 0 };
    fread(magic, 1, 4, f);
    fseek(f, 0, SEEK_SET);

    if (magic[0] == 'G' && magic[1] == 'I' && magic[2] == 'F') {
        uint32_t k = play_gif(f, result);
        fclose(f);
        return k;
    }

    /* big files take a moment: say so on the screen being shown */
    long size = 0;
    fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET);
    if (size > 200 * 1024) {
        ui_begin();
        ui_text_w(0, FM_ROWS / 2, FM_COLS, "", C_TEXT, C_BG);
        char t[48];
        snprintf(t, sizeof t, "%s", tr(T_LOADING));
        ui_text((FM_COLS - (int)strlen(t)) / 2, FM_ROWS / 2, t, C_DIM, C_BG);
        ui_end();
    }

    ui_begin();
    img_result_t r;
    if (magic[0] == 0xFF && magic[1] == 0xD8) r = decode_jpeg(f);
    else if (magic[0] == 0x89 && magic[1] == 'P') r = decode_png(f);
    else if (magic[0] == 'B' && magic[1] == 'M') r = decode_bmp(f);
    else r = (has_ext(path, "jpg", "jpeg") || has_ext(path, "png", "gif")) ? IMG_ERROR : IMG_UNSUPPORTED;
    fclose(f);
    *result = r;
    if (r != IMG_OK) return 0;
    ui_end();
    return input_wait();
}
