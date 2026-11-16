/*
 * File Manager — .zip extraction (tools overlay): stored and deflated entries (zip64 too), the
 * CRC of each one checked. The central directory at the end of the file says what is inside;
 * a second handle reads each entry's data while the first walks the directory.
 *
 * Work memory (ex_mem): 32 KB inflate window | the inflater's state | 16 KB input.
 */
#include <string.h>
#include <stdio.h>
#include "extract.h"
#include "hash.h"
#include "tinfl_tools.h"

#define IN_SIZE (16 * 1024)

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }

static bool seek64(FILE *f, uint64_t pos) { return pos <= 0x7FFFFFFF && fseek(f, (long)pos, SEEK_SET) == 0; }

typedef struct {
    uint16_t flags, method;
    uint32_t crc;
    uint64_t csize, usize, local;
    char name[512];
    bool utf8;
} zent_t;

/* The next central-directory entry from f */
static bool read_entry(FILE *f, zent_t *e)
{
    uint8_t h[46];
    if (fread(h, 1, 46, f) != 46 || rd32(h) != 0x02014b50) return false;
    e->flags = rd16(h + 8);
    e->method = rd16(h + 10);
    e->crc = rd32(h + 16);
    e->csize = rd32(h + 20);
    e->usize = rd32(h + 24);
    e->local = rd32(h + 42);
    e->utf8 = (e->flags & 0x800) != 0;
    uint16_t nlen = rd16(h + 28), xlen = rd16(h + 30), clen = rd16(h + 32);
    size_t keep = nlen < sizeof e->name ? nlen : sizeof e->name - 1;
    if (fread(e->name, 1, keep, f) != keep) return false;
    e->name[keep] = '\0';
    if (nlen > keep && fseek(f, nlen - (long)keep, SEEK_CUR) != 0) return false;
    /* zip64: the sizes and offset that did not fit are in extra field 0x0001, in this order */
    uint8_t x[256];
    while (xlen >= 4) {
        uint8_t xh[4];
        if (fread(xh, 1, 4, f) != 4) return false;
        uint16_t id = rd16(xh), sz = rd16(xh + 2);
        xlen -= 4;
        if (sz > xlen) return false;
        if (id == 0x0001 && sz <= sizeof x) {
            if (fread(x, 1, sz, f) != sz) return false;
            const uint8_t *q = x, *end = x + sz;
            if (e->usize == 0xFFFFFFFFu && q + 8 <= end) { e->usize = rd64(q); q += 8; }
            if (e->csize == 0xFFFFFFFFu && q + 8 <= end) { e->csize = rd64(q); q += 8; }
            if (e->local == 0xFFFFFFFFu && q + 8 <= end) { e->local = rd64(q); q += 8; }
        } else if (fseek(f, sz, SEEK_CUR) != 0) {
            return false;
        }
        xlen -= sz;
    }
    if (xlen && fseek(f, xlen, SEEK_CUR) != 0) return false;
    return clen == 0 || fseek(f, clen, SEEK_CUR) == 0;
}

/* Where the central directory is, and how many entries */
static bool find_directory(FILE *f, uint64_t *cd_off, uint64_t *count, ex_result_t *r)
{
    if (fseek(f, 0, SEEK_END) != 0) return false;
    long size = ftell(f);
    if (size < 22) return false;
    long tail = size < 65557 ? size : 65557;
    uint8_t *buf = ex_mem;
    if (fseek(f, size - tail, SEEK_SET) != 0 || fread(buf, 1, (size_t)tail, f) != (size_t)tail) return false;
    long i;
    for (i = tail - 22; i >= 0; i--)
        if (rd32(buf + i) == 0x06054b50) break;
    if (i < 0) return false;
    *count = rd16(buf + i + 10);
    *cd_off = rd32(buf + i + 16);
    if (rd16(buf + i + 4) != 0 || rd16(buf + i + 6) != 0) {             /* split over several disks */
        snprintf(r->detail, sizeof r->detail, "multi-part zip");
        r->status = EX_NOT_SUPPORTED;
        return false;
    }
    if (*count == 0xFFFF || *cd_off == 0xFFFFFFFFu) {                    /* zip64 */
        if (i < 20 || rd32(buf + i - 20) != 0x07064b50) return false;
        uint64_t z64 = rd64(buf + i - 20 + 8);
        uint8_t h[56];
        if (!seek64(f, z64) || fread(h, 1, 56, f) != 56 || rd32(h) != 0x06064b50) return false;
        *count = rd64(h + 32);
        *cd_off = rd64(h + 48);
    }
    return true;
}

static void extract_entry(ex_job_t *j, FILE *data, zent_t *e)
{
    char path[512];
    if (!ex_target(j, e->name, !e->utf8, path, sizeof path)) return;
    size_t n = strlen(e->name);
    if (n && (e->name[n - 1] == '/' || e->name[n - 1] == '\\')) {             /* a folder */
        if (ex_mkdirs(path, true)) j->r->dirs++;
        else { j->r->bad++; ex_problem(j, e->name); }
        return;
    }
    if ((e->flags & 1) || (e->method != 0 && e->method != 8)) {             /* encrypted / other method */
        j->r->unsupported++;
        ex_problem(j, e->name);
        ex_progress(j, e->usize);
        return;
    }
    uint8_t lh[30];
    if (!seek64(data, e->local) || fread(lh, 1, 30, data) != 30 || rd32(lh) != 0x04034b50 ||
        fseek(data, rd16(lh + 26) + rd16(lh + 28), SEEK_CUR) != 0) {
        j->r->bad++;
        ex_problem(j, e->name);
        return;
    }
    bool skip;
    FILE *out = ex_create(j, path, &skip);
    if (!out) {
        if (j->r->status == EX_CANCELLED) return;
        if (!skip) { j->r->bad++; ex_problem(j, e->name); }
        ex_progress(j, e->usize);
        return;
    }

    uint8_t *dict = ex_mem, *in = ex_mem + TINFL_LZ_DICT_SIZE + sizeof(tinfl_decompressor);
    tinfl_decompressor *d = (tinfl_decompressor *)(ex_mem + TINFL_LZ_DICT_SIZE);
    uint32_t crc;
    crc32_init(&crc);
    uint64_t left = e->csize, written = 0;
    bool ok = true;
    if (e->method == 0) {
        while (ok && left) {
            size_t want = left < IN_SIZE ? (size_t)left : IN_SIZE;
            if (fread(in, 1, want, data) != want) { ok = false; break; }
            left -= want;
            crc32_update(&crc, in, want);
            if (fwrite(in, 1, want, out) != want) { ok = false; break; }
            written += want;
            if (!ex_progress(j, want)) { ok = false; break; }
        }
    } else {
        tinfl_init(d);
        size_t avail = 0, dict_ofs = 0;
        const uint8_t *p = in;
        for (;;) {
            if (!avail && left) {
                size_t want = left < IN_SIZE ? (size_t)left : IN_SIZE;
                if (fread(in, 1, want, data) != want) { ok = false; break; }
                left -= want;
                p = in;
                avail = want;
            }
            size_t in_n = avail, out_n = TINFL_LZ_DICT_SIZE - dict_ofs;
            tinfl_status st = tinfl_decompress(d, p, &in_n, dict, dict + dict_ofs, &out_n,
                                               left ? TINFL_FLAG_HAS_MORE_INPUT : 0);
            p += in_n;
            avail -= in_n;
            if (out_n) {
                crc32_update(&crc, dict + dict_ofs, out_n);
                if (fwrite(dict + dict_ofs, 1, out_n, out) != out_n) { ok = false; break; }
                written += out_n;
                if (!ex_progress(j, out_n)) { ok = false; break; }
            }
            dict_ofs = (dict_ofs + out_n) & (TINFL_LZ_DICT_SIZE - 1);
            if (st == TINFL_STATUS_DONE) break;
            if (st < 0 || (st == TINFL_STATUS_NEEDS_MORE_INPUT && !left && !avail)) { ok = false; break; }
        }
    }
    bool good = fclose(out) == 0 && ok;
    if (j->r->status == EX_CANCELLED) {
        ex_remove(path);                         /* not half a file */
        return;
    }
    if (!good || written != e->usize || crc32_final(&crc) != e->crc) {
        j->r->bad++;
        ex_problem(j, e->name);
        if (written < e->usize) ex_progress(j, e->usize - written);
        return;
    }
    j->r->files++;
}

void unzip_extract(const char *archive, const char *dest, ex_result_t *r)
{
    ex_job_t j;
    ex_begin(&j, dest, r, 0);
    if (ex_mem_size < TINFL_LZ_DICT_SIZE + sizeof(tinfl_decompressor) + IN_SIZE) { r->status = EX_NO_MEMORY; return; }
    FILE *dir = fopen(archive, "rb");
    FILE *data = dir ? fopen(archive, "rb") : NULL;
    uint64_t cd_off = 0, count = 0;
    if (!data || !find_directory(dir, &cd_off, &count, r)) {
        if (!r->status) r->status = EX_BAD_ARCHIVE;
        goto out;
    }
    /* first pass: the total size, for the progress bar */
    zent_t e;
    if (!seek64(dir, cd_off)) { r->status = EX_BAD_ARCHIVE; goto out; }
    for (uint64_t i = 0; i < count; i++) {
        if (!read_entry(dir, &e)) { r->status = EX_BAD_ARCHIVE; goto out; }
        j.total += e.usize;
    }
    if (!ex_mkdirs(dest, true)) { r->status = EX_WRITE_ERROR; goto out; }
    seek64(dir, cd_off);
    for (uint64_t i = 0; i < count && r->status == EX_OK; i++) {
        if (!read_entry(dir, &e)) { r->status = EX_BAD_ARCHIVE; break; }
        extract_entry(&j, data, &e);
    }
out:
    if (data) fclose(data);
    if (dir) fclose(dir);
}
