/*
 * File Manager — LZMA / LZMA2 decoder (tools overlay), written from the LZMA specification
 * (Igor Pavlov's LzmaSpec, public domain). What is different: the window is smaller than the
 * dictionary, and bytes further back come from the card (lz_t.far). See lzma.h.
 */
#include <string.h>
#include "lzma.h"

#define FLUSH_EVERY (16 * 1024)

/* ---------------------------------------------------------------- input */

static uint8_t rd(lz_t *z)
{
    if (z->ipos == z->ilen) {
        uint32_t want = z->in_left < z->isize ? (uint32_t)z->in_left : z->isize;
        if (!want || z->read(z->in, z->ibuf, want) != want) {
            z->in_err = true;
            return 0;
        }
        z->in_left -= want;
        z->ipos = 0;
        z->ilen = want;
    }
    return z->ibuf[z->ipos++];
}

/* ---------------------------------------------------------------- range coder */

static bool rc_init(lz_t *z)
{
    z->range = 0xFFFFFFFFu;
    z->code = 0;
    uint8_t b = rd(z);
    for (int i = 0; i < 4; i++) z->code = (z->code << 8) | rd(z);
    return b == 0 && z->code != z->range && !z->in_err;
}

static inline unsigned bit(lz_t *z, uint16_t *p)
{
    uint32_t bound = (z->range >> 11) * *p;
    unsigned b;
    if (z->code < bound) {
        z->range = bound;
        *p += (2048 - *p) >> 5;
        b = 0;
    } else {
        z->range -= bound;
        z->code -= bound;
        *p -= *p >> 5;
        b = 1;
    }
    if (z->range < (1u << 24)) {
        z->range <<= 8;
        z->code = (z->code << 8) | rd(z);
    }
    return b;
}

static uint32_t direct(lz_t *z, unsigned n)
{
    uint32_t r = 0;
    while (n--) {
        z->range >>= 1;
        z->code -= z->range;
        uint32_t t = 0 - (z->code >> 31);
        z->code += z->range & t;
        if (z->code == z->range) z->in_err = true;      /* (corrupted) */
        if (z->range < (1u << 24)) {
            z->range <<= 8;
            z->code = (z->code << 8) | rd(z);
        }
        r = (r << 1) + (t + 1);
    }
    return r;
}

static unsigned tree(lz_t *z, uint16_t *p, unsigned bits)
{
    unsigned m = 1;
    for (unsigned i = 0; i < bits; i++) m = (m << 1) + bit(z, p + m);
    return m - (1u << bits);
}

static unsigned tree_rev(lz_t *z, uint16_t *p, unsigned bits)
{
    unsigned m = 1, sym = 0;
    for (unsigned i = 0; i < bits; i++) {
        unsigned b = bit(z, p + m);
        m = (m << 1) + b;
        sym |= b << i;
    }
    return sym;
}

/* ---------------------------------------------------------------- model */

enum {
    P_ISMATCH = 0, P_ISREP = 192, P_ISREPG0 = 204, P_ISREPG1 = 216, P_ISREPG2 = 228,
    P_ISREP0LONG = 240, P_POSSLOT = 432, P_SPECPOS = 688, P_ALIGN = 803, P_LEN = 819,
    P_REPLEN = 1333, P_LIT = 1847,
};
/* a length decoder: choice, choice2, low[16][8], mid[16][8], high[256] */
enum { L_CHOICE = 0, L_CHOICE2 = 1, L_LOW = 2, L_MID = 130, L_HIGH = 258 };

uint32_t lz_probs_needed(unsigned lc_plus_lp) { return P_LIT + (0x300u << lc_plus_lp); }

static bool set_props(lz_t *z, unsigned d)
{
    if (d >= 9 * 5 * 5) return false;
    z->lc = d % 9;
    d /= 9;
    z->lp = d % 5;
    z->pb = d / 5;
    return lz_probs_needed(z->lc + z->lp) <= z->probs_max;
}

static void reset_state(lz_t *z)
{
    uint32_t n = lz_probs_needed(z->lc + z->lp);
    for (uint32_t i = 0; i < n; i++) z->probs[i] = 1024;
    z->state = 0;
    z->rep[0] = z->rep[1] = z->rep[2] = z->rep[3] = 0;
}

static unsigned len_decode(lz_t *z, uint16_t *p, unsigned pos_state)
{
    if (!bit(z, p + L_CHOICE)) return tree(z, p + L_LOW + (pos_state << 3), 3);
    if (!bit(z, p + L_CHOICE2)) return 8 + tree(z, p + L_MID + (pos_state << 3), 3);
    return 16 + tree(z, p + L_HIGH, 8);
}

/* ---------------------------------------------------------------- window */

static bool flush_some(lz_t *z)
{
    while (z->flushed < z->pos) {
        uint32_t back = (uint32_t)(z->pos - z->flushed);
        uint32_t start = z->wpos >= back ? z->wpos - back : z->wpos + z->wsize - back;
        uint32_t n = z->wsize - start < back ? z->wsize - start : back;
        if (!z->put(z, z->win + start, n)) { z->error = LZ_STOPPED; return false; }
        z->flushed += n;
    }
    return true;
}

bool lz_flush(lz_t *z) { return flush_some(z); }

static inline bool out(lz_t *z, uint8_t b)
{
    z->win[z->wpos] = b;
    if (++z->wpos == z->wsize) z->wpos = 0;
    z->pos++;
    if (z->pos - z->flushed >= FLUSH_EVERY) return flush_some(z);
    return true;
}

/* the byte `d` back (d >= 1) */
static inline uint8_t back(lz_t *z, uint32_t d)
{
    if (d <= z->wsize) return z->win[z->wpos >= d ? z->wpos - d : z->wpos + z->wsize - d];
    uint8_t b = 0;
    z->far_reads++;
    if (!z->far(z, z->pos - d, &b, 1)) z->error = LZ_FAR;
    return b;
}

/* len bytes from rep0 + 1 back */
static bool copy_match(lz_t *z, uint32_t d, unsigned len)
{
    if (d <= z->wsize) {
        while (len--) if (!out(z, back(z, d))) return false;
        return !z->error;
    }
    uint8_t buf[274];                     /* (d > wsize >= len: the source does not overlap) */
    z->far_reads++;
    if (!z->far(z, z->pos - d, buf, len)) { z->error = LZ_FAR; return false; }
    for (unsigned i = 0; i < len; i++) if (!out(z, buf[i])) return false;
    return true;
}

/* ---------------------------------------------------------------- LZMA */

/* Decodes until pos == end (or the end marker, when allowed) */
static bool lzma_run(lz_t *z, uint64_t end, bool marker_ok)
{
    uint16_t *P = z->probs;
    unsigned pb_mask = (1u << z->pb) - 1, lp_mask = (1u << z->lp) - 1;
    while (z->pos < end) {
        if (z->in_err) { z->error = LZ_INPUT; return false; }
        if (z->error) return false;
        unsigned ps = (unsigned)z->pos & pb_mask;
        uint32_t s = z->state;
        if (!bit(z, P + P_ISMATCH + (s << 4) + ps)) {
            unsigned prev = z->pos > z->dict_start ? back(z, 1) : 0;
            uint16_t *lit = P + P_LIT + 0x300 * ((((unsigned)z->pos & lp_mask) << z->lc) + (prev >> (8 - z->lc)));
            unsigned sym = 1;
            if (s >= 7) {
                if (z->pos - z->dict_start <= z->rep[0]) { z->error = LZ_DATA; return false; }
                unsigned mb = back(z, z->rep[0] + 1);
                do {
                    unsigned mbit = (mb >> 7) & 1;
                    mb <<= 1;
                    unsigned b = bit(z, lit + ((1 + mbit) << 8) + sym);
                    sym = (sym << 1) | b;
                    if (mbit != b) break;
                } while (sym < 0x100);
            }
            while (sym < 0x100) sym = (sym << 1) | bit(z, lit + sym);
            z->state = s < 4 ? 0 : s < 10 ? s - 3 : s - 6;
            if (!out(z, (uint8_t)sym)) return false;
            continue;
        }
        unsigned len;
        if (bit(z, P + P_ISREP + s)) {
            if (z->pos == z->dict_start) { z->error = LZ_DATA; return false; }
            if (!bit(z, P + P_ISREPG0 + s)) {
                if (!bit(z, P + P_ISREP0LONG + (s << 4) + ps)) {         /* short rep: 1 byte */
                    z->state = s < 7 ? 9 : 11;
                    if (z->pos - z->dict_start <= z->rep[0]) { z->error = LZ_DATA; return false; }
                    if (!out(z, back(z, z->rep[0] + 1))) return false;
                    continue;
                }
            } else {
                uint32_t d;
                if (!bit(z, P + P_ISREPG1 + s)) {
                    d = z->rep[1];
                } else {
                    if (!bit(z, P + P_ISREPG2 + s)) {
                        d = z->rep[2];
                    } else {
                        d = z->rep[3];
                        z->rep[3] = z->rep[2];
                    }
                    z->rep[2] = z->rep[1];
                }
                z->rep[1] = z->rep[0];
                z->rep[0] = d;
            }
            len = len_decode(z, P + P_REPLEN, ps);
            z->state = s < 7 ? 8 : 11;
        } else {
            z->rep[3] = z->rep[2];
            z->rep[2] = z->rep[1];
            z->rep[1] = z->rep[0];
            len = len_decode(z, P + P_LEN, ps);
            z->state = s < 7 ? 7 : 10;
            unsigned ls = len < 3 ? len : 3;
            unsigned slot = tree(z, P + P_POSSLOT + (ls << 6), 6);
            uint32_t d;
            if (slot < 4) {
                d = slot;
            } else {
                unsigned nd = (slot >> 1) - 1;
                d = (2 | (slot & 1)) << nd;
                if (slot < 14) {
                    d += tree_rev(z, P + P_SPECPOS + d - slot, nd);
                } else {
                    d += direct(z, nd - 4) << 4;
                    d += tree_rev(z, P + P_ALIGN, 4);
                }
            }
            if (d == 0xFFFFFFFFu) {                                  /* end marker */
                if (!marker_ok) { z->error = LZ_DATA; return false; }
                return true;
            }
            z->rep[0] = d;
        }
        len += 2;
        if (z->rep[0] >= z->pos - z->dict_start) { z->error = LZ_DATA; return false; }
        if (len > end - z->pos) { z->error = LZ_DATA; return false; }
        if (!copy_match(z, z->rep[0] + 1, len)) return false;
    }
    return !z->in_err && !z->error;
}

bool lzma_decode(lz_t *z, const uint8_t *props, uint64_t size)
{
    z->error = LZ_OK;
    if (!set_props(z, props[0])) { z->error = LZ_PROPS; return false; }
    reset_state(z);
    z->dict_start = z->pos;
    if (!rc_init(z)) { z->error = LZ_INPUT; return false; }
    return lzma_run(z, z->pos + size, true) && flush_some(z);
}

bool lzma2_decode(lz_t *z, uint8_t prop, uint64_t size)
{
    (void)prop;                          /* (the dictionary size: the window is what it is) */
    z->error = LZ_OK;
    uint64_t end = z->pos + size;
    bool need_dict = true, need_props = true;
    for (;;) {
        unsigned c = rd(z);
        if (z->in_err) { z->error = LZ_INPUT; return false; }
        if (c == 0) break;
        if (c == 1 || c == 2) {                                     /* stored chunk */
            uint32_t n = ((uint32_t)rd(z) << 8 | rd(z)) + 1;
            if (c == 1) { z->dict_start = z->pos; need_dict = false; }
            else if (need_dict) { z->error = LZ_DATA; return false; }
            while (n--) {
                uint8_t b = rd(z);
                if (z->in_err) { z->error = LZ_INPUT; return false; }
                if (!out(z, b)) return false;
            }
            continue;
        }
        if (c < 0x80) { z->error = LZ_DATA; return false; }
        uint32_t un = ((c & 0x1F) << 16) + ((uint32_t)rd(z) << 8) + rd(z) + 1;
        rd(z); rd(z);                                                /* packed size: not needed */
        unsigned mode = (c >> 5) & 3;
        if (mode == 3) { z->dict_start = z->pos; need_dict = false; }
        else if (need_dict) { z->error = LZ_DATA; return false; }
        if (mode >= 2) {
            if (!set_props(z, rd(z)) || z->lc + z->lp > 4) { z->error = LZ_PROPS; return false; }
            need_props = false;
        } else if (need_props) { z->error = LZ_DATA; return false; }
        if (mode >= 1) reset_state(z);
        if (!rc_init(z)) { z->error = LZ_INPUT; return false; }
        if (!lzma_run(z, z->pos + un, false)) return false;
    }
    if (z->pos != end) { z->error = LZ_DATA; return false; }
    return flush_some(z);
}

bool lz_copy(lz_t *z, uint64_t size)
{
    z->error = LZ_OK;
    z->dict_start = z->pos;
    while (size--) {
        uint8_t b = rd(z);
        if (z->in_err) { z->error = LZ_INPUT; return false; }
        if (!out(z, b)) return false;
    }
    return flush_some(z);
}
