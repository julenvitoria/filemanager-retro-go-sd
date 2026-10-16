/* File Manager — small FLAC decoder (flac.c) */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#define FLAC_MAX_BLOCK 8192            /* samples per channel in one frame */

typedef struct {
    uint32_t rate, max_block, max_frame;
    int channels, bps;
    uint64_t total;                    /* samples per channel, 0 = unknown */
    uint32_t first_frame;              /* file offset of the audio */
    char title[96], artist[96];
} flac_info_t;

/* 0 ok, -1 not FLAC / broken, -2 not supported (more than 2 channels, huge blocks), -3 no audio.
 * tmp is used to read the tags. */
int flac_open(FILE *f, uint32_t file_size, flac_info_t *in, uint8_t *tmp, uint32_t tmp_size);
/* decode from byte pos (anywhere: the next frame is searched); ch0/ch1 hold FLAC_MAX_BLOCK */
void flac_start(FILE *f, const flac_info_t *in, uint32_t pos, uint32_t end,
                uint8_t *buf, uint32_t buf_size, int32_t *ch0, int32_t *ch1);
/* next frame as mono 16-bit into out (FLAC_MAX_BLOCK); returns samples, 0 at the end */
int flac_decode(int16_t *out, uint64_t *first_sample);
