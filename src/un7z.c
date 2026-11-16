/*
 * File Manager — .7z extraction (tools overlay): LZMA, LZMA2 and stored folders, with the x86
 * BCJ filter in front (7-Zip's for .exe files); solid or not; plain or compressed headers.
 * Not: PPMd, BZip2, Deflate, BCJ2, ARM filters, encryption -- those entries are reported.
 *
 * Work memory (ex_mem), in this order:
 *   LZMA model (28 KB) | packed input (8 KB) | 4 x 4 KB pages read back from the card |
 *   the archive's file list | the window (the rest: ~120-140 KB)
 * A match further back than the window reads the bytes again from the files already written
 * (lzma.c); the pages keep the last few such reads.
 */
#include <string.h>
#include <stdio.h>
#include "extract.h"
#include "hash.h"
#include "lzma.h"

#define PROBS_BYTES (28 * 1024)          /* lc + lp <= 4 */
#define IN_BYTES    (8 * 1024)
#ifndef PAGE
#define PAGE        4096u
#endif
#ifndef PAGES
#define PAGES       4
#endif
#define MIN_WINDOW  (32 * 1024)

/* ---------------------------------------------------------------- the archive file(s) */
/* "name.7z.001", ".002"... (7-Zip's "split to volumes", what an iPhone makes of a big 7z) are
 * read as one file. */
#define MV_MAX 128
typedef struct {
    char base[512];                      /* the path without the 3 digits ("" = one file) */
    uint64_t start[MV_MAX + 1];          /* where each part begins; start[n] = the end */
    uint32_t n, cur;
    FILE *f;
    uint64_t pos;
} mv_t;

static bool mv_open_part(mv_t *v, uint32_t i)
{
    if (v->f && v->cur == i) return true;
    if (v->f) fclose(v->f);
    v->f = NULL;
    char p[520];
    if (v->base[0]) snprintf(p, sizeof p, "%s%03u", v->base, (unsigned)(i + 1));
    else return false;
    v->f = fopen(p, "rb");
    v->cur = i;
    return v->f != NULL;
}

static bool mv_open(mv_t *v, const char *path)
{
    memset(v, 0, sizeof *v);
    size_t n = strlen(path);
    if (n > 4 && n < sizeof v->base && strcmp(path + n - 4, ".001") == 0) {
        memcpy(v->base, path, n - 3);
        v->base[n - 3] = '\0';
        uint64_t at = 0;
        for (uint32_t i = 0; i < MV_MAX; i++) {
            char p[520];
            snprintf(p, sizeof p, "%s%03u", v->base, (unsigned)(i + 1));
            FILE *f = fopen(p, "rb");
            if (!f) break;
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fclose(f);
            v->start[v->n++] = at;
            at += (uint64_t)sz;
        }
        v->start[v->n] = at;
        return v->n > 0 && mv_open_part(v, 0);
    }
    v->f = fopen(path, "rb");
    if (!v->f) return false;
    fseek(v->f, 0, SEEK_END);
    v->start[0] = 0;
    v->start[1] = (uint64_t)ftell(v->f);
    v->n = 1;
    return fseek(v->f, 0, SEEK_SET) == 0;
}

static bool mv_seek(mv_t *v, uint64_t pos)
{
    if (pos > v->start[v->n]) return false;
    v->pos = pos;
    uint32_t i = 0;
    while (i + 1 < v->n && pos >= v->start[i + 1]) i++;
    if (v->base[0] && !mv_open_part(v, i)) return false;
    return fseek(v->f, (long)(pos - v->start[i]), SEEK_SET) == 0;
}

static size_t mv_read(void *ctx, uint8_t *buf, size_t n)
{
    mv_t *v = ctx;
    size_t got = 0;
    while (got < n) {
        uint32_t i = v->base[0] ? v->cur : 0;
        if (v->pos >= v->start[v->n]) break;
        if (v->base[0] && v->pos >= v->start[i + 1]) {       /* on to the next part */
            if (!mv_seek(v, v->pos)) break;
            continue;
        }
        uint64_t room = v->start[i + 1] - v->pos;
        size_t k = n - got < room ? n - got : (size_t)room;
        size_t r = fread(buf + got, 1, k, v->f);
        got += r;
        v->pos += r;
        if (r != k) break;
    }
    return got;
}

static void mv_close(mv_t *v)
{
    if (v->f) fclose(v->f);
    v->f = NULL;
}

/* ---------------------------------------------------------------- the archive's description */

enum { M_COPY, M_LZMA, M_LZMA2, M_NONE };
typedef struct {
    uint8_t method, bcj, props[5], has_crc;
    uint32_t crc;
    uint64_t unpack;                     /* what the folder unpacks to */
    uint64_t pack_off, pack_size;        /* its packed stream, from the start of the file */
    uint32_t streams;                    /* files in it */
} folder_t;

enum { F_STREAM = 1, F_DIR = 2, F_CRC = 4, F_TEMP = 8, F_FAILED = 16 };
typedef struct {
    uint64_t size;
    uint32_t crc, name;                  /* name: offset in the name pool */
    uint8_t flags;
} file_t;

typedef struct {
    const uint8_t *p, *end;
    bool bad;
} rd_t;

static uint8_t byte(rd_t *r)
{
    if (r->p >= r->end) { r->bad = true; return 0; }
    return *r->p++;
}

static uint64_t num(rd_t *r)
{
    uint8_t first = byte(r), mask = 0x80;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        if (!(first & mask)) return v | ((uint64_t)(first & (mask - 1)) << (8 * i));
        v |= (uint64_t)byte(r) << (8 * i);
        mask >>= 1;
    }
    return v;
}

static uint32_t u32(rd_t *r)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)byte(r) << (8 * i);
    return v;
}

static void skip(rd_t *r, uint64_t n)
{
    if (n > (uint64_t)(r->end - r->p)) { r->bad = true; r->p = r->end; return; }
    r->p += n;
}

/* The archive, as read from its header */
typedef struct {
    uint8_t *mem, *mem_end, *bump;       /* the file list's memory */
    folder_t *folders;
    uint32_t n_folders;
    uint64_t *sub_sizes;                 /* (while reading: each stream file's size) */
    uint32_t *sub_crc;
    uint8_t *sub_has_crc;
    uint32_t n_sub;
    file_t *files;
    uint32_t n_files;
    char *names;
    uint32_t names_used, names_max;
    uint64_t pack_pos;
    uint64_t *pack_sizes;
    uint32_t n_pack;
} arc_t;

static void *bump(arc_t *a, size_t n)
{
    n = (n + 7) & ~(size_t)7;
    if ((size_t)(a->mem_end - a->bump) < n) return NULL;
    void *p = a->bump;
    a->bump += n;
    memset(p, 0, n);
    return p;
}

/* a bit vector, MSB first; all_defined_byte: the "all set" byte comes first */
static bool bits(rd_t *r, uint8_t *out, uint32_t n, bool all_defined_byte)
{
    if (all_defined_byte && byte(r)) { memset(out, 1, n); return !r->bad; }
    uint8_t cur = 0, mask = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!mask) { cur = byte(r); mask = 0x80; }
        out[i] = (cur & mask) != 0;
        mask >>= 1;
    }
    return !r->bad;
}

static bool pack_info(rd_t *r, arc_t *a)
{
    a->pack_pos = num(r);
    a->n_pack = (uint32_t)num(r);
    if (a->n_pack > 1000000 || !(a->pack_sizes = bump(a, a->n_pack * 8ull))) return false;
    for (;;) {
        uint64_t t = num(r);
        if (r->bad) return false;
        if (t == 0) return true;
        if (t == 0x09) {
            for (uint32_t i = 0; i < a->n_pack; i++) a->pack_sizes[i] = num(r);
        } else if (t == 0x0A) {                                        /* digests: skipped */
            uint8_t *def = bump(a, a->n_pack);
            if (!def || !bits(r, def, a->n_pack, true)) return false;
            for (uint32_t i = 0; i < a->n_pack; i++) if (def[i]) u32(r);
            a->bump = def;
        } else {
            skip(r, num(r));
        }
    }
}

/* One folder: which coders, how they are chained; only the chains this can unpack are kept */
typedef struct { uint32_t id; uint8_t props[5], nprops; uint32_t in, out; } coder_t;

static bool folder(rd_t *r, folder_t *f, uint32_t *n_out, uint32_t *main_out, uint32_t *packed_streams)
{
    coder_t c[4];
    uint32_t nc = (uint32_t)num(r), tin = 0, tout = 0;
    if (nc == 0 || nc > 4) return false;
    for (uint32_t i = 0; i < nc; i++) {
        uint8_t fl = byte(r);
        unsigned idsz = fl & 0x0F;
        if (fl & 0x80 || idsz > 4) return false;
        c[i].id = 0;
        for (unsigned k = 0; k < idsz; k++) c[i].id = (c[i].id << 8) | byte(r);
        c[i].in = c[i].out = 1;
        if (fl & 0x10) { c[i].in = (uint32_t)num(r); c[i].out = (uint32_t)num(r); }
        c[i].nprops = 0;
        if (fl & 0x20) {
            uint64_t n = num(r);
            for (uint64_t k = 0; k < n; k++) {
                uint8_t b = byte(r);
                if (k < 5) c[i].props[k] = b;
            }
            c[i].nprops = n < 5 ? (uint8_t)n : 5;
        }
        if (c[i].in > 8 || c[i].out > 8) return false;
        tin += c[i].in;
        tout += c[i].out;
    }
    uint32_t bound_out = 0;                                 /* (bit set: that output feeds a coder) */
    for (uint32_t i = 0; i + 1 < tout; i++) {
        num(r);
        uint64_t o = num(r);
        if (o < 32) bound_out |= 1u << o;
    }
    uint32_t npk = tin - (tout - 1);
    if (npk > 1) for (uint32_t i = 0; i < npk; i++) num(r);
    *n_out = tout;
    *packed_streams = npk;
    *main_out = 0;
    for (uint32_t i = 0; i < tout; i++) if (!(bound_out & (1u << i))) { *main_out = i; break; }
    /* what this can unpack: one coder, or BCJ x86 fed by one coder */
    f->method = M_NONE;
    f->bcj = 0;
    const coder_t *m = NULL;
    if (nc == 1 && c[0].in == 1) m = &c[0];
    else if (nc == 2 && c[0].id == 0x03030103 && c[0].in == 1 && c[1].in == 1 && c[1].out == 1) { m = &c[1]; f->bcj = 1; }
    if (m) {
        if (m->id == 0x00) f->method = M_COPY;
        else if (m->id == 0x030101 && m->nprops == 5) f->method = M_LZMA;
        else if (m->id == 0x21 && m->nprops >= 1) f->method = M_LZMA2;
        memcpy(f->props, m->props, 5);
    }
    return !r->bad;
}

static bool unpack_info(rd_t *r, arc_t *a)
{
    if (byte(r) != 0x0B) return false;
    a->n_folders = (uint32_t)num(r);
    if (byte(r) != 0) return false;                     /* (folders kept elsewhere: no) */
    if (a->n_folders > 1000000 || !(a->folders = bump(a, a->n_folders * (uint64_t)sizeof(folder_t)))) return false;
    uint8_t *nout = bump(a, a->n_folders), *mainout = bump(a, a->n_folders);
    if (a->n_folders && (!nout || !mainout)) return false;
    uint32_t pk = 0;
    uint64_t off = 32 + a->pack_pos;
    for (uint32_t i = 0; i < a->n_folders; i++) {
        uint32_t no, mo, np;
        if (!folder(r, &a->folders[i], &no, &mo, &np) || no > 8) return false;
        nout[i] = (uint8_t)no;
        mainout[i] = (uint8_t)mo;
        folder_t *f = &a->folders[i];
        if (np != 1) f->method = M_NONE;
        f->pack_off = off;
        f->pack_size = pk < a->n_pack ? a->pack_sizes[pk] : 0;
        for (uint32_t k = 0; k < np && pk < a->n_pack; k++) off += a->pack_sizes[pk++];
        f->streams = 1;
    }
    for (;;) {
        uint64_t t = num(r);
        if (r->bad) return false;
        if (t == 0) break;
        if (t == 0x0C) {
            for (uint32_t i = 0; i < a->n_folders; i++)
                for (uint32_t k = 0; k < nout[i]; k++) {
                    uint64_t s = num(r);
                    if (k == mainout[i]) a->folders[i].unpack = s;
                }
        } else if (t == 0x0A) {
            uint8_t *def = bump(a, a->n_folders);
            if (!def || !bits(r, def, a->n_folders, true)) return false;
            for (uint32_t i = 0; i < a->n_folders; i++)
                if (def[i]) { a->folders[i].crc = u32(r); a->folders[i].has_crc = 1; }
        } else {
            skip(r, num(r));
        }
    }
    return true;
}

static bool substreams_info(rd_t *r, arc_t *a)
{
    uint64_t t = num(r);
    if (t == 0x0D) {
        for (uint32_t i = 0; i < a->n_folders; i++) a->folders[i].streams = (uint32_t)num(r);
        t = num(r);
    }
    uint64_t total = 0;
    for (uint32_t i = 0; i < a->n_folders; i++) total += a->folders[i].streams;
    if (total > 1000000) return false;
    a->n_sub = (uint32_t)total;
    a->sub_sizes = bump(a, total * 8);
    a->sub_crc = bump(a, total * 4);
    a->sub_has_crc = bump(a, total);
    if (total && (!a->sub_sizes || !a->sub_crc || !a->sub_has_crc)) return false;
    uint32_t k = 0;
    for (uint32_t i = 0; i < a->n_folders; i++) {
        folder_t *f = &a->folders[i];
        if (!f->streams) continue;
        uint64_t sum = 0;
        for (uint32_t s = 0; s + 1 < f->streams; s++) {
            uint64_t v = t == 0x09 ? num(r) : 0;
            a->sub_sizes[k++] = v;
            sum += v;
        }
        a->sub_sizes[k++] = f->unpack - sum;
    }
    if (t == 0x09) t = num(r);
    /* digests: for every stream, except a folder's only one when the folder has its own */
    uint32_t need = 0;
    for (uint32_t i = 0; i < a->n_folders; i++)
        if (!(a->folders[i].streams == 1 && a->folders[i].has_crc)) need += a->folders[i].streams;
    k = 0;
    for (uint32_t i = 0; i < a->n_folders; i++)
        if (a->folders[i].streams == 1 && a->folders[i].has_crc) {
            a->sub_crc[k] = a->folders[i].crc;
            a->sub_has_crc[k++] = 1;
        } else {
            k += a->folders[i].streams;
        }
    while (t != 0 && !r->bad) {
        if (t == 0x0A) {
            uint8_t *def = bump(a, need ? need : 1);
            if (!def || !bits(r, def, need, true)) return false;
            uint32_t d = 0;
            k = 0;
            for (uint32_t i = 0; i < a->n_folders; i++) {
                if (a->folders[i].streams == 1 && a->folders[i].has_crc) { k++; continue; }
                for (uint32_t s = 0; s < a->folders[i].streams; s++, k++, d++)
                    if (def[d]) { a->sub_crc[k] = u32(r); a->sub_has_crc[k] = 1; }
            }
        } else {
            skip(r, num(r));
        }
        t = num(r);
    }
    return !r->bad;
}

static bool streams_info(rd_t *r, arc_t *a)
{
    bool have_sub = false;
    for (;;) {
        uint64_t t = num(r);
        if (r->bad) return false;
        if (t == 0) break;
        if (t == 0x06) { if (!pack_info(r, a)) return false; }
        else if (t == 0x07) { if (!unpack_info(r, a)) return false; }
        else if (t == 0x08) { if (!substreams_info(r, a)) return false; have_sub = true; }
        else return false;
    }
    if (!have_sub) {                                    /* one stream per folder */
        a->n_sub = a->n_folders;
        a->sub_sizes = bump(a, a->n_sub * 8ull);
        a->sub_crc = bump(a, a->n_sub * 4ull);
        a->sub_has_crc = bump(a, a->n_sub);
        if (a->n_sub && (!a->sub_sizes || !a->sub_crc || !a->sub_has_crc)) return false;
        for (uint32_t i = 0; i < a->n_folders; i++) {
            a->sub_sizes[i] = a->folders[i].unpack;
            a->sub_crc[i] = a->folders[i].crc;
            a->sub_has_crc[i] = a->folders[i].has_crc;
        }
    }
    return true;
}

static size_t utf8(char *o, uint32_t u)
{
    if (u < 0x80) { o[0] = (char)u; return 1; }
    if (u < 0x800) { o[0] = (char)(0xC0 | u >> 6); o[1] = (char)(0x80 | (u & 63)); return 2; }
    if (u < 0x10000) {
        o[0] = (char)(0xE0 | u >> 12); o[1] = (char)(0x80 | ((u >> 6) & 63)); o[2] = (char)(0x80 | (u & 63));
        return 3;
    }
    o[0] = (char)(0xF0 | u >> 18); o[1] = (char)(0x80 | ((u >> 12) & 63));
    o[2] = (char)(0x80 | ((u >> 6) & 63)); o[3] = (char)(0x80 | (u & 63));
    return 4;
}

static bool files_info(rd_t *r, arc_t *a)
{
    a->n_files = (uint32_t)num(r);
    if (a->n_files > 200000 || !(a->files = bump(a, a->n_files * (uint64_t)sizeof(file_t)))) return false;
    uint8_t *empty = bump(a, a->n_files ? a->n_files : 1), *empty_file = NULL, *isdir = bump(a, a->n_files ? a->n_files : 1);
    if (!empty || !isdir) return false;
    uint32_t n_empty = 0;
    bool names = false;
    for (;;) {
        uint64_t t = num(r);
        if (r->bad) return false;
        if (t == 0) break;
        uint64_t size = num(r);
        const uint8_t *next = r->p + size;
        if (size > (uint64_t)(r->end - r->p)) return false;
        if (t == 0x0E) {
            bits(r, empty, a->n_files, false);
            n_empty = 0;
            for (uint32_t i = 0; i < a->n_files; i++) n_empty += empty[i];
        } else if (t == 0x0F) {
            if (!(empty_file = bump(a, n_empty ? n_empty : 1))) return false;
            bits(r, empty_file, n_empty, false);
        } else if (t == 0x11) {
            if (byte(r) != 0) return false;                  /* (names kept elsewhere: no) */
            /* the pool takes the rest of the memory for now; trimmed after */
            a->names = (char *)a->bump;
            a->names_max = (uint32_t)(a->mem_end - a->bump);
            a->names_used = 0;
            for (uint32_t i = 0; i < a->n_files; i++) {
                a->files[i].name = a->names_used;
                for (;;) {
                    if (r->p + 2 > next) return false;
                    uint32_t u = r->p[0] | r->p[1] << 8;
                    r->p += 2;
                    if (!u) break;
                    if (u >= 0xD800 && u < 0xDC00 && r->p + 2 <= next) {
                        uint32_t lo = r->p[0] | r->p[1] << 8;
                        if (lo >= 0xDC00 && lo < 0xE000) { u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00); r->p += 2; }
                    }
                    if (a->names_used + 5 >= a->names_max) return false;
                    a->names_used += (uint32_t)utf8(a->names + a->names_used, u);
                }
                a->names[a->names_used++] = '\0';
            }
            a->bump += (a->names_used + 7) & ~7u;
            names = true;
        } else if (t == 0x15) {                               /* attributes: the folder bit */
            uint8_t *def = bump(a, a->n_files ? a->n_files : 1);
            if (!def) return false;
            bits(r, def, a->n_files, true);
            if (byte(r) != 0) { r->p = next; continue; }
            for (uint32_t i = 0; i < a->n_files; i++)
                if (def[i]) isdir[i] = (u32(r) & 0x10) != 0;
        }
        r->p = next;
    }
    if (!names) return false;
    uint32_t e = 0, s = 0;
    for (uint32_t i = 0; i < a->n_files; i++) {
        file_t *f = &a->files[i];
        if (empty[i]) {
            bool efile = empty_file && empty_file[e];
            e++;
            if (isdir[i] || !efile) f->flags |= F_DIR;
        } else {
            if (s >= a->n_sub) return false;
            f->flags |= F_STREAM;
            f->size = a->sub_sizes[s];
            if (a->sub_has_crc[s]) { f->flags |= F_CRC; f->crc = a->sub_crc[s]; }
            s++;
        }
    }
    return !r->bad;
}

/* ---------------------------------------------------------------- BCJ x86 (Bra86.c, public domain) */

static const uint8_t kMaskToAllowed[8] = { 1, 1, 1, 0, 1, 0, 0, 0 };
static const uint8_t kMaskToBit[8] = { 0, 1, 2, 2, 3, 3, 3, 3 };
#define MSB_OK(b) ((b) == 0 || (b) == 0xFF)

static uint32_t x86_decode(uint8_t *data, uint32_t size, uint32_t ip, uint32_t *state)
{
    uint32_t pos = 0, prev_pos = (uint32_t)0 - 1, prev_mask = *state & 7;
    if (size < 5) return 0;
    ip += 5;
    for (;;) {
        uint8_t *p = data + pos, *limit = data + size - 4;
        for (; p < limit; p++) if ((*p & 0xFE) == 0xE8) break;
        pos = (uint32_t)(p - data);
        if (p >= limit) break;
        prev_pos = pos - prev_pos;
        if (prev_pos > 3) {
            prev_mask = 0;
        } else {
            prev_mask = (prev_mask << (prev_pos - 1)) & 7;
            if (prev_mask) {
                uint8_t b = p[4 - kMaskToBit[prev_mask]];
                if (!kMaskToAllowed[prev_mask] || MSB_OK(b)) {
                    prev_pos = pos;
                    prev_mask = ((prev_mask << 1) & 7) | 1;
                    pos++;
                    continue;
                }
            }
        }
        prev_pos = pos;
        if (MSB_OK(p[4])) {
            uint32_t src = (uint32_t)p[4] << 24 | (uint32_t)p[3] << 16 | (uint32_t)p[2] << 8 | p[1], dest;
            for (;;) {
                dest = src - (ip + pos);
                if (!prev_mask) break;
                unsigned idx = kMaskToBit[prev_mask] * 8;
                uint8_t b = (uint8_t)(dest >> (24 - idx));
                if (!MSB_OK(b)) break;
                src = dest ^ ((1u << (32 - idx)) - 1);
            }
            p[4] = (uint8_t)(~(((dest >> 24) & 1) - 1));
            p[3] = (uint8_t)(dest >> 16);
            p[2] = (uint8_t)(dest >> 8);
            p[1] = (uint8_t)dest;
            pos += 5;
        } else {
            prev_mask = ((prev_mask << 1) & 7) | 1;
            pos++;
        }
    }
    prev_pos = pos - prev_pos;
    *state = prev_pos > 3 ? 0 : (prev_mask << (prev_pos - 1)) & 7;
    return pos;
}

/* ---------------------------------------------------------------- unpacking */

typedef struct {
    ex_job_t *j;
    arc_t *a;
    lz_t z;
    folder_t *f;
    uint32_t first;                      /* the folder's first stream file (file index) */
    uint32_t left_files;                 /* its stream files not started yet */
    uint32_t cur;                        /* the file being written (file index), or ~0 */
    uint32_t next;                       /* where to look for the next one */
    uint64_t cur_left;
    FILE *out;
    uint32_t crc;
    uint8_t *pages;                      /* PAGES x PAGE; with BCJ: the filter's buffer */
    uint64_t page_at[PAGES];
    uint32_t page_len[PAGES], page_use[PAGES], tick;
    FILE *rf;                            /* reading back: that file, its index, and its size then */
    uint32_t rf_index;
    uint64_t rf_size;
    uint32_t bcj_state, bcj_ip, bcj_have;
    uint32_t misses;                     /* (statistics: pages read back) */
} unp_t;

static void file_path(unp_t *u, uint32_t i, char *out, size_t size)
{
    if (!ex_target(u->j, u->a->names + u->a->files[i].name, false, out, size)) out[0] = '\0';
    if (u->a->files[i].flags & F_TEMP) {
        size_t n = strlen(out);
        snprintf(out + n, size - n, ".7ztmp~");
    }
}

static uint32_t next_stream_file(arc_t *a, uint32_t from)
{
    while (from < a->n_files && !(a->files[from].flags & F_STREAM)) from++;
    return from;
}

static bool finish_file(unp_t *u)
{
    if (u->cur == ~0u) return true;
    file_t *f = &u->a->files[u->cur];
    bool ok = true;
    if (u->out) {
        ok = fclose(u->out) == 0;
        u->out = NULL;
    }
    if (u->rf && u->rf_index == u->cur) { fclose(u->rf); u->rf = NULL; }
    if (!(f->flags & (F_TEMP | F_FAILED))) {
        if (!ok || ((f->flags & F_CRC) && crc32_final(&u->crc) != f->crc)) {
            u->j->r->bad++;
            ex_problem(u->j, u->a->names + f->name);
        } else {
            u->j->r->files++;
        }
    }
    u->cur = ~0u;
    return true;
}

static bool start_file(unp_t *u)
{
    uint32_t i = next_stream_file(u->a, u->next);
    if (i >= u->a->n_files || !u->left_files) return false;
    u->left_files--;
    u->next = i + 1;
    u->cur = i;
    file_t *f = &u->a->files[i];
    u->cur_left = f->size;
    crc32_init(&u->crc);
    char path[512];
    file_path(u, i, path, sizeof path);
    bool skip = false;
    u->out = path[0] ? ex_create(u->j, path, &skip) : NULL;
    if (u->j->r->status == EX_CANCELLED) return false;
    if (!u->out && skip) {                     /* kept as it is: unpacked aside, for what follows */
        f->flags |= F_TEMP;
        file_path(u, i, path, sizeof path);
        u->out = fopen(path, "w+b");
    }
    if (!u->out) {
        f->flags |= F_FAILED;
        u->j->r->bad++;
        ex_problem(u->j, u->a->names + f->name);
    }
    return true;
}

static bool write_out(unp_t *u, const uint8_t *p, uint32_t n)
{
    while (n) {
        if (u->cur == ~0u || !u->cur_left) {
            finish_file(u);
            if (!start_file(u)) return u->j->r->status != EX_CANCELLED && false;
            if (!u->cur_left) continue;
        }
        uint32_t k = u->cur_left < n ? (uint32_t)u->cur_left : n;
        if (u->out) {
            if (fwrite(p, 1, k, u->out) != k) {
                fclose(u->out);
                u->out = NULL;
                u->a->files[u->cur].flags |= F_FAILED;
                u->j->r->bad++;
                ex_problem(u->j, u->a->names + u->a->files[u->cur].name);
            }
        }
        crc32_update(&u->crc, p, k);
        u->cur_left -= k;
        p += k;
        n -= k;
        if (!ex_progress(u->j, k)) return false;
    }
    return true;
}

static bool put(lz_t *z, const uint8_t *p, uint32_t n)
{
    unp_t *u = z->user;
    if (!u->f->bcj) return write_out(u, p, n);
    /* x86 filter: the bytes it could not decide yet (<= 4) wait for the next piece */
    uint8_t *b = u->pages;
    while (n) {
        uint32_t k = PAGES * PAGE - u->bcj_have;
        if (k > n) k = n;
        memcpy(b + u->bcj_have, p, k);
        u->bcj_have += k;
        p += k;
        n -= k;
        uint32_t done = x86_decode(b, u->bcj_have, u->bcj_ip, &u->bcj_state);
        if (!write_out(u, b, done)) return false;
        u->bcj_ip += done;
        memmove(b, b + done, u->bcj_have - done);
        u->bcj_have -= done;
    }
    return true;
}

/* bytes [at, at+n) of the folder, read back from the files written */
static bool far_fill(unp_t *u, uint64_t at, uint8_t *outp, uint32_t n)
{
    if (u->f->bcj) return false;          /* (the files hold the filtered bytes) */
    uint64_t start = 0;
    uint32_t i = u->first;
    while (n) {
        i = next_stream_file(u->a, i);
        if (i >= u->a->n_files) return false;
        file_t *f = &u->a->files[i];
        if (at >= start + f->size) { start += f->size; i++; continue; }
        if (f->flags & F_FAILED) return false;
        uint64_t off = at - start;
        uint32_t k = f->size - off < n ? (uint32_t)(f->size - off) : n;
        if (i == u->cur) {                    /* the file being written: its own handle ("w+") */
            if (!u->out) return false;
            long end = (long)(f->size - u->cur_left);
            bool ok = fseek(u->out, (long)off, SEEK_SET) == 0 && fread(outp, 1, k, u->out) == k;
            if (fseek(u->out, end, SEEK_SET) != 0 || !ok) return false;
            outp += k;
            at += k;
            n -= k;
            continue;
        }
        if (u->rf && (u->rf_index != i || off + k > u->rf_size)) { fclose(u->rf); u->rf = NULL; }
        if (!u->rf) {
            char path[512];
            file_path(u, i, path, sizeof path);
            if (!(u->rf = fopen(path, "rb"))) return false;
            u->rf_index = i;
            fseek(u->rf, 0, SEEK_END);
            u->rf_size = (uint64_t)ftell(u->rf);
            if (off + k > u->rf_size) return false;
        }
        if (fseek(u->rf, (long)off, SEEK_SET) != 0 || fread(outp, 1, k, u->rf) != k) return false;
        outp += k;
        at += k;
        n -= k;
    }
    return true;
}

static bool far(lz_t *z, uint64_t at, uint8_t *out, uint32_t n)
{
    unp_t *u = z->user;
    while (n) {
        uint64_t pg = at / PAGE * PAGE;
        int hit = -1;
        for (int i = 0; i < PAGES; i++)
            if (u->page_len[i] && u->page_at[i] == pg && at + 1 <= pg + u->page_len[i]) { hit = i; break; }
        if (hit < 0) {
            hit = 0;
            for (int i = 1; i < PAGES; i++) if (u->page_use[i] < u->page_use[hit]) hit = i;
            uint64_t avail = z->flushed - pg;                        /* (only what is on the card) */
            uint32_t len = avail < PAGE ? (uint32_t)avail : PAGE;
            if (at >= pg + len || !far_fill(u, pg, u->pages + hit * PAGE, len)) return false;
            u->misses++;
            u->page_at[hit] = pg;
            u->page_len[hit] = len;
        }
        u->page_use[hit] = ++u->tick;
        uint32_t o = (uint32_t)(at - pg), k = u->page_len[hit] - o;
        if (k > n) k = n;
        memcpy(out, u->pages + hit * PAGE + o, k);
        out += k;
        at += k;
        n -= k;
    }
    return true;
}

static const char *const LZ_WHY[] = { "", "damaged data", "archive cut short", "", "x86 filter: too big", "LZMA settings" };

/* Unpacks folder fi; first = its first file (index in a->files) */
static void unpack_folder(unp_t *u, mv_t *arc, uint32_t fi, uint32_t first)
{
    arc_t *a = u->a;
    folder_t *f = &a->folders[fi];
    u->f = f;
    u->first = first;
    u->left_files = f->streams;
    u->cur = ~0u;
    u->next = first;
    u->out = NULL;
    u->bcj_state = u->bcj_ip = u->bcj_have = 0;
    for (int i = 0; i < PAGES; i++) u->page_len[i] = 0;
    lz_t *z = &u->z;
    z->in = arc;
    z->read = mv_read;
    z->in_left = f->pack_size;
    z->ipos = z->ilen = 0;
    z->in_err = false;
    z->pos = z->flushed = z->dict_start = 0;
    z->wpos = 0;
    z->error = 0;
    bool ok;
    if (f->method == M_NONE || !mv_seek(arc, f->pack_off)) {
        ok = false;
    } else if (f->method == M_COPY) {
        ok = lz_copy(z, f->unpack);
    } else if (f->method == M_LZMA) {
        ok = lzma_decode(z, f->props, f->unpack);
    } else {
        ok = lzma2_decode(z, f->props[0], f->unpack);
    }
    if (ok && f->bcj && u->bcj_have) ok = write_out(u, u->pages, u->bcj_have);   /* the last bytes, as they are */
    finish_file(u);
    if (u->rf) { fclose(u->rf); u->rf = NULL; }
    /* the temporary copies of the files kept as they were */
    uint32_t i = first;
    for (uint32_t n = 0; n < f->streams; n++, i++) {
        i = next_stream_file(a, i);
        if (i >= a->n_files) break;
        if (a->files[i].flags & F_TEMP) {
            char path[512];
            file_path(u, i, path, sizeof path);
            ex_remove(path);
        }
    }
#ifdef UN7Z_DEBUG
    fprintf(stderr, "folder %u streams %u method %d bcj %d ok %d err %d pos %llu/%llu far %llu\n", fi, f->streams, f->method, f->bcj, ok, z->error, (unsigned long long)z->pos, (unsigned long long)f->unpack, (unsigned long long)z->far_reads);
#endif
    if (!ok && u->j->r->status != EX_CANCELLED) {
        if (f->method == M_NONE) {
            u->j->r->unsupported += u->left_files;
        } else {
            u->j->r->bad += u->left_files;
            if (!u->j->r->detail[0] && z->error < 6) snprintf(u->j->r->detail, sizeof u->j->r->detail, "%s", LZ_WHY[z->error]);
        }
        uint32_t k = next_stream_file(a, first);
        if (k < a->n_files) ex_problem(u->j, a->names + a->files[k].name);
        ex_progress(u->j, f->unpack > z->pos ? f->unpack - z->pos : 0);
    }
}

static bool hold(lz_t *z, const uint8_t *p, uint32_t n) { (void)z; (void)p; (void)n; return true; }

void un7z_extract(const char *archive, const char *dest, ex_result_t *r)
{
    ex_job_t j;
    ex_begin(&j, dest, r, 0);
    static unp_t u;
    memset(&u, 0, sizeof u);
    u.j = &j;
    uint8_t *m = ex_mem, *end = ex_mem + ex_mem_size;
    u.z.probs = (uint16_t *)m;
    u.z.probs_max = PROBS_BYTES / 2;
    u.z.ibuf = m + PROBS_BYTES;
    u.z.isize = IN_BYTES;
    u.pages = m + PROBS_BYTES + IN_BYTES;
    u.z.put = put;
    u.z.far = far;
    u.z.user = &u;
    uint8_t *free_mem = u.pages + PAGES * PAGE;
    if (free_mem + MIN_WINDOW > end) { r->status = EX_NO_MEMORY; return; }

    static mv_t mv;
    mv_t *f = &mv;
    if (!mv_open(f, archive)) { r->status = EX_BAD_ARCHIVE; mv_close(f); return; }
    uint8_t sh[32];
    static arc_t a;
    memset(&a, 0, sizeof a);
    if (mv_read(f, sh, 32) != 32 || memcmp(sh, "7z\xBC\xAF\x27\x1C", 6) != 0) { r->status = EX_BAD_ARCHIVE; goto out; }
    uint64_t nh_off = 0, nh_size = 0;
    for (int i = 0; i < 8; i++) { nh_off |= (uint64_t)sh[12 + i] << (8 * i); nh_size |= (uint64_t)sh[20 + i] << (8 * i); }
    /* the header goes at the end of the memory; the file list is built at its start */
    if (nh_size == 0 || nh_size > (uint64_t)(end - free_mem) / 2) { r->status = nh_size ? EX_NO_MEMORY : EX_BAD_ARCHIVE; goto out; }
    uint8_t *hdr = end - nh_size;
    if (!mv_seek(f, 32 + nh_off) || mv_read(f, hdr, nh_size) != nh_size) { r->status = EX_BAD_ARCHIVE; goto out; }
    rd_t rr = { hdr, end, false };
    a.mem = a.bump = free_mem;
    a.mem_end = hdr;
    uint8_t t = byte(&rr);
    if (t == 0x17) {
        /* the real header is packed: unpack it into the window, then read it from there */
        arc_t ea;
        memset(&ea, 0, sizeof ea);
        ea.mem = ea.bump = free_mem;
        ea.mem_end = hdr;
        if (!streams_info(&rr, &ea) || ea.n_folders != 1 || ea.folders[0].method == M_NONE || ea.folders[0].bcj) {
            r->status = EX_NOT_SUPPORTED;
            snprintf(r->detail, sizeof r->detail, "packed header");
            goto out;
        }
        uint64_t size = ea.folders[0].unpack;
        uint8_t *win = ea.bump;
        if (size == 0 || size > (uint64_t)(end - win)) { r->status = EX_NO_MEMORY; goto out; }
        /* window = exactly the header's size, at the very end: it ends up there in one piece */
        hdr = end - size;
        if (hdr < win) { r->status = EX_NO_MEMORY; goto out; }
        u.z.win = hdr;
        u.z.wsize = (uint32_t)size;
        u.f = &ea.folders[0];
        lz_t *z = &u.z;
        z->in = f;
        z->read = mv_read;
        z->in_left = ea.folders[0].pack_size;
        bool ok = mv_seek(f, ea.folders[0].pack_off);
        /* (its window is exactly its size: it stays there in one piece, nothing to write) */
        z->put = hold;
        if (ok) ok = ea.folders[0].method == M_LZMA ? lzma_decode(z, ea.folders[0].props, size) :
                      ea.folders[0].method == M_LZMA2 ? lzma2_decode(z, ea.folders[0].props[0], size) : lz_copy(z, size);
        z->put = put;
        if (!ok) { r->status = EX_BAD_ARCHIVE; snprintf(r->detail, sizeof r->detail, "header"); goto out; }
        rr.p = hdr;
        rr.end = end;
        rr.bad = false;
        a.mem_end = hdr;
        t = byte(&rr);
    }
    if (t != 0x01) { r->status = EX_BAD_ARCHIVE; goto out; }
    for (;;) {
        uint64_t k = num(&rr);
        if (rr.bad) { r->status = EX_BAD_ARCHIVE; goto out; }
        if (k == 0) break;
        if (k == 0x02) {                                        /* archive properties */
            for (;;) { uint64_t id = num(&rr); if (!id || rr.bad) break; skip(&rr, num(&rr)); }
        } else if (k == 0x03) {
            r->status = EX_NOT_SUPPORTED;
            goto out;
        } else if (k == 0x04) {
            if (!streams_info(&rr, &a)) { r->status = a.bump >= a.mem_end ? EX_NO_MEMORY : EX_BAD_ARCHIVE; goto out; }
        } else if (k == 0x05) {
            if (!files_info(&rr, &a)) { r->status = a.bump >= a.mem_end ? EX_NO_MEMORY : EX_BAD_ARCHIVE; goto out; }
        } else {
            r->status = EX_BAD_ARCHIVE;
            goto out;
        }
    }
    /* the window: whatever is left after the file list (the header is not needed any more) */
    u.z.win = a.bump;
    u.z.wsize = (uint32_t)(end - a.bump);
    if (u.z.wsize < MIN_WINDOW) { r->status = EX_NO_MEMORY; snprintf(r->detail, sizeof r->detail, "too many files"); goto out; }
    u.a = &a;
    for (uint32_t i = 0; i < a.n_files; i++) if (a.files[i].flags & F_STREAM) j.total += a.files[i].size;
    /* a folder bigger than the window, packed with a dictionary bigger than it: slow */
    {
        uint64_t big = 0;
        uint32_t dict = 0;
        for (uint32_t i = 0; i < a.n_folders; i++) {
            folder_t *fo = &a.folders[i];
            uint32_t d = fo->method == M_LZMA ? (uint32_t)(fo->props[1] | fo->props[2] << 8 | fo->props[3] << 16 | (uint32_t)fo->props[4] << 24) :
                         fo->method == M_LZMA2 ? (fo->props[0] >= 40 ? 0xFFFFFFFFu : (2u | (fo->props[0] & 1)) << (fo->props[0] / 2 + 11)) : 0;
            if (fo->unpack > u.z.wsize && d > u.z.wsize && fo->unpack > big) { big = fo->unpack; dict = d; }
        }
        if (big && !ex_confirm_slow(big, dict, u.z.wsize)) { r->status = EX_CANCELLED; goto out; }
    }
    if (!ex_mkdirs(dest, true)) { r->status = EX_WRITE_ERROR; goto out; }
    /* folders and empty files first */
    for (uint32_t i = 0; i < a.n_files && r->status == EX_OK; i++) {
        file_t *fl = &a.files[i];
        if (fl->flags & F_STREAM) continue;
        char path[512];
        if (!ex_target(&j, a.names + fl->name, false, path, sizeof path)) continue;
        if (fl->flags & F_DIR) {
            if (ex_mkdirs(path, true)) r->dirs++;
            else { r->bad++; ex_problem(&j, a.names + fl->name); }
        } else {
            bool sk;
            FILE *o = ex_create(&j, path, &sk);
            if (o) { fclose(o); r->files++; }
            else if (!sk && r->status != EX_CANCELLED) { r->bad++; ex_problem(&j, a.names + fl->name); }
        }
    }
    uint32_t next = 0;
    for (uint32_t fi = 0; fi < a.n_folders && r->status == EX_OK; fi++) {
        if (!a.folders[fi].streams) continue;
        next = next_stream_file(&a, next);
        if (next >= a.n_files) break;
        unpack_folder(&u, f, fi, next);
        for (uint32_t n = 0; n < a.folders[fi].streams; n++) next = next_stream_file(&a, next) + 1;
    }
    printf("7z: window %lu KB, %llu bytes from before it, %lu pages read back\n", (unsigned long)(u.z.wsize / 1024), (unsigned long long)u.z.far_reads, (unsigned long)u.misses);
out:
    if (u.out) fclose(u.out);
    if (u.rf) fclose(u.rf);
    mv_close(f);
}

