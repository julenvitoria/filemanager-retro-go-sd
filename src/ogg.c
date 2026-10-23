/*
 * File Manager — Ogg Vorbis for the audio player, with stb_vorbis (public domain, the copy in
 * sylverb/pce-go-retro-go-sd). Lives in the media overlay with audio.c.
 *
 * Memory: everything comes from the caller's buffer (fm_scratch): a file buffer first, then
 * stb_vorbis's arena (codebooks, windows, per-channel buffers). Its settings below keep a
 * 44.1 kHz stereo file at quality 9 under ~110 KB.
 *
 * stb_vorbis reads the file a byte at a time (fgetc) and seeks a lot while it looks for pages,
 * so it gets a small buffered reader instead of the firmware's FILE.
 */
#define NDEBUG
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "ogg.h"

/* ---------------------------------------------------------------- buffered reader */

typedef struct {
    FILE *f;
    uint32_t size, pos;                 /* file size, logical position */
    uint32_t bstart, blen;              /* what buf holds */
    uint8_t *buf;
    uint32_t cap;
} oggf_t;

static oggf_t g_of;

static int of_fill(oggf_t *o)
{
    if (o->pos >= o->size) return 0;
    uint32_t n = o->size - o->pos < o->cap ? o->size - o->pos : o->cap;
    if (fseek(o->f, (long)o->pos, SEEK_SET) != 0) return 0;
    size_t got = fread(o->buf, 1, n, o->f);
    o->bstart = o->pos;
    o->blen = (uint32_t)got;
    return got > 0;
}

static inline bool of_has(const oggf_t *o) { return o->pos >= o->bstart && o->pos < o->bstart + o->blen; }

static int of_getc(oggf_t *o)
{
    if (!of_has(o) && !of_fill(o)) return EOF;
    return o->buf[o->pos++ - o->bstart];
}

static size_t of_read(void *dst, size_t size, size_t count, oggf_t *o)
{
    uint8_t *d = dst;
    size_t want = size * count, done = 0;
    while (done < want) {
        if (!of_has(o) && !of_fill(o)) break;
        uint32_t avail = o->bstart + o->blen - o->pos;
        size_t n = want - done < avail ? want - done : avail;
        memcpy(d + done, o->buf + (o->pos - o->bstart), n);
        o->pos += (uint32_t)n;
        done += n;
    }
    return size ? done / size : 0;
}

static int of_seek(oggf_t *o, long off, int whence)
{
    int64_t p = whence == SEEK_END ? (int64_t)o->size + off : whence == SEEK_CUR ? (int64_t)o->pos + off : off;
    if (p < 0) return -1;
    if (p > o->size) { o->pos = o->size; return -1; }
    o->pos = (uint32_t)p;
    return 0;
}

static long of_tell(oggf_t *o) { return (long)o->pos; }

/* ---------------------------------------------------------------- math for stb_vorbis's setup
 * The homebrew links no libm: sin/cos (window and twiddle tables), exp/log (codebook sizes) and
 * ldexp (codebook floats), only called while opening a file, in double precision. */

static const double PI_2 = 1.57079632679489661923;

static double fm_sin_k(double x)                  /* |x| <= pi/4 */
{
    double x2 = x * x, t = x, s = x;
    for (int i = 1; i <= 8; i++) { t *= -x2 / ((2 * i) * (2 * i + 1)); s += t; }
    return s;
}
static double fm_cos_k(double x)
{
    double x2 = x * x, t = 1, s = 1;
    for (int i = 1; i <= 8; i++) { t *= -x2 / ((2 * i - 1) * (2 * i)); s += t; }
    return s;
}
static double fm_sincos(double x, int cos_q)       /* cos(x) = sin(x + pi/2) */
{
    double k = floor(x / PI_2 + 0.5);
    double r = x - k * PI_2;                       /* the arguments here stay small: enough */
    int q = ((int)(int64_t)k + cos_q) & 3;
    switch (q) {
        case 0:  return fm_sin_k(r);
        case 1:  return fm_cos_k(r);
        case 2:  return -fm_sin_k(r);
        default: return -fm_cos_k(r);
    }
}
static double fm_sin(double x) { return fm_sincos(x, 0); }
static double fm_cos(double x) { return fm_sincos(x, 1); }

static double fm_ldexp(double m, int e)
{
    while (e > 60)  { m *= 1152921504606846976.0; e -= 60; }    /* 2^60 */
    while (e < -60) { m /= 1152921504606846976.0; e += 60; }
    return e >= 0 ? m * (double)(1ull << e) : m / (double)(1ull << -e);
}

static double fm_exp(double x)
{
    double k = floor(x / 0.69314718055994530942 + 0.5);
    double r = x - k * 0.69314718055994530942, t = 1, s = 1;   /* |r| <= ln2/2 */
    for (int i = 1; i <= 16; i++) { t *= r / i; s += t; }
    return fm_ldexp(s, (int)k);
}

static double fm_log(double x)
{
    if (x <= 0) return -1e300;
    int e = 0;
    while (x > 1.41421356237309505) { x *= 0.5; e++; }
    while (x < 0.70710678118654752) { x *= 2.0; e--; }
    double z = (x - 1) / (x + 1), z2 = z * z, t = z, s = 0;     /* log x = 2 atanh z */
    for (int i = 1; i <= 41; i += 2) { s += t / i; t *= z2; }
    return 2 * s + e * 0.69314718055994530942;
}

#define sin   fm_sin
#define cos   fm_cos
#define exp   fm_exp
#define log   fm_log
#define ldexp fm_ldexp

/* ---------------------------------------------------------------- stb_vorbis */

#define FILE oggf_t
#define fgetc(o)            of_getc(o)
#define fread(d, s, n, o)   of_read(d, s, n, o)
#define fseek(o, off, w)    of_seek(o, off, w)
#define ftell(o)            of_tell(o)
#define fclose(o)           ((void)(o))
#define fopen(n, m)         ((oggf_t *)0)

#define STB_VORBIS_MAX_CHANNELS          2
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_INTEGER_CONVERSION
#define STB_VORBIS_FAST_HUFFMAN_LENGTH   8        /* 10: +2 KB per codebook */
#define STB_VORBIS_DIVIDES_IN_CODEBOOK            /* no expanded lookup tables */
#include "third_party/stb_vorbis/stb_vorbis.c"

#undef FILE
#undef fgetc
#undef fread
#undef fseek
#undef ftell
#undef fclose
#undef fopen
#undef sin
#undef cos
#undef exp
#undef log
#undef ldexp

/* ---------------------------------------------------------------- player side */

#define IO_SIZE (8 * 1024)

static stb_vorbis *g_v;

static void tag(char *dst, size_t n, const char *c, const char *key)
{
    size_t k = strlen(key);
    if (!dst[0] && strncasecmp(c, key, k) == 0 && c[k] == '=') snprintf(dst, n, "%s", c + k + 1);
}

int ogg_open(FILE *f, uint32_t file_size, uint8_t *mem, uint32_t mem_size, ogg_info_t *in)
{
    memset(in, 0, sizeof *in);
    g_v = NULL;
    if (mem_size < IO_SIZE + 32 * 1024) return -2;
    memset(&g_of, 0, sizeof g_of);
    g_of.f = f;
    g_of.size = file_size;
    g_of.buf = mem;
    g_of.cap = IO_SIZE;

    stb_vorbis_alloc a = { (char *)mem + IO_SIZE, (int)(mem_size - IO_SIZE) };
    int err = 0;
    g_v = stb_vorbis_open_file_section(&g_of, 0, &err, &a, file_size);
    if (!g_v) {
        if (err == VORBIS_outofmem || err == VORBIS_too_many_channels) return -2;
        if (err == VORBIS_feature_not_supported) return -2;
        if (err == VORBIS_unexpected_eof && file_size < 16 * 1024) return -3;
        return -1;
    }
    stb_vorbis_info vi = stb_vorbis_get_info(g_v);
    in->rate = vi.sample_rate;
    in->channels = vi.channels;
    if (in->rate < 4000 || in->rate > 192000 || in->channels < 1 || in->channels > 2) { g_v = NULL; return -2; }
    stb_vorbis_comment c = stb_vorbis_get_comment(g_v);
    for (int i = 0; i < c.comment_list_length; i++) {
        tag(in->title, sizeof in->title, c.comment_list[i], "TITLE");
        tag(in->artist, sizeof in->artist, c.comment_list[i], "ARTIST");
    }
    unsigned int total = stb_vorbis_stream_length_in_samples(g_v);
    in->total = total == SAMPLE_unknown ? 0 : total;
    if (stb_vorbis_seek_start(g_v) == 0) { g_v = NULL; return -1; }
    g_v->channel_buffer_start = g_v->channel_buffer_end = 0;
    return 0;
}

/* straight from stb_vorbis's channel buffers: what is left of a frame after a seek too */
int ogg_read(int16_t *out, int max)
{
    if (!g_v) return 0;
    for (int tries = 0; g_v->channel_buffer_start >= g_v->channel_buffer_end; tries++)   /* a frame can be empty */
        if (tries == 8 || !stb_vorbis_get_frame_float(g_v, NULL, NULL)) return 0;
    int n = g_v->channel_buffer_end - g_v->channel_buffer_start;
    if (n > max) n = max;
    const float *l = g_v->channel_buffers[0] + g_v->channel_buffer_start;
    const float *r = g_v->channel_buffers[g_v->channels > 1 ? 1 : 0] + g_v->channel_buffer_start;
    for (int i = 0; i < n; i++) {
        float v = (l[i] + r[i]) * (32767.0f * 0.5f);
        out[i] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (int)v);
    }
    g_v->channel_buffer_start += n;
    return n;
}

uint32_t ogg_seek(uint32_t sample)
{
    if (!g_v) return 0;
    if (!stb_vorbis_seek(g_v, sample)) {         /* past the end or a broken page: from the start */
        stb_vorbis_seek_start(g_v);
        g_v->channel_buffer_start = g_v->channel_buffer_end = 0;
        return 0;
    }
    return sample;
}

void ogg_close(void)
{
    if (g_v) stb_vorbis_close(g_v);              /* everything is in the caller's buffer */
    g_v = NULL;
}
