/*
 * File Manager — what extracting a zip and a 7z have in common: where each entry goes, folders,
 * "already exists", progress. Tools overlay.
 */
#include <string.h>
#include <stdio.h>
#include "extract.h"

void ex_begin(ex_job_t *j, const char *dest, ex_result_t *r, uint64_t total)
{
    memset(j, 0, sizeof *j);
    memset(r, 0, sizeof *r);
    j->dest = dest;
    j->r = r;
    j->total = total;
}

/* CP437 (DOS) 0x80..0xFF -> Unicode, for zip names without the UTF-8 flag */
static const uint16_t cp437[128] = {
    0xC7, 0xFC, 0xE9, 0xE2, 0xE4, 0xE0, 0xE5, 0xE7, 0xEA, 0xEB, 0xE8, 0xEF, 0xEE, 0xEC, 0xC4, 0xC5,
    0xC9, 0xE6, 0xC6, 0xF4, 0xF6, 0xF2, 0xFB, 0xF9, 0xFF, 0xD6, 0xDC, 0xA2, 0xA3, 0xA5, 0x20A7, 0x192,
    0xE1, 0xED, 0xF3, 0xFA, 0xF1, 0xD1, 0xAA, 0xBA, 0xBF, 0x2310, 0xAC, 0xBD, 0xBC, 0xA1, 0xAB, 0xBB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x3B1, 0xDF, 0x393, 0x3C0, 0x3A3, 0x3C3, 0xB5, 0x3C4, 0x3A6, 0x398, 0x3A9, 0x3B4, 0x221E, 0x3C6, 0x3B5, 0x2229,
    0x2261, 0xB1, 0x2265, 0x2264, 0x2320, 0x2321, 0xF7, 0x2248, 0xB0, 0x2219, 0xB7, 0x221A, 0x207F, 0xB2, 0x25A0, 0xA0,
};

static size_t put_utf8(char *o, uint16_t u)
{
    if (u < 0x80) { o[0] = (char)u; return 1; }
    if (u < 0x800) { o[0] = (char)(0xC0 | u >> 6); o[1] = (char)(0x80 | (u & 63)); return 2; }
    o[0] = (char)(0xE0 | u >> 12); o[1] = (char)(0x80 | ((u >> 6) & 63)); o[2] = (char)(0x80 | (u & 63));
    return 3;
}

bool ex_target(ex_job_t *j, const char *name, bool cp437_name, char *out, size_t size)
{
    char rel[512];
    size_t o = 0;
    /* to UTF-8, with "/" for "\" */
    for (const uint8_t *p = (const uint8_t *)name; *p && o + 4 < sizeof rel; p++) {
        uint8_t c = *p;
        if (c == '\\') c = '/';
        if (cp437_name && c >= 0x80) { o += put_utf8(rel + o, cp437[c - 0x80]); continue; }
        rel[o++] = (char)c;
    }
    rel[o] = '\0';
    /* component by component: no "", ".", "..", drive letters; FAT-illegal characters -> "_" */
    size_t n = (size_t)snprintf(out, size, "%s", j->dest);
    if (n >= size) return false;
    if (n && out[n - 1] == '/') n--;
    bool any = false;
    for (char *c = rel, *next; c; c = next) {
        next = strchr(c, '/');
        if (next) *next++ = '\0';
        if (!c[0] || strcmp(c, ".") == 0 || strcmp(c, "..") == 0) continue;
        size_t len = strlen(c);
        if (len == 2 && c[1] == ':') continue;               /* "C:" */
        for (char *q = c; *q; q++)
            if (strchr(":*?\"<>|", *q) || (uint8_t)*q < 0x20) *q = '_';
        while (len && (c[len - 1] == ' ' || c[len - 1] == '.')) c[--len] = '\0';   /* FAT drops them */
        if (!len) continue;
        if (n + 1 + len >= size) return false;
        out[n++] = '/';
        memcpy(out + n, c, len);
        n += len;
        any = true;
    }
    out[n] = '\0';
    return any;
}

bool ex_mkdirs(const char *path, bool last_too)
{
    char p[512];
    snprintf(p, sizeof p, "%s", path);
    size_t n = strlen(p);
    for (size_t i = 1; i <= n; i++) {
        if (p[i] != '/' && p[i] != '\0') continue;
        if (p[i] == '\0' && !last_too) break;
        char keep = p[i];
        p[i] = '\0';
        bool dir;
        if (!ex_path_exists(p, &dir)) {
            if (!ex_mkdir(p)) return false;
        } else if (!dir) {
            return false;                                   /* a file is in the way */
        }
        p[i] = keep;
    }
    return true;
}

FILE *ex_create(ex_job_t *j, const char *path, bool *skip)
{
    *skip = false;
    bool dir;
    if (!ex_mkdirs(path, false)) return NULL;
    if (ex_path_exists(path, &dir)) {
        if (dir) return NULL;
        if (!j->overwrite_all) {
            const char *base = strrchr(path, '/');
            switch (ex_ask_exists(base ? base + 1 : path)) {
            case EXQ_OVERWRITE: break;
            case EXQ_OVERWRITE_ALL: j->overwrite_all = true; break;
            case EXQ_SKIP: *skip = true; j->r->skipped++; return NULL;
            default: j->r->status = EX_CANCELLED; return NULL;
            }
        }
        ex_remove(path);
    }
    return fopen(path, "w+b");          /* (+: 7z reads back what it wrote, lzma.h) */
}

void ex_problem(ex_job_t *j, const char *name)
{
    if (!j->r->first_problem[0]) snprintf(j->r->first_problem, sizeof j->r->first_problem, "%s", name);
}

bool ex_progress(ex_job_t *j, uint64_t add)
{
    j->done += add;
    if (!ex_report(j->done, j->total)) {
        j->r->status = EX_CANCELLED;
        return false;
    }
    return true;
}
