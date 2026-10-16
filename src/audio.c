/*
 * File Manager — audio player: WAV (PCM 8/16/24/32-bit, float, A-law, µ-law), MP3
 * (also MPEG layer 1/2) decoded with minimp3 (CC0), and FLAC (flac.c).
 *
 * The sound goes to the firmware's PCM ring (pcm_attach), mono, at the file's rate when the
 * SAI supports it (linear resampling otherwise). Controls as in the video player:
 * A pause, Left/Right -/+10 s, Down/Up -/+60 s, TIME next file, GAME previous file, B stop.
 *
 * Memory: everything lives in fm_scratch (ring, decoder, scratch, buffers).
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "fm.h"
#include "flac.h"

extern void wdog_refresh(void);
extern uint32_t HAL_GetTick(void);

void     pcm_attach(int16_t *ring, int size, volatile uint16_t *head, volatile uint16_t *tail);
void     pcm_audio_enable(int on);
void     pcm_audio_set(int vol, int play);
void     odroid_audio_init(int sample_rate);
void     audio_start_playing(uint16_t length);
void     audio_stop_playing(void);
uint8_t  common_emu_sound_get_volume(void);
void     lcd_swap(void);
void     lcd_sleep_while_swap_pending(void);
void    *lcd_get_active_buffer(void);

/* ---------------------------------------------------------------- memory map (fm_scratch) */
#define RING_SAMPLES 16384                               /* power of two */
#define OFF_RING     0                                   /* 32 KB */
#define OFF_DEC      (32 * 1024)                         /* mp3dec_t (~6.7 KB) */
#define OFF_SCR      (40 * 1024)                         /* mp3dec_scratch_t (~16.3 KB) */
#define OFF_IN       (60 * 1024)                         /* file input */
#define IN_SIZE      (32 * 1024)
#define OFF_PCM      (92 * 1024)                         /* decoded frame (int16) */
#define PCM_BYTES    (16 * 1024)
#define OFF_END      (OFF_PCM + PCM_BYTES)
/* FLAC: its input window over the MP3 decoder's area, the channels after the rest */
#define OFF_FIN      OFF_DEC
#define FIN_SIZE     (OFF_PCM - OFF_DEC)                 /* 60 KB */
#define OFF_FCH      OFF_END                             /* 2 x 32 KB */
#define FCH_BYTES    (FLAC_MAX_BLOCK * 4)

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#define MINIMP3_SCRATCH_PTR (fm_scratch + OFF_SCR)
#include "third_party/minimp3/minimp3.h"

_Static_assert(sizeof(mp3dec_t) <= OFF_SCR - OFF_DEC, "mp3dec_t does not fit");
_Static_assert(sizeof(mp3dec_scratch_t) <= OFF_IN - OFF_SCR, "minimp3 scratch does not fit");
_Static_assert(MINIMP3_MAX_SAMPLES_PER_FRAME * 2 <= PCM_BYTES, "pcm buffer");
_Static_assert(OFF_END <= FM_SCRATCH_SIZE, "fm_scratch too small");
_Static_assert(OFF_FCH + 2 * FCH_BYTES <= FM_SCRATCH_SIZE, "fm_scratch too small for FLAC");
_Static_assert(FLAC_MAX_BLOCK * 2 <= PCM_BYTES, "pcm buffer (FLAC)");

static int16_t *const g_ring = (int16_t *)(fm_scratch + OFF_RING);
static uint8_t *const g_in = fm_scratch + OFF_IN;
static int16_t *const g_pcm = (int16_t *)(fm_scratch + OFF_PCM);
#define g_dec ((mp3dec_t *)(fm_scratch + OFF_DEC))

/* ---------------------------------------------------------------- file types */

static bool ext_is(const char *name, const char *const *list)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    for (; *list; list++) if (strcasecmp(dot + 1, *list) == 0) return true;
    return false;
}
static const char *const PLAYABLE[] = { "wav", "wave", "mp3", "mp2", "mpa", "flac", "fla", NULL };
static const char *const OTHER[]    = { "ogg", "oga", "opus", "m4a", "aac", "wma", "aif", "aiff",
                                        "ape", "wv", "mid", "midi", "mod", "xm", "s3m", "it", "amr", "ac3", NULL };

bool is_audio_ext(const char *name)
{
    if (is_macos_meta(name)) return false;
    return ext_is(name, PLAYABLE) || ext_is(name, OTHER);
}

/* ---------------------------------------------------------------- source */

enum { SRC_WAV, SRC_MP3, SRC_FLAC };
enum { W_PCM, W_FLOAT, W_ALAW, W_ULAW };

typedef struct {
    FILE *f;
    int kind;
    uint32_t data_start, data_end;           /* audio bytes (no tags) */
    uint32_t rate;                           /* 0 until known (MP3: first frame) */
    int channels;
    uint32_t total_ms;                       /* 0 = unknown */
    /* WAV */
    int wfmt, bits;
    uint32_t align;
    /* MP3 */
    uint32_t avg_bps;                        /* bytes per second (seeking without a TOC) */
    uint8_t toc[100]; bool has_toc;
    uint32_t in_len, in_pos;                 /* g_in window */
    bool eof;
    /* FLAC */
    flac_info_t fi;
    bool resync;                             /* after a seek: take the time from the next frame */
    /* tags */
    char title[96], artist[96];
} src_t;

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint32_t syncsafe(const uint8_t *p) { return (p[0] & 0x7F) << 21 | (p[1] & 0x7F) << 14 | (p[2] & 0x7F) << 7 | (p[3] & 0x7F); }

static uint32_t file_size(FILE *f)
{
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    return n > 0 ? (uint32_t)n : 0;
}

static bool rd_at(FILE *f, uint32_t pos, void *buf, uint32_t n)
{
    return fseek(f, (long)pos, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
}

/* ---- WAV */

static int wav_open(src_t *s, uint32_t size)
{
    uint8_t h[40];
    if (!rd_at(s->f, 0, h, 12) || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return -1;
    uint32_t pos = 12;
    bool have_fmt = false;
    int tag = 0;
    while (pos + 8 <= size) {
        if (!rd_at(s->f, pos, h, 8)) return -1;
        uint32_t len = le32(h + 4);
        if (!memcmp(h, "fmt ", 4)) {
            uint32_t n = len < 40 ? len : 40;
            if (n < 16 || !rd_at(s->f, pos + 8, h, n)) return -1;
            tag = le16(h);
            s->channels = le16(h + 2);
            s->rate = le32(h + 4);
            s->align = le16(h + 12);
            s->bits = le16(h + 14);
            if (tag == 0xFFFE && n >= 26) tag = le16(h + 24);      /* WAVE_FORMAT_EXTENSIBLE */
            have_fmt = true;
        } else if (!memcmp(h, "data", 4)) {
            if (!have_fmt) return -1;
            s->data_start = pos + 8;
            s->data_end = (len == 0 || len == 0xFFFFFFFFu || s->data_start + len > size) ? size : s->data_start + len;
            break;
        }
        pos += 8 + len + (len & 1);
    }
    if (!have_fmt) return -1;
    if (!s->data_start || s->data_end <= s->data_start) return -3;
    if (s->channels < 1 || s->channels > 8 || s->rate < 4000 || s->rate > 192000 || !s->align) return -2;
    if (tag == 1 && (s->bits == 8 || s->bits == 16 || s->bits == 24 || s->bits == 32)) s->wfmt = W_PCM;
    else if (tag == 3 && s->bits == 32) s->wfmt = W_FLOAT;
    else if (tag == 6 && s->bits == 8) s->wfmt = W_ALAW;
    else if (tag == 7 && s->bits == 8) s->wfmt = W_ULAW;
    else return -2;
    if (s->align != (uint32_t)(s->channels * s->bits / 8)) return -2;
    s->kind = SRC_WAV;
    s->total_ms = (uint32_t)((uint64_t)(s->data_end - s->data_start) / s->align * 1000 / s->rate);
    return 0;
}

static int16_t alaw(uint8_t v)
{
    v ^= 0x55;
    int t = (v & 0x0F) << 4, seg = (v & 0x70) >> 4;
    t = seg == 0 ? t + 8 : seg == 1 ? t + 0x108 : (t + 0x108) << (seg - 1);
    return (int16_t)((v & 0x80) ? t : -t);
}
static int16_t ulaw(uint8_t v)
{
    v = (uint8_t)~v;
    int t = (((v & 0x0F) << 3) + 0x84) << ((v & 0x70) >> 4);
    return (int16_t)((v & 0x80) ? 0x84 - t : t - 0x84);
}

static int32_t wav_sample(const src_t *s, const uint8_t *q)
{
    switch (s->wfmt) {
        case W_FLOAT: {
            float f; memcpy(&f, q, 4);
            if (f != f) return 0;
            if (f > 1.0f) f = 1.0f; else if (f < -1.0f) f = -1.0f;
            return (int32_t)(f * 32767.0f);
        }
        case W_ALAW: return alaw(q[0]);
        case W_ULAW: return ulaw(q[0]);
        default:
            switch (s->bits) {
                case 8:  return ((int)q[0] - 128) * 256;
                case 16: return (int16_t)le16(q);
                case 24: return (int16_t)(q[1] | q[2] << 8);
                default: return (int16_t)(q[2] | q[3] << 8);
            }
    }
}

/* next block of mono samples into g_pcm; 0 at the end */
static int wav_read(src_t *s, uint32_t *pos)
{
    if (*pos >= s->data_end) return 0;
    uint32_t frames = PCM_BYTES / 2;
    if (frames * s->align > IN_SIZE) frames = IN_SIZE / s->align;
    uint32_t left = (s->data_end - *pos) / s->align;
    if (frames > left) frames = left;
    if (frames > 2048) frames = 2048;                       /* small steps keep the keys snappy */
    if (!frames || !rd_at(s->f, *pos, g_in, frames * s->align)) return 0;
    *pos += frames * s->align;
    int ch = s->channels, bps = s->bits / 8;
    for (uint32_t i = 0; i < frames; i++) {
        int32_t acc = 0;
        for (int c = 0; c < ch; c++) acc += wav_sample(s, g_in + i * s->align + (uint32_t)c * bps);
        g_pcm[i] = (int16_t)(acc / ch);
    }
    return (int)frames;
}

/* ---- tags */

/* ID3 text (encoding byte first) -> UTF-8 */
static void id3_text(char *out, size_t size, const uint8_t *p, uint32_t n)
{
    size_t o = 0;
    if (!n) { out[0] = 0; return; }
    int enc = p[0]; p++; n--;
    bool be = enc == 2;
    if (enc == 1 && n >= 2) {
        if (p[0] == 0xFE && p[1] == 0xFF) be = true;
        p += 2; n -= 2;
    }
    for (uint32_t i = 0; i < n && o + 4 < size;) {
        uint32_t c;
        if (enc == 1 || enc == 2) {
            if (i + 1 >= n) break;
            c = be ? (uint32_t)(p[i] << 8 | p[i + 1]) : (uint32_t)(p[i] | p[i + 1] << 8);
            i += 2;
        } else c = p[i++];
        if (!c) break;
        if (enc == 3 || c < 0x80) out[o++] = (char)c;      /* UTF-8 bytes pass through */
        else if (c < 0x800) { out[o++] = (char)(0xC0 | c >> 6); out[o++] = (char)(0x80 | (c & 0x3F)); }
        else { out[o++] = (char)(0xE0 | c >> 12); out[o++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[o++] = (char)(0x80 | (c & 0x3F)); }
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
}

/* ID3v2 at the start: returns its total size (0 = none) and takes title/artist */
static uint32_t id3v2(src_t *s)
{
    uint8_t h[10];
    if (!rd_at(s->f, 0, h, 10) || memcmp(h, "ID3", 3) || h[3] < 2 || h[3] > 4) return 0;
    uint32_t total = 10 + syncsafe(h + 6) + ((h[5] & 0x10) ? 10 : 0);
    int ver = h[3];
    uint32_t n = total - 10 < IN_SIZE ? total - 10 : IN_SIZE;
    if (!rd_at(s->f, 10, g_in, n)) return total;
    uint32_t i = 0;
    if (ver >= 3 && (h[5] & 0x40) && n >= 4) i += ver == 4 ? syncsafe(g_in) : be32(g_in) + 4;   /* extended header */
    int hl = ver == 2 ? 6 : 10;
    while (i + hl <= n && g_in[i]) {
        const uint8_t *f = g_in + i;
        uint32_t len = ver == 2 ? (uint32_t)(f[3] << 16 | f[4] << 8 | f[5]) : ver == 4 ? syncsafe(f + 4) : be32(f + 4);
        if (i + hl + len > n) break;
        if (!memcmp(f, ver == 2 ? "TT2" : "TIT2", ver == 2 ? 3 : 4)) id3_text(s->title, sizeof s->title, f + hl, len);
        if (!memcmp(f, ver == 2 ? "TP1" : "TPE1", ver == 2 ? 3 : 4)) id3_text(s->artist, sizeof s->artist, f + hl, len);
        i += hl + len;
    }
    return total;
}

static void latin1(char *out, size_t size, const uint8_t *p, int n)
{
    uint8_t t[32];
    t[0] = 0;
    memcpy(t + 1, p, (size_t)n);
    id3_text(out, size, t, (uint32_t)n + 1);
}

/* ---- MP3 */

static const uint16_t BR[2][3][15] = {   /* [mpeg1?][layer-1][index] kbps */
    { { 0,32,48,56,64,80,96,112,128,144,160,176,192,224,256 }, { 0,8,16,24,32,40,48,56,64,80,96,112,128,144,160 },
      { 0,8,16,24,32,40,48,56,64,80,96,112,128,144,160 } },
    { { 0,32,64,96,128,160,192,224,256,288,320,352,384,416,448 }, { 0,32,48,56,64,80,96,112,128,160,192,224,256,320,384 },
      { 0,32,40,48,56,64,80,96,112,128,160,192,224,256,320 } } };
static const uint16_t SR[3] = { 44100, 48000, 32000 };

typedef struct { int ver, layer, kbps, hz, ch, bytes, samples; } mhdr_t;   /* ver: 1, 2, 25 */

static bool mp3_hdr(const uint8_t *h, mhdr_t *m)
{
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return false;
    int v = (h[1] >> 3) & 3, l = (h[1] >> 1) & 3, b = h[2] >> 4, r = (h[2] >> 2) & 3;
    if (v == 1 || l == 0 || b == 0 || b == 15 || r == 3) return false;
    m->ver = v == 3 ? 1 : v == 2 ? 2 : 25;
    m->layer = 4 - l;
    m->kbps = BR[m->ver == 1][m->layer - 1][b];
    m->hz = SR[r] >> (m->ver == 1 ? 0 : m->ver == 2 ? 1 : 2);
    m->ch = ((h[3] >> 6) == 3) ? 1 : 2;
    int pad = (h[2] >> 1) & 1;
    if (m->layer == 1) { m->bytes = (12000 * m->kbps / m->hz + pad) * 4; m->samples = 384; }
    else {
        int k = (m->layer == 3 && m->ver != 1) ? 72 : 144;
        m->bytes = k * 1000 * m->kbps / m->hz + pad;
        m->samples = (m->layer == 3 && m->ver != 1) ? 576 : 1152;
    }
    return m->bytes > 4;
}

/* first frame after the tags: rate, length (Xing/Info/VBRI or the bitrate) */
static int mp3_open(src_t *s, uint32_t size)
{
    s->data_start = id3v2(s);
    s->data_end = size;
    uint8_t t[128];
    if (size >= 128 && rd_at(s->f, size - 128, t, 128) && !memcmp(t, "TAG", 3)) {
        s->data_end = size - 128;
        if (!s->title[0]) latin1(s->title, sizeof s->title, t + 3, 30);
        if (!s->artist[0]) latin1(s->artist, sizeof s->artist, t + 33, 30);
    }
    if (s->data_end <= s->data_start + 512) return -3;      /* only tags (and cover art) */
    /* find two consecutive frame headers */
    uint32_t n = s->data_end - s->data_start < IN_SIZE ? s->data_end - s->data_start : IN_SIZE;
    if (!rd_at(s->f, s->data_start, g_in, n)) return -1;
    mhdr_t m, m2;
    uint32_t i;
    for (i = 0; i + 4 <= n; i++) {
        if (!mp3_hdr(g_in + i, &m)) continue;
        if (i + m.bytes + 4 > n) { if (i + m.bytes >= n) break; }
        else if (!mp3_hdr(g_in + i + m.bytes, &m2) || m2.hz != m.hz || m2.layer != m.layer) continue;
        break;
    }
    if (i + 4 > n || !mp3_hdr(g_in + i, &m)) {
        /* no MP3 frames: an error page saved by a failed download, or another format renamed */
        uint32_t j = 0;
        while (j < n && (g_in[j] == ' ' || g_in[j] == '\r' || g_in[j] == '\n' || g_in[j] == '\t' || g_in[j] == 0)) j++;
        if (j == n || g_in[j] == '<' || s->data_end - s->data_start < 16 * 1024) return -3;
        return -1;
    }
    s->data_start += i;
    s->kind = SRC_MP3;
    s->channels = m.ch;
    s->rate = (uint32_t)m.hz;
    uint32_t bytes = s->data_end - s->data_start;
    s->avg_bps = (uint32_t)m.kbps * 125;
    /* Xing / Info */
    const uint8_t *f = g_in + i;
    int side = m.layer != 3 ? 0 : m.ver == 1 ? (m.ch == 1 ? 17 : 32) : (m.ch == 1 ? 9 : 17);
    uint32_t frames = 0;
    if (m.layer == 3 && i + 4 + side + 120 <= n && (!memcmp(f + 4 + side, "Xing", 4) || !memcmp(f + 4 + side, "Info", 4))) {
        const uint8_t *x = f + 4 + side + 4;
        uint32_t flags = be32(x), o = 4;
        if (flags & 1) { frames = be32(x + o); o += 4; }
        if (flags & 2) { uint32_t b = be32(x + o); o += 4; if (b && b <= bytes) bytes = b; }
        if (flags & 4) { memcpy(s->toc, x + o, 100); s->has_toc = true; }
    } else if (i + 4 + 32 + 18 <= n && !memcmp(f + 4 + 32, "VBRI", 4)) {
        const uint8_t *x = f + 4 + 32;
        bytes = be32(x + 10) ? be32(x + 10) : bytes;
        frames = be32(x + 14);
    }
    if (frames) {
        uint64_t ms = (uint64_t)frames * (uint32_t)m.samples * 1000 / (uint32_t)m.hz;
        s->total_ms = (uint32_t)ms;
        if (ms) s->avg_bps = (uint32_t)((uint64_t)bytes * 1000 / ms);
    } else if (s->avg_bps) s->total_ms = (uint32_t)((uint64_t)bytes * 1000 / s->avg_bps);
    return 0;
}

static void mp3_restart(src_t *s, uint32_t pos)
{
    mp3dec_init(g_dec);
    fseek(s->f, (long)pos, SEEK_SET);
    s->in_len = s->in_pos = 0;
    s->eof = false;
}

/* next decoded frame (mono) into g_pcm; 0 at the end */
static int mp3_read(src_t *s, uint32_t *pos)
{
    for (int tries = 0; tries < 64; tries++) {
        if (s->in_len - s->in_pos < 16 * 1024 && !s->eof) {
            uint32_t keep = s->in_len - s->in_pos;
            memmove(g_in, g_in + s->in_pos, keep);
            uint32_t want = IN_SIZE - keep;
            uint32_t at = (uint32_t)ftell(s->f);
            if (at + want > s->data_end) want = at < s->data_end ? s->data_end - at : 0;
            size_t got = want ? fread(g_in + keep, 1, want, s->f) : 0;
            if (got < want || !want) s->eof = true;
            s->in_len = keep + (uint32_t)got;
            s->in_pos = 0;
        }
        if (s->in_pos >= s->in_len) return 0;
        mp3dec_frame_info_t info;
        int n = mp3dec_decode_frame(g_dec, g_in + s->in_pos, (int)(s->in_len - s->in_pos), g_pcm, &info);
        if (!info.frame_bytes) {
            if (s->eof) return 0;
            s->in_pos = s->in_len;                     /* nothing usable here: read on */
            continue;
        }
        s->in_pos += (uint32_t)info.frame_bytes;
        *pos += (uint32_t)info.frame_bytes;
        if (!n) continue;
        if (info.channels == 2)
            for (int i = 0; i < n; i++) g_pcm[i] = (int16_t)(((int32_t)g_pcm[2 * i] + g_pcm[2 * i + 1]) >> 1);
        return n;
    }
    return 0;
}

/* ---------------------------------------------------------------- output */

static volatile uint16_t g_head, g_tail;
static uint64_t g_pushed;
static uint32_t g_out_rate, g_rs_step, g_rs_phase;
static int32_t  g_rs_prev;
static uint32_t g_keys;
#define URGENT_KEYS (K_B | K_LEFT | K_RIGHT | K_UP | K_DOWN | K_REDRAW | K_TIME | K_GAME | K_A)

static int ring_fill(void) { return (uint16_t)(g_head - g_tail) & (RING_SAMPLES - 1); }
static int ring_free(void) { return RING_SAMPLES - 1 - ring_fill(); }
static void ring_reset(void) { g_tail = g_head; g_pushed = 0; g_rs_phase = 0; }

static bool rate_supported(uint32_t r)
{
    /* 8000 and 11025 are left out: the SAI ran them too fast (the samples are doubled instead) */
    static const uint32_t R[] = { 12000, 16000, 18000, 22050, 32000, 44100, 48000 };
    for (unsigned i = 0; i < sizeof R / sizeof R[0]; i++) if (R[i] == r) return true;
    return false;
}
static uint32_t out_rate_for(uint32_t r)
{
    if (rate_supported(r)) return r;
    return r > 48000 ? 48000 : r > 32000 ? 44100 : r > 16000 ? 22050 : r > 11025 ? 16000 : r > 8000 ? 22050 : 16000;
}

static void audio_attach(void)
{
    odroid_audio_init((int)g_out_rate);
    uint32_t half = g_out_rate / 60;
    if (half > 1000) half = 1000;
    audio_start_playing((uint16_t)half);
    pcm_attach(g_ring, RING_SAMPLES, &g_head, &g_tail);
    pcm_audio_enable(1);
}

static void set_rate(uint32_t in_rate)
{
    g_out_rate = out_rate_for(in_rate);
    g_rs_step = g_out_rate == in_rate ? 0 : (uint32_t)(((uint64_t)in_rate << 16) / g_out_rate);
    g_rs_prev = 0;
    audio_attach();
    ring_reset();
}

static void player_tick(void);

static bool wait_room(void)
{
    while (ring_free() < 1) {
        wdog_refresh();
        g_keys |= input_poll_pressed();
        player_tick();
        if (g_keys & URGENT_KEYS) return false;
        HAL_Delay(2);
    }
    return true;
}

/* mono samples into the ring; returns how many were taken (fewer if a key interrupted) */
static int push(const int16_t *p, int n)
{
    for (int i = 0; i < n; i++) {
        int32_t s = p[i];
        if (!g_rs_step) {
            if (!wait_room()) return i;
            g_ring[g_head] = (int16_t)s;
            g_head = (uint16_t)((g_head + 1) & (RING_SAMPLES - 1));
            g_pushed++;
        } else {
            while (g_rs_phase < 0x10000) {
                if (!wait_room()) return i;          /* the phase is kept: resumes with this sample */
                g_ring[g_head] = (int16_t)(g_rs_prev + (((s - g_rs_prev) * (int32_t)(g_rs_phase >> 4)) >> 12));
                g_head = (uint16_t)((g_head + 1) & (RING_SAMPLES - 1));
                g_pushed++;
                g_rs_phase += g_rs_step;
            }
            g_rs_phase -= 0x10000;
            g_rs_prev = s;
        }
    }
    return n;
}

/* ---------------------------------------------------------------- screen */

static struct {
    src_t *s;
    const char *name;
    uint32_t base_ms;                /* position of the first sample in the ring after a seek */
    bool paused, menu;
    uint32_t title_until, next_draw;
} P;

static uint32_t pos_ms(void)
{
    if (!g_out_rate) return P.base_ms;
    return P.base_ms + (uint32_t)((g_pushed - (uint64_t)ring_fill()) * 1000 / g_out_rate);
}

static void fmt_time(char *out, size_t n, uint32_t s)
{
    if (s >= 3600) snprintf(out, n, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
    else snprintf(out, n, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

static void draw(void)
{
    lcd_sleep_while_swap_pending();
    uint16_t *fb = lcd_get_active_buffer();
    memset(fb, 0, FM_W * FM_H * 2);
    const src_t *s = P.s;

    /* the waveform being played now (what follows the ring's read position) */
    const int mid = 112, amp = 70;
    uint16_t tail = g_tail;
    int fill = ring_fill();
    int prev = mid;
    for (int x = 0; x < FM_W; x++) {
        int v = 0;
        if (!P.paused && x * 2 < fill) v = g_ring[(tail + x * 2) & (RING_SAMPLES - 1)];
        int y = mid - v * amp / 32768;
        int y0 = prev < y ? prev : y, y1 = prev < y ? y : prev;
        if (x == 0) y0 = y1 = y;
        for (int yy = y0; yy <= y1; yy++) fb[yy * FM_W + x] = 0x07EF;
        prev = y;
    }
    ui_fill(0, mid, FM_W, 1, 0x2945);

    /* the time below and a short format line */
    char t1[16], t2[16], line[64];
    uint32_t cur = pos_ms(), tot = s->total_ms;
    if (tot && cur > tot) cur = tot;
    fmt_time(t1, sizeof t1, cur / 1000);
    fmt_time(t2, sizeof t2, (tot + 500) / 1000);
    snprintf(line, sizeof line, "%s %s / %s", P.paused ? "||" : ">", t1, tot ? t2 : "?");
    ui_fill(0, FM_H - 14, FM_W, 14, C_BAR);
    ui_text(1, FM_ROWS - 1, line, C_BAR_TEXT, C_BAR);
    int x0 = 8 * ((int)strlen(line) + 3), w = FM_W - x0 - 8;
    if (w > 20 && tot) {
        ui_fill(x0, FM_H - 9, w, 4, 0x4208);
        ui_fill(x0, FM_H - 9, (int)((uint64_t)w * cur / tot), 4, C_SEL_BG);
    }
    snprintf(line, sizeof line, "%s  %lu Hz  %s", s->kind == SRC_MP3 ? "MP3" : s->kind == SRC_FLAC ? "FLAC" : "WAV", (unsigned long)s->rate,
             s->channels == 1 ? "mono" : "stereo");
    ui_text_px((FM_W - (int)strlen(line) * 8) / 2, FM_H - 28, line, C_DIM, C_BG);

    /* name (or ID3 title / artist) at the start and while paused */
    if (P.paused || (int32_t)(P.title_until - HAL_GetTick()) > 0) {
        if (s->title[0]) ui_banner(s->title, s->artist[0] ? s->artist : P.name);
        else ui_banner(P.name, NULL);
    }
    fm_overlay_draw();
    lcd_swap();
}

static void player_tick(void)
{
    fm_overlay_expired();
    if ((int32_t)(HAL_GetTick() - P.next_draw) >= 0) {
        P.next_draw = HAL_GetTick() + 40;
        draw();
    }
}

static void menu_hook(bool entering)
{
    P.menu = entering;
    if (!entering && g_out_rate) audio_attach();
    pcm_audio_set(common_emu_sound_get_volume(), (entering || P.paused) ? 0 : 1);
}

/* ---------------------------------------------------------------- FLAC glue */

static int flac_open_src(src_t *s, uint32_t size)
{
    int r = flac_open(s->f, size, &s->fi, g_in, IN_SIZE);
    if (r) return r;
    s->kind = SRC_FLAC;
    s->rate = s->fi.rate;
    s->channels = s->fi.channels;
    s->bits = s->fi.bps;
    s->data_start = s->fi.first_frame;
    s->data_end = size;
    s->total_ms = (uint32_t)(s->fi.total * 1000 / s->fi.rate);
    snprintf(s->title, sizeof s->title, "%s", s->fi.title);
    snprintf(s->artist, sizeof s->artist, "%s", s->fi.artist);
    return 0;
}

static void flac_restart(src_t *s, uint32_t pos)
{
    flac_start(s->f, &s->fi, pos, s->data_end, fm_scratch + OFF_FIN, FIN_SIZE,
               (int32_t *)(fm_scratch + OFF_FCH), (int32_t *)(fm_scratch + OFF_FCH + FCH_BYTES));
}

static int flac_read(src_t *s)
{
    uint64_t first;
    int n = flac_decode(g_pcm, &first);
    if (n > 0 && s->resync) {                     /* where the seek really landed */
        s->resync = false;
        P.base_ms = (uint32_t)(first * 1000 / s->rate);
    }
    return n;
}

/* ---------------------------------------------------------------- player */

static void seek_to(src_t *s, uint32_t *pos, int64_t ms)
{
    if (ms < 0) ms = 0;
    if (s->total_ms && ms > s->total_ms) ms = s->total_ms;
    if (s->kind == SRC_WAV) {
        uint64_t frame = (uint64_t)ms * s->rate / 1000;
        *pos = s->data_start + (uint32_t)(frame * s->align);
        if (*pos > s->data_end) *pos = s->data_end;
        ms = (int64_t)(frame * 1000 / s->rate);
    } else if (s->kind == SRC_FLAC) {
        uint32_t bytes = s->data_end - s->data_start;
        uint32_t off = s->total_ms ? (uint32_t)((uint64_t)bytes * (uint64_t)ms / s->total_ms) : 0;
        if (off > 64) off -= 64;                   /* land just before a frame rather than after */
        *pos = s->data_start + (off < bytes ? off : bytes);
        flac_restart(s, *pos);
        s->resync = ms > 0;
    } else {
        uint32_t bytes = s->data_end - s->data_start, off;
        if (s->has_toc && s->total_ms) {
            uint32_t pct10 = (uint32_t)(ms * 1000 / s->total_ms);              /* 0..1000 */
            uint32_t k = pct10 / 10; if (k > 99) k = 99;
            uint32_t a = s->toc[k], b = k < 99 ? s->toc[k + 1] : 256;
            off = (uint32_t)(((uint64_t)a * 10 + (uint64_t)(b - a) * (pct10 % 10)) * bytes / 2560);
        } else off = (uint32_t)((uint64_t)ms * s->avg_bps / 1000);
        if (off > bytes) off = bytes;
        *pos = s->data_start + off;
        mp3_restart(s, *pos);
    }
    ring_reset();
    P.base_ms = (uint32_t)ms;
}

au_result_t play_audio(const char *path)
{
    const char *name = path_base(path);
    if (!ext_is(path, PLAYABLE)) {
        const char *dot = strrchr(path, '.');
        char l1[64];
        snprintf(l1, sizeof l1, "%s: %s", tr(T_AUD_FORMAT), dot ? dot + 1 : "?");
        ui_message(name, l1, tr(T_AUD_CONVERT), NULL, false);
        return AU_ERROR;
    }
    static src_t s;
    memset(&s, 0, sizeof s);
    s.f = fopen(path, "rb");
    if (!s.f) { ui_message(name, tr(T_AUD_BROKEN), NULL, NULL, false); return AU_ERROR; }
    uint32_t size = file_size(s.f);
    uint8_t magic[12] = { 0 };
    rd_at(s.f, 0, magic, sizeof magic);
    /* other formats saved with a playable extension */
    const char *real = !memcmp(magic, "OggS", 4) ? "OGG" :
                       !memcmp(magic + 4, "ftyp", 4) ? "M4A/MP4" : !memcmp(magic, "FORM", 4) ? "AIFF" :
                       !memcmp(magic, "\x30\x26\xB2\x75", 4) ? "WMA" : NULL;
    bool flac = !memcmp(magic, "fLaC", 4) || (!memcmp(magic, "ID3", 3) && ext_is(path, (const char *const[]){ "flac", "fla", NULL }));
    int r = real ? -4 : flac ? flac_open_src(&s, size) : !memcmp(magic, "RIFF", 4) ? wav_open(&s, size) : mp3_open(&s, size);
    if (r) {
        fclose(s.f);
        char l1[64];
        if (r == -4) {
            snprintf(l1, sizeof l1, "%s: %s", tr(T_AUD_FORMAT), real);
            ui_message(name, l1, tr(T_AUD_CONVERT), NULL, false);
        } else if (r == -3) ui_message(name, tr(T_AUD_EMPTY), tr(T_AUD_DOWNLOAD), NULL, false);
        else ui_message(name, tr(r == -2 ? T_AUD_CODEC : T_AUD_BROKEN), tr(T_AUD_CONVERT), NULL, false);
        return AU_ERROR;
    }

    memset(&P, 0, sizeof P);
    P.s = &s;
    P.name = name;
    P.title_until = HAL_GetTick() + 3000;
    g_head = g_tail = 0;
    g_out_rate = 0;
    uint32_t pos = s.data_start;
    if (s.kind == SRC_MP3) mp3_restart(&s, pos);
    if (s.kind == SRC_FLAC) flac_restart(&s, pos);
    set_rate(s.rate);
    fm_menu_hook = menu_hook;
    g_keys = 0;
    (void)input_poll_pressed();                  /* drop the A that opened the file */

    au_result_t res = AU_END;
    int n = 0, done = 0;                         /* samples in g_pcm / already pushed */
    bool ended = false;
    for (;;) {
        wdog_refresh();
        pcm_audio_set(common_emu_sound_get_volume(), (P.paused || P.menu) ? 0 : 1);
        g_keys |= input_poll_pressed();
        uint32_t k = g_keys;
        g_keys = 0;
        if (k & K_B)    { res = AU_STOP; break; }
        if (k & K_TIME) { res = AU_NEXT; break; }
        if (k & K_GAME) { res = AU_PREV; break; }
        if (k & K_A) P.paused = !P.paused;
        int seek = 0;
        if (k & K_LEFT)  seek = -10;
        if (k & K_RIGHT) seek = 10;
        if (k & K_DOWN)  seek = -60;
        if (k & K_UP)    seek = 60;
        if (seek) {
            int64_t to = (int64_t)pos_ms() + seek * 1000;
            if (s.total_ms && to >= (int64_t)s.total_ms) { res = AU_END; break; }   /* past the end: next file */
            seek_to(&s, &pos, to);
            n = done = 0;
            ended = false;
        }
        player_tick();
        if (P.paused) { HAL_Delay(10); continue; }

        if (done >= n) {
            if (ended) {
                if (ring_fill() == 0) break;
                HAL_Delay(5);
                continue;
            }
            n = s.kind == SRC_WAV ? wav_read(&s, &pos) : s.kind == SRC_FLAC ? flac_read(&s) : mp3_read(&s, &pos);
            done = 0;
            if (n <= 0) { n = 0; ended = true; continue; }
        }
        done += push(g_pcm + done, n - done);       /* returns early when a key is waiting */
    }

    fm_menu_hook = NULL;
    fm_overlay_clear();
    pcm_audio_enable(0);
    pcm_audio_set(0, 0);
    audio_stop_playing();
    fclose(s.f);
    return res;
}
