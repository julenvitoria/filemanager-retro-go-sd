/* File Manager — Ogg Vorbis decoding (ogg.c, stb_vorbis) */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

typedef struct {
    uint32_t rate;
    int channels;
    uint32_t total;                    /* samples per channel, 0 = unknown */
    char title[96], artist[96];
} ogg_info_t;

/* 0 ok, -1 not Vorbis / broken, -2 not supported (more than 2 channels, too big for mem),
 * -3 no audio. mem holds the file buffer and the decoder (~140 KB is plenty). */
int ogg_open(FILE *f, uint32_t file_size, uint8_t *mem, uint32_t mem_size, ogg_info_t *in);
/* next samples as mono 16-bit into out (at most max); 0 at the end */
int ogg_read(int16_t *out, int max);
/* go to a sample (per channel); returns where it landed */
uint32_t ogg_seek(uint32_t sample);
void ogg_close(void);
