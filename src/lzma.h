/* File Manager — LZMA / LZMA2 decoder with a window smaller than the dictionary (lzma.c).
 *
 * The console has ~190 KB to work in and a 7z's dictionary is usually 16-64 MB. The decoder keeps
 * the last `wsize` bytes in RAM; a match that reaches further back reads the bytes again from
 * what was already written to the card, through the `far` callback. Output goes out through
 * `put` in pieces of the window. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

typedef struct lz_s lz_t;
struct lz_s {
    /* input: the packed stream */
    size_t (*read)(void *ctx, uint8_t *buf, size_t n);
    void *in;
    uint64_t in_left;          /* packed bytes not read yet */
    uint8_t *ibuf;
    uint32_t isize, ipos, ilen;
    bool in_err;

    /* range coder */
    uint32_t range, code;

    /* model */
    uint16_t *probs;
    uint32_t probs_max;        /* entries the probs memory has room for */
    unsigned lc, lp, pb;
    uint32_t state, rep[4];

    /* window */
    uint8_t *win;
    uint32_t wsize, wpos;      /* wpos = where pos falls in win */
    uint64_t pos;              /* bytes out so far (the folder's position) */
    uint64_t dict_start;       /* LZMA2 dictionary reset: nothing before this */
    uint64_t flushed;          /* handed to put() up to here */

    /* the application */
    bool (*put)(lz_t *z, const uint8_t *p, uint32_t n);            /* false = stop */
    bool (*far)(lz_t *z, uint64_t at, uint8_t *out, uint32_t n);    /* bytes before the window */
    void *user;
    uint64_t far_reads;        /* (statistics) */
    int error;                 /* 0, or why it stopped */
};
enum { LZ_OK, LZ_DATA, LZ_INPUT, LZ_STOPPED, LZ_FAR, LZ_PROPS };

/* props: the 5 bytes of a 7z LZMA coder; size: bytes to produce */
bool lzma_decode(lz_t *z, const uint8_t *props, uint64_t size);
/* prop: the 1 byte of an LZMA2 coder */
bool lzma2_decode(lz_t *z, uint8_t prop, uint64_t size);
/* copy coder: the packed bytes are the data */
bool lz_copy(lz_t *z, uint64_t size);
bool lz_flush(lz_t *z);        /* the rest of the window out */
uint32_t lz_probs_needed(unsigned lc_plus_lp);
