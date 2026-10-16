/*
 * File Manager — video player: AVI with Motion-JPEG video and PCM audio.
 *
 * The frames are decoded by the G&W's hardware JPEG decoder (firmware ABI: JPEG_DecodeToFrame*)
 * straight into the LCD back buffer; the audio goes through the firmware's PCM ring for
 * homebrews (pcm_attach: the SAI interrupt reads it, so playback never stalls the sound).
 * The audio is the master clock: a frame is shown when the samples played reach its time, and
 * frames that are already late are dropped (every MJPEG frame is a key frame).
 *
 * Anything else (MP4/MKV/MOV containers, H.264/MPEG-4/... codecs, frames bigger than the
 * screen) gets a message with what to do: convert with ffmpeg, see the README.
 *
 * Memory: fm_scratch = 160 KB hardware-decoder YCbCr buffer + 32 KB audio ring;
 *         the dialog background buffer (150 KB) holds the compressed frame.
 *
 * Keys: A pause/resume, Left/Right -/+10 s, Up/Down -/+60 s, B stop, PAUSE/SET Retro-Go menu.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "fm.h"
#include "third_party/minimp3/minimp3.h"      /* MP3 sound (the decoder itself is built in audio.c) */

extern void wdog_refresh(void);
extern uint32_t HAL_GetTick(void);
extern bool fm_lang_es(void);

/* firmware (through the SDK bridge) */
uint32_t JPEG_DecodeToFrameInit(uint32_t JPEG_Buffer, uint32_t JPEG_Buffer_Size);
uint32_t JPEG_DecodeToFrame(uint32_t SrcAddress, uint32_t DestAddress, uint16_t x, uint16_t y, uint8_t luma_alpha);
uint32_t JPEG_DecodeDeInit(void);
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

#define YCBCR_SIZE   (116 * 1024)                /* 320x240 4:2:0 = 115200 bytes */
#define RING_SAMPLES 16384                       /* power of two, 32 KB */
#define FRAME_MAX    (FM_W * FM_H * 2)           /* the dialog background buffer */
/* fm_scratch: YCbCr | ring | MP3 decoder state, scratch, one frame of samples, input bytes */
#define V_OFF_RING   YCBCR_SIZE
#define V_OFF_DEC    (V_OFF_RING + RING_SAMPLES * 2)
#define V_OFF_SCR    (V_OFF_DEC + 7 * 1024)       /* sizes checked in audio.c */
#define V_OFF_PCM    (V_OFF_SCR + 16 * 1024)
#define V_OFF_ACC    (V_OFF_PCM + MINIMP3_MAX_SAMPLES_PER_FRAME * 2)
#define V_ACC_SIZE   (FM_SCRATCH_SIZE - V_OFF_ACC)
_Static_assert(V_ACC_SIZE >= 12 * 1024, "fm_scratch: MP3 input");
extern uint8_t *fm_mp3_scratch;
#define CKPT_EVERY   128                         /* seek checkpoint every 128 video frames */
#define CKPT_MAX     1024                        /* 131072 frames: 2.4 h at 15 fps (then seeks scan) */

static inline uint32_t fcc(const char *s) { return (uint32_t)s[0] | s[1] << 8 | s[2] << 16 | (uint32_t)s[3] << 24; }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* ================================================================ AVI */

typedef struct {
    FILE *f;
    uint32_t us_per_frame, total_frames;
    int w, h;
    int vstream, astream;              /* stream numbers in the chunk ids, -1 = none */
    uint32_t vcodec;                   /* fourcc */
    int a_format, a_channels, a_bits;
    uint32_t a_rate, a_avg;            /* a_avg: bytes per second (MP3) */
    bool a_mp3;
    uint32_t movi_start, movi_end;     /* data of the current LIST movi */
    uint32_t idx1_off, idx1_n;         /* legacy index (first RIFF only), 0 = none */
    uint32_t idx1_base;                /* what idx1 offsets are relative to */
    /* position */
    uint32_t pos;                      /* next chunk */
    uint32_t frame;                    /* next video frame number */
    uint64_t asamples;                 /* audio samples (per channel) before the next audio chunk */
} avi_t;

typedef struct { uint32_t pos, frame, asamples, movi_end; } ckpt_t;
static ckpt_t   g_ckpt[CKPT_MAX];
static int      g_nckpt;

static bool rd(avi_t *a, uint32_t off, void *buf, size_t n)
{
    return fseek(a->f, (long)off, SEEK_SET) == 0 && fread(buf, 1, n, a->f) == n;
}

typedef enum { V_OK, V_NOT_AVI, V_CODEC, V_TOO_BIG, V_ERROR } vres_t;

static vres_t avi_open(avi_t *a)
{
    uint8_t h[12];
    if (!rd(a, 0, h, 12) || le32(h) != fcc("RIFF") || le32(h + 8) != fcc("AVI ")) return V_NOT_AVI;
    uint32_t riff_end = 8 + le32(h + 4);
    uint32_t off = 12;
    int stream = -1;
    uint32_t stype = 0;
    a->vstream = a->astream = -1;
    a->movi_start = 0;
    while (off + 8 <= riff_end) {
        uint8_t c[12];
        if (!rd(a, off, c, 12)) break;
        uint32_t id = le32(c), size = le32(c + 4);
        if (id == fcc("LIST")) {
            uint32_t type = le32(c + 8);
            if (type == fcc("movi")) { a->movi_start = off + 12; a->movi_end = off + 8 + size; break; }
            if (type == fcc("hdrl") || type == fcc("strl")) {
                if (type == fcc("strl")) stream++;
                off += 12;                       /* descend */
                continue;
            }
        } else if (id == fcc("avih") && size >= 40) {
            uint8_t b[40];
            if (!rd(a, off + 8, b, 40)) return V_ERROR;
            a->us_per_frame = le32(b);
            a->total_frames = le32(b + 16);
            a->w = (int)le32(b + 32); a->h = (int)le32(b + 36);
        } else if (id == fcc("strh") && size >= 32) {
            uint8_t b[32];
            if (!rd(a, off + 8, b, 32)) return V_ERROR;
            stype = le32(b);
            if (stype == fcc("vids") && a->vstream < 0) {
                a->vstream = stream;
                uint32_t scale = le32(b + 20), rate = le32(b + 24);
                if (rate && scale) a->us_per_frame = (uint32_t)((uint64_t)scale * 1000000 / rate);
            } else if (stype == fcc("auds") && a->astream < 0) {
                a->astream = stream;
            }
        } else if (id == fcc("strf")) {
            if (stype == fcc("vids") && stream == a->vstream && size >= 20) {
                uint8_t b[20];
                if (!rd(a, off + 8, b, 20)) return V_ERROR;
                a->w = (int)le32(b + 4);
                a->h = (int)le32(b + 8); if (a->h < 0) a->h = -a->h;
                a->vcodec = le32(b + 16);
            } else if (stype == fcc("auds") && stream == a->astream && size >= 16) {
                uint8_t b[16];
                if (!rd(a, off + 8, b, 16)) return V_ERROR;
                a->a_format = le16(b); a->a_channels = le16(b + 2);
                a->a_rate = le32(b + 4); a->a_avg = le32(b + 8); a->a_bits = le16(b + 14);
                a->a_mp3 = a->a_format == 0x55 || a->a_format == 0x50;
            }
        }
        off += 8 + ((size + 1) & ~1u);
    }
    if (!a->movi_start || a->vstream < 0) return V_ERROR;
    char cc[5] = { (char)(a->vcodec & 0xFF), (char)(a->vcodec >> 8), (char)(a->vcodec >> 16), (char)(a->vcodec >> 24), 0 };
    if (strcasecmp(cc, "MJPG") && strcasecmp(cc, "JPEG") && strcasecmp(cc, "AVRN") && strcasecmp(cc, "LJPG") &&
        strcasecmp(cc, "DMB1"))
        return V_CODEC;
    if (a->w <= 0 || a->h <= 0 || a->w > FM_W || a->h > FM_H) return V_TOO_BIG;
    if (!a->us_per_frame) a->us_per_frame = 66667;
    /* idx1 right after the movi list: makes seeking a single read of the index */
    {
        uint8_t c[8];
        uint32_t off = (a->movi_end + 1) & ~1u;
        a->idx1_off = 0;
        if (rd(a, off, c, 8) && le32(c) == fcc("idx1") && le32(c + 4) >= 16) {
            uint8_t e[16];
            if (rd(a, off + 8, e, 16)) {
                uint32_t o = le32(e + 8), id = le32(e);
                uint8_t h[4];
                /* offsets are relative to the 'movi' fourcc (usual) or absolute */
                if (rd(a, a->movi_start - 4 + o, h, 4) && le32(h) == id) a->idx1_base = a->movi_start - 4;
                else if (rd(a, o, h, 4) && le32(h) == id) a->idx1_base = 0;
                else a->idx1_base = 1;            /* unusable */
                if (a->idx1_base != 1) { a->idx1_off = off + 8; a->idx1_n = le32(c + 4) / 16; }
            }
        }
    }
    a->pos = a->movi_start;
    a->frame = 0;
    a->asamples = 0;
    return V_OK;
}

/* chunk id -> 0 video, 1 audio, -1 other */
static int chunk_kind(const avi_t *a, uint32_t id)
{
    char c0 = (char)(id & 0xFF), c1 = (char)(id >> 8);
    if (c0 < '0' || c0 > '9' || c1 < '0' || c1 > '9') return -1;
    int n = (c0 - '0') * 10 + (c1 - '0');
    char t0 = (char)(id >> 16), t1 = (char)(id >> 24);
    if (n == a->vstream && t0 == 'd' && (t1 == 'c' || t1 == 'b')) return 0;
    if (n == a->astream && t0 == 'w' && t1 == 'b') return 1;
    return -1;
}

/* Next video or audio chunk at or after a->pos: fills kind/size/data offset and moves a->pos past it.
 * Handles LIST rec, JUNK, index chunks and OpenDML "RIFF AVIX" continuations. */
static bool next_chunk(avi_t *a, int *kind, uint32_t *size, uint32_t *data)
{
    for (;;) {
        if (a->pos + 8 > a->movi_end) {
            /* OpenDML: the movie may go on in a RIFF AVIX after this one */
            uint8_t r[12];
            uint32_t off = (a->movi_end + 1) & ~1u;
            bool found = false;
            while (rd(a, off, r, 12)) {
                uint32_t id = le32(r), sz = le32(r + 4);
                if (id == fcc("RIFF") && le32(r + 8) == fcc("AVIX")) { off += 12; continue; }
                if (id == fcc("LIST") && le32(r + 8) == fcc("movi")) {
                    a->pos = off + 12; a->movi_end = off + 8 + sz; found = true; break;
                }
                off += 8 + ((sz + 1) & ~1u);
            }
            if (!found) return false;
        }
        uint8_t c[12];
        if (!rd(a, a->pos, c, 12)) return false;
        uint32_t id = le32(c), sz = le32(c + 4);
        if (id == fcc("LIST")) { a->pos += 12; continue; }      /* LIST rec: descend */
        int k = chunk_kind(a, id);
        uint32_t d = a->pos + 8;
        a->pos += 8 + ((sz + 1) & ~1u);
        if (k < 0) continue;
        *kind = k; *size = sz; *data = d;
        return true;
    }
}

static uint32_t audio_frame_bytes(const avi_t *a) { return (uint32_t)(a->a_channels * (a->a_bits / 8)); }
/* audio samples in a chunk of `bytes` (MP3: from the average byte rate) */
static uint32_t chunk_samples(const avi_t *a, uint32_t bytes)
{
    if (a->astream < 0) return 0;
    if (a->a_mp3) return a->a_avg ? (uint32_t)((uint64_t)bytes * a->a_rate / a->a_avg) : 0;
    uint32_t afb = a->a_bits ? audio_frame_bytes(a) : 0;
    return afb ? bytes / afb : 0;
}

static void ckpt_record(const avi_t *a, uint32_t chunk_pos)
{
    if (a->frame % CKPT_EVERY) return;
    int i = (int)(a->frame / CKPT_EVERY);
    if (i >= CKPT_MAX || i < g_nckpt) return;
    if (i != g_nckpt) return;                      /* only in order */
    g_ckpt[i] = (ckpt_t){ chunk_pos, a->frame, (uint32_t)a->asamples, a->movi_end };
    g_nckpt++;
}

/* Moves to video frame `target` without decoding (counting the audio on the way). */
/* Seek through idx1: a sequential read of the index, 16 bytes per chunk. */
static bool avi_seek_idx(avi_t *a, uint32_t target, uint32_t first_movi_end)
{
    if (!a->idx1_off) return false;
    uint8_t *buf = fm_scratch;                     /* the decoder's buffer: free between frames */
    const uint32_t per = 4096 / 16;
    uint32_t frame = 0;
    uint64_t asamples = 0;
    for (uint32_t i = 0; i < a->idx1_n; i += per) {
        uint32_t n = a->idx1_n - i < per ? a->idx1_n - i : per;
        if (!rd(a, a->idx1_off + i * 16, buf, n * 16)) return false;
        for (uint32_t j = 0; j < n; j++) {
            const uint8_t *e = buf + j * 16;
            int k = chunk_kind(a, le32(e));
            if (k == 0) {
                if (frame == target) {
                    a->pos = a->idx1_base + le32(e + 8);
                    a->frame = frame; a->asamples = asamples; a->movi_end = first_movi_end;
                    return true;
                }
                frame++;
            } else if (k == 1) asamples += chunk_samples(a, le32(e + 12));
        }
        wdog_refresh();
    }
    return false;                                  /* beyond the index (OpenDML part): scan */
}

static void avi_seek(avi_t *a, uint32_t target, uint32_t first_movi_end)
{
    if (avi_seek_idx(a, target, first_movi_end)) return;
    if (target < a->frame) {
        int i = (int)(target / CKPT_EVERY);
        if (i >= g_nckpt) i = g_nckpt - 1;
        if (i >= 0) {
            a->pos = g_ckpt[i].pos; a->frame = g_ckpt[i].frame;
            a->asamples = g_ckpt[i].asamples; a->movi_end = g_ckpt[i].movi_end;
        } else {
            a->pos = a->movi_start; a->frame = 0; a->asamples = 0; a->movi_end = first_movi_end;
        }
    }
    while (a->frame < target) {
        uint32_t before = a->pos;
        int kind; uint32_t size, data;
        if (!next_chunk(a, &kind, &size, &data)) break;
        if (kind == 0) {
            /* the chunk header just consumed starts at data - 8 */
            ckpt_record(a, data - 8);
            a->frame++;
        } else a->asamples += chunk_samples(a, size);
        (void)before;
        if ((a->frame & 63) == 0) wdog_refresh();
    }
}

/* ================================================================ audio */

static int16_t           *g_ring;
static volatile uint16_t  g_head, g_tail;
static uint64_t           g_pushed;          /* samples pushed since the clock base */
static uint32_t           g_out_rate;        /* rate the SAI runs at */
static uint32_t           g_rs_step, g_rs_phase;   /* resampling (16.16) when the file's rate is not supported */
static int32_t            g_rs_prev;

static uint32_t g_keys;                    /* presses waiting to be handled by the player loop */
#define URGENT_KEYS (K_B | K_LEFT | K_RIGHT | K_UP | K_DOWN | K_REDRAW)   /* stop, seek, Retro-Go menu: drop what is being done */
static void poll_keys(void) { g_keys |= input_poll_pressed(); }

static int ring_fill(void) { return (uint16_t)(g_head - g_tail) & (RING_SAMPLES - 1); }
static int ring_free(void) { return RING_SAMPLES - 1 - ring_fill(); }

static void ring_reset(void)
{
    /* drop what is queued; the ring keeps playing (silenced here, it would never drain and a
     * full ring would block the player) */
    g_tail = g_head;
    g_pushed = 0;
    g_rs_phase = 0;
}

static bool rate_supported(uint32_t r)
{
    /* 8000 and 11025 are left out: the SAI ran them too fast (the samples are doubled instead) */
    static const uint32_t R[] = { 12000, 16000, 18000, 22050, 32000, 44100, 48000 };
    for (unsigned i = 0; i < sizeof R / sizeof R[0]; i++) if (R[i] == r) return true;
    return false;
}

static inline void ring_put(int16_t s)
{
    g_ring[g_head] = s;
    g_head = (uint16_t)((g_head + 1) & (RING_SAMPLES - 1));
    g_pushed++;
}

static void player_idle(void);
/* one mono sample into the ring (waits for room); false if a key interrupted */
static bool push_one(int32_t s)
{
    if (!g_rs_step) {
        while (ring_free() < 1) {
            wdog_refresh(); poll_keys(); player_idle();
            if (g_keys & URGENT_KEYS) return false;
            HAL_Delay(1);
        }
        ring_put((int16_t)s);
        return true;
    }
    /* linear resampling to g_out_rate */
    while (g_rs_phase < 0x10000) {
        while (ring_free() < 1) {
            wdog_refresh(); poll_keys(); player_idle();
            if (g_keys & URGENT_KEYS) return false;
            HAL_Delay(1);
        }
        int32_t v = g_rs_prev + (((s - g_rs_prev) * (int32_t)(g_rs_phase >> 4)) >> 12);
        ring_put((int16_t)v);
        g_rs_phase += g_rs_step;
    }
    g_rs_phase -= 0x10000;
    g_rs_prev = s;
    return true;
}

/* PCM bytes from the file -> mono 16-bit into the ring */
static void push_pcm(const avi_t *a, const uint8_t *p, uint32_t n_frames)
{
    int ch = a->a_channels, bytes = a->a_bits / 8;
    for (uint32_t i = 0; i < n_frames; i++) {
        int32_t s = 0;
        for (int c = 0; c < ch; c++) {
            const uint8_t *q = p + ((size_t)i * ch + c) * bytes;
            s += bytes == 1 ? ((int)q[0] - 128) * 256 : (int16_t)(q[0] | q[1] << 8);
        }
        if (!push_one(s / ch)) return;
    }
}

/* MP3 sound: chunk bytes gather in an input buffer and whole frames are decoded from it. The
 * decoder only gets a frame when a few more KB follow (or at the end), as it needs the next
 * frame's header to be sure of the sync. */
#define MP3_KEEP 4096
#define V_DEC ((mp3dec_t *)(fm_scratch + V_OFF_DEC))
#define V_PCM ((int16_t *)(fm_scratch + V_OFF_PCM))
#define V_ACC (fm_scratch + V_OFF_ACC)
static uint32_t g_acc_len;

static void mp3_reset(void) { mp3dec_init(V_DEC); g_acc_len = 0; }

int fm_mp3_frame_len(const uint8_t *h);

static void mp3_decode_acc(bool flush)
{
    uint32_t at = 0;
    for (;;) {
        uint32_t left = g_acc_len - at;
        if (left < 5) break;
        /* a whole frame here: decode it now, alone (the picture waits for this sound);
         * anything else only with plenty of data, so the decoder can find its way */
        int fl = fm_mp3_frame_len(V_ACC + at);
        if (fl ? left < (uint32_t)fl : (left < MP3_KEEP && !flush)) break;
        mp3dec_frame_info_t info;
        int n = mp3dec_decode_frame(V_DEC, V_ACC + at, fl ? fl : (int)left, V_PCM, &info);
        if (!info.frame_bytes) break;
        at += (uint32_t)info.frame_bytes;
        bool stop = false;
        for (int i = 0; i < n && !stop; i++)
            stop = !push_one(info.channels == 2 ? ((int32_t)V_PCM[2 * i] + V_PCM[2 * i + 1]) / 2 : V_PCM[i]);
        if (stop || (g_keys & URGENT_KEYS)) break;
    }
    memmove(V_ACC, V_ACC + at, g_acc_len - at);
    g_acc_len -= at;
}

static void mp3_feed(const avi_t *a, uint32_t off, uint32_t size)
{
    while (size) {
        if (g_acc_len == V_ACC_SIZE) g_acc_len = 0;          /* full of something undecodable */
        uint32_t n = V_ACC_SIZE - g_acc_len;
        if (n > size) n = size;
        if (!rd(a, off, V_ACC + g_acc_len, n)) return;
        g_acc_len += n; off += n; size -= n;
        mp3_decode_acc(false);
        if (g_keys & URGENT_KEYS) return;
    }
}

/* ================================================================ player */

static bool g_paused_for_menu;
/* (Re)starts the SAI at the video's rate with the PCM ring attached. Also after the Retro-Go
 * menu: the firmware may stop or restart the audio DMA there (which drops pcm ownership). */
static void audio_attach(void)
{
    odroid_audio_init((int)g_out_rate);
    uint32_t half = g_out_rate / 60;
    if (half > 1000) half = 1000;
    audio_start_playing((uint16_t)half);
    pcm_attach(g_ring, RING_SAMPLES, &g_head, &g_tail);
    pcm_audio_enable(1);
}

static void reload_pending(void);
static bool g_sound_on;

/* Around PAUSE/SET (Retro-Go menu, volume/brightness shortcuts): the frame buffer is the dialog
 * background, so the frame waiting to be shown is read again afterwards; the sound pauses
 * meanwhile and is set up again (the firmware may have stopped or restarted the audio DMA). */
static void menu_hook(bool entering)
{
    g_paused_for_menu = entering;
    if (!entering) reload_pending();
    if (!g_sound_on) return;
    if (!entering) audio_attach();
    pcm_audio_set(common_emu_sound_get_volume(), entering ? 0 : 1);
}

static void fmt_time(char *out, size_t n, uint32_t s)
{
    if (s >= 3600) snprintf(out, n, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
    else snprintf(out, n, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

/* bar at the bottom of the back buffer: state, time / total and progress */
static void draw_osd(const avi_t *a, uint32_t frame, bool paused)
{
    char t1[16], t2[16], line[48];
    uint32_t cur = (uint32_t)((uint64_t)frame * a->us_per_frame / 1000000);
    uint32_t tot = (uint32_t)(((uint64_t)a->total_frames * a->us_per_frame + 500000) / 1000000);
    fmt_time(t1, sizeof t1, cur);
    fmt_time(t2, sizeof t2, tot);
    snprintf(line, sizeof line, "%s %s / %s", paused ? "||" : ">", t1, t2);
    ui_fill(0, FM_H - 14, FM_W, 14, C_BAR);
    ui_text(1, FM_ROWS - 1, line, C_BAR_TEXT, C_BAR);
    int x0 = 8 * ((int)strlen(line) + 3), w = FM_W - x0 - 8;
    if (w > 20 && a->total_frames) {
        ui_fill(x0, FM_H - 9, w, 4, 0x4208);
        ui_fill(x0, FM_H - 9, (int)((uint64_t)w * (frame > a->total_frames ? a->total_frames : frame) / a->total_frames), 4, C_SEL_BG);
    }
}

/* JPEG SOF size check, so the hardware decoder never gets more than its buffer */
/* What the hardware decoder path can show. The firmware's decoder only handles colour 4:2:0
 * frames correctly (4:2:2 is sized and converted as 4:4:4 and asserts when it does not fit;
 * greyscale is not converted), so only those are passed to it. */
typedef enum { FR_OK, FR_SUBSAMPLING, FR_BAD } frame_kind_t;

static frame_kind_t frame_kind(const uint8_t *d, uint32_t n)
{
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return FR_BAD;
    uint32_t i = 2;
    while (i + 12 < n) {
        if (d[i] != 0xFF) { i++; continue; }
        uint8_t m = d[i + 1];
        if (m == 0xD8 || m == 0x01 || m == 0xFF || (m >= 0xD0 && m <= 0xD7)) { i += m == 0xFF ? 1 : 2; continue; }
        uint32_t len = (uint32_t)(d[i + 2] << 8 | d[i + 3]);
        if (m >= 0xC0 && m <= 0xC3) {
            int h = d[i + 5] << 8 | d[i + 6], w = d[i + 7] << 8 | d[i + 8];
            int ncomp = d[i + 9];
            if (w <= 0 || h <= 0 || w > FM_W || h > FM_H) return FR_BAD;
            if (ncomp == 3 && i + 18 > n) return FR_BAD;
            if (ncomp != 3 || d[i + 11] != 0x22) return FR_SUBSAMPLING;   /* Y must be H2V2 */
            if (d[i + 14] != 0x11 || d[i + 17] != 0x11) return FR_SUBSAMPLING;
            uint32_t pw = (uint32_t)((w + 15) / 16 * 16), ph = (uint32_t)((h + 15) / 16 * 16);
            return pw * ph * 3 / 2 <= YCBCR_SIZE ? FR_OK : FR_BAD;
        }
        if (m == 0xDA) return FR_BAD;
        i += 2 + len;
    }
    return FR_BAD;
}

static bool frame_fits(const uint8_t *d, uint32_t n) { return frame_kind(d, n) == FR_OK; }

/* file name across the top for the first seconds */
static const char *g_vtitle;
static uint32_t g_title_until;
static bool title_visible(void) { return g_vtitle && (int32_t)(g_title_until - HAL_GetTick()) > 0; }

static void show_frame(const avi_t *a, const uint8_t *data, bool osd, uint32_t frame, bool paused)
{
    lcd_sleep_while_swap_pending();
    uint16_t *fb = lcd_get_active_buffer();
    if (a->w < FM_W || a->h < FM_H || osd) memset(fb, 0, FM_W * FM_H * 2);
    /* set the decoder up every time: the firmware (pause menu, sleep...) may have used it */
    JPEG_DecodeToFrameInit((uint32_t)fm_scratch, YCBCR_SIZE);
    JPEG_DecodeToFrame((uint32_t)data, (uint32_t)fb, (uint16_t)((FM_W - a->w) / 2), (uint16_t)((FM_H - a->h) / 2), 255);
    if (osd) draw_osd(a, frame, paused);
    if (title_visible()) ui_banner(g_vtitle, NULL);
    fm_overlay_draw();                       /* volume / brightness indicator (PAUSE+D-pad) */
    lcd_swap();
}

static void message(const char *title, const char *l1, const char *l2)
{
    ui_message(title, l1, l2, NULL, false);
}


#ifdef VID_DEBUG             /* emulator debugging: a log written to /vid.log every second */
#include <stdarg.h>
static char g_dbg[16384]; static int g_dbg_n;
static void dbg(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(g_dbg + g_dbg_n, sizeof g_dbg - g_dbg_n, fmt, ap); va_end(ap);
    if (n > 0 && g_dbg_n + n < (int)sizeof g_dbg) g_dbg_n += n;
}
static void dbg_flush(void) { FILE *f = fopen("/vid.log", "wb"); if (f) { fwrite(g_dbg, 1, g_dbg_n, f); fclose(f); } }
#endif

/* player state (also used from the audio waits in push_pcm) */
static struct {
    avi_t *a;
    bool sound, paused;
    uint8_t *frame_buf;
    uint64_t base_us;                 /* clock at tick0 / at sample 0 of the ring */
    uint32_t tick0;
    bool pending;                     /* a decoded-ready frame waiting for its time */
    uint32_t pend_frame, pend_pos, pend_size;
    uint32_t shown_frame, shown_pos, shown_size;
    uint32_t osd_until;
    bool underrun; uint32_t under_tick; uint64_t under_base;
} PL;

/* Playback clock in microseconds. With sound: the samples actually played (so the picture
 * follows the speaker); while the ring is empty it runs on the system tick instead, so a
 * stretch without audio cannot freeze the picture. */
static uint64_t clock_us(void)
{
    uint32_t now = HAL_GetTick();
    if (PL.paused) return PL.base_us;
    if (!PL.sound) return PL.base_us + (uint64_t)(now - PL.tick0) * 1000;
    uint64_t t = PL.base_us + (uint64_t)(g_pushed - (uint64_t)ring_fill()) * 1000000 / g_out_rate;
    if (ring_fill() == 0) {
        if (!PL.underrun) { PL.underrun = true; PL.under_tick = now; PL.under_base = t; }
        return PL.under_base + (uint64_t)(now - PL.under_tick) * 1000;
    }
    PL.underrun = false;
    return t;
}

static void show_pending(void)
{
    const avi_t *a = PL.a;
    show_frame(a, PL.frame_buf, PL.paused || (int32_t)(PL.osd_until - HAL_GetTick()) > 0, PL.pend_frame, PL.paused);
    PL.shown_frame = PL.pend_frame; PL.shown_pos = PL.pend_pos; PL.shown_size = PL.pend_size;
    PL.pending = false;
}

static void try_show(void)
{
    if (PL.pending && clock_us() >= (uint64_t)PL.pend_frame * PL.a->us_per_frame)
        show_pending();
}

/* the last frame again (pause, OSD, after the Retro-Go menu): read back from the file */
static void reload_pending(void)
{
    if (PL.pending && !rd(PL.a, PL.pend_pos, PL.frame_buf, PL.pend_size)) PL.pending = false;
}

static void redraw_shown(void)
{
    if (PL.pending) {                         /* the next frame is ready: that one, now if paused */
        if (PL.paused) show_pending();
        return;
    }
    if (!PL.shown_size) return;
    if (rd(PL.a, PL.shown_pos, PL.frame_buf, PL.shown_size))
        show_frame(PL.a, PL.frame_buf, true, PL.shown_frame, PL.paused);
}

static void player_idle(void) { try_show(); }

bool is_video_ext(const char *name)
{
    if (is_macos_meta(name)) return false;
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    static const char *const L[] = { "avi", "mp4", "m4v", "mkv", "mov", "webm", "wmv", "flv", "mpg", "mpeg", "3gp", "ts", NULL };
    for (const char *const *l = L; *l; l++) if (strcasecmp(dot + 1, *l) == 0) return true;
    return false;
}

void play_video(const char *path)
{
    const char *title = path_base(path);
    const char *dot = strrchr(path, '.');
    if (!dot || strcasecmp(dot + 1, "avi") != 0) {
        char l1[64];
        snprintf(l1, sizeof l1, "%s: %s", tr(T_VID_FORMAT), dot ? dot + 1 : "?");
        message(title, l1, tr(T_VID_CONVERT));
        return;
    }
    static avi_t a;
    memset(&a, 0, sizeof a);
    a.f = fopen(path, "rb");
    if (!a.f) { message(title, tr(T_ERROR), NULL); return; }
    vres_t r = avi_open(&a);
    if (r != V_OK) {
        char l1[64];
        if (r == V_CODEC) {
            char cc[5] = { (char)(a.vcodec & 0xFF), (char)(a.vcodec >> 8), (char)(a.vcodec >> 16), (char)(a.vcodec >> 24), 0 };
            for (int i = 0; i < 4; i++) if ((uint8_t)cc[i] < 0x20 || (uint8_t)cc[i] > 0x7e) cc[i] = '?';
            snprintf(l1, sizeof l1, "%s: %s", tr(T_VID_CODEC), cc);
        } else if (r == V_TOO_BIG) {
            snprintf(l1, sizeof l1, "%s: %dx%d (max. %dx%d)", tr(T_VID_SIZE), a.w, a.h, FM_W, FM_H);
        } else {
            snprintf(l1, sizeof l1, "%s", tr(T_IMG_BROKEN));
        }
        fclose(a.f);
        message(title, l1, tr(T_VID_CONVERT));
        return;
    }
    /* the first frame tells the JPEG layout: only colour 4:2:0 can be decoded here */
    {
        avi_t probe = a;
        int kind; uint32_t size, data;
        while (next_chunk(&probe, &kind, &size, &data)) {
            if (kind != 0 || size < 64) continue;
            uint32_t n = size < 4096 ? size : 4096;
            uint8_t *hdr = (uint8_t *)fm_canvas();
            if (rd(&a, data, hdr, n) && frame_kind(hdr, n) == FR_SUBSAMPLING) {
                fclose(a.f);
                message(title, tr(T_VID_SUBSAMPLING), tr(T_VID_CONVERT));
                return;
            }
            break;
        }
    }
    bool sound = a.astream >= 0 && (((a.a_format == 1 || a.a_format == 0xFFFE) &&
                 (a.a_bits == 8 || a.a_bits == 16) && a.a_channels >= 1 && a.a_channels <= 8 && a.a_rate >= 4000) ||
                 (a.a_mp3 && a.a_channels >= 1 && a.a_channels <= 2 && a.a_rate >= 8000));
    if (a.astream >= 0 && !sound) {
        char l1[64];
        snprintf(l1, sizeof l1, "%s (0x%04X)", tr(T_VID_NO_AUDIO), a.a_format);
        message(title, l1, tr(T_VID_CONVERT));
    }

    PL.a = &a;
    PL.sound = sound;
    PL.frame_buf = (uint8_t *)fm_canvas();
    g_ring = (int16_t *)(fm_scratch + V_OFF_RING);
    fm_mp3_scratch = fm_scratch + V_OFF_SCR;
    mp3_reset();
    g_head = g_tail = 0;
    g_nckpt = 0;
    uint32_t first_movi_end = a.movi_end;

    JPEG_DecodeToFrameInit((uint32_t)fm_scratch, YCBCR_SIZE);
    if (sound) {
        g_out_rate = rate_supported(a.a_rate) ? a.a_rate : 22050;
        g_rs_step = g_out_rate == a.a_rate ? 0 : (uint32_t)(((uint64_t)a.a_rate << 16) / g_out_rate);
        g_rs_prev = 0;
        audio_attach();
        ring_reset();
    }
    g_sound_on = sound;
    fm_menu_hook = menu_hook;

    PL.base_us = 0;
    PL.tick0 = HAL_GetTick();
    PL.paused = false;
    PL.pending = false;
    PL.underrun = false;
    PL.shown_size = 0;
    PL.osd_until = HAL_GetTick() + 2500;
    g_vtitle = title;
    g_title_until = HAL_GetTick() + 3000;
    bool title_on = true;
    uint32_t afb = sound ? audio_frame_bytes(&a) : 0;
    g_keys = 0;
    avi_t au = a;                                   /* the sound's cursor */
    bool au_end = false, vid_end = false;

#ifdef VID_DEBUG
    uint32_t dbg_t = HAL_GetTick(), dbg_it = 0;
#endif
    for (;;) {
        wdog_refresh();
#ifdef VID_DEBUG
        dbg_it++;
        if (HAL_GetTick() - dbg_t > 500) {
            dbg_t = HAL_GetTick();
            dbg("t=%lu it=%lu pos=%lu frame=%lu pend=%d/%lu shown=%lu clock=%lu fill=%d pushed=%lu under=%d keys=%lx\n",
                (unsigned long)dbg_t, (unsigned long)dbg_it, (unsigned long)a.pos, (unsigned long)a.frame, PL.pending,
                (unsigned long)PL.pend_frame, (unsigned long)PL.shown_frame, (unsigned long)(clock_us() / 1000), ring_fill(),
                (unsigned long)g_pushed, PL.underrun, (unsigned long)g_keys);
            dbg_flush();
        }
#endif
        if (sound) pcm_audio_set(common_emu_sound_get_volume(), !PL.paused);

        /* ---- keys */
        poll_keys();
        uint32_t k = g_keys;
        g_keys = 0;
        if (k & K_B) break;
        if (k & K_REDRAW) redraw_shown();             /* back from the Retro-Go menu */
        if (fm_overlay_expired() && PL.paused) redraw_shown();   /* indicator gone */
        if (title_on && !title_visible()) { title_on = false; if (PL.paused) redraw_shown(); }
        if (k & K_A) {
            if (!PL.paused) { PL.base_us = clock_us(); PL.paused = true; }
            else { PL.paused = false; PL.tick0 = HAL_GetTick(); PL.underrun = false;
                   if (sound) { PL.base_us -= (uint64_t)(g_pushed - (uint64_t)ring_fill()) * 1000000 / g_out_rate; } }
            PL.osd_until = HAL_GetTick() + 2000;
            redraw_shown();
        }
        int seek_s = 0;
        if (k & K_LEFT)  seek_s = -10;
        if (k & K_RIGHT) seek_s = 10;
        if (k & K_DOWN)  seek_s = -60;
        if (k & K_UP)    seek_s = 60;
        if (seek_s) {
            uint32_t from = PL.pending ? PL.pend_frame : PL.shown_frame;
            int64_t t = (int64_t)from + (int64_t)seek_s * 1000000 / a.us_per_frame;
            if (t < 0) t = 0;
            if (a.total_frames && t >= a.total_frames) t = a.total_frames - 1;
            PL.pending = false;
            if (sound) ring_reset();
            mp3_reset();
            avi_seek(&a, (uint32_t)t, first_movi_end);
            au = a;
            au_end = vid_end = false;
#ifdef VID_DEBUG
            dbg("seek %ld -> frame %lu asamples %lu pos %lu\n", (long)t, (unsigned long)a.frame, (unsigned long)a.asamples, (unsigned long)a.pos);
#endif
            /* the clock restarts at the audio position reached (sound) or at the frame (no sound) */
            /* (MP3: from the frame, as the byte rate in the header is often missing or rough) */
            PL.base_us = sound && !a.a_mp3 ? a.asamples * 1000000 / a.a_rate : (uint64_t)a.frame * a.us_per_frame;
            PL.tick0 = HAL_GetTick();
            PL.underrun = false;
            PL.osd_until = HAL_GetTick() + 2000;
            if (PL.paused) {                            /* show where we are */
                PL.paused = false;
                PL.base_us = (uint64_t)a.frame * a.us_per_frame;
            }
        }
        if (PL.paused) { HAL_Delay(10); continue; }

        try_show();

        /* Two cursors over the chunks: the sound one reads ahead while there is room in the
         * ring (so the speaker never runs dry while a frame waits for its time), the picture
         * one takes the next frame once the previous one is shown. */
        bool busy = false;

        /* ---- sound */
        while (sound && !au_end && !(g_keys & URGENT_KEYS)) {
            poll_keys();                            /* a short press must not slip between polls */
            uint32_t save_pos = au.pos, save_end = au.movi_end;
            int kind; uint32_t size, data;
            if (!next_chunk(&au, &kind, &size, &data)) {
                au_end = true;
                if (au.a_mp3) mp3_decode_acc(true);
                break;
            }
            if (kind != 1 || (!afb && !au.a_mp3)) continue;
            uint32_t need = (uint32_t)((uint64_t)(chunk_samples(&au, size) + (au.a_mp3 ? 1152 : 0)) * g_out_rate / au.a_rate) + 2;
            if (need > RING_SAMPLES / 2) need = RING_SAMPLES / 2;
            if ((uint32_t)ring_free() < need) { au.pos = save_pos; au.movi_end = save_end; break; }   /* full enough */
            busy = true;
            if (au.a_mp3) {
                mp3_feed(&au, data, size);
                au.asamples += chunk_samples(&au, size);
                continue;
            }
            uint8_t *tmp = PL.frame_buf + FRAME_MAX - 16384;   /* frames never reach the buffer's tail */
            uint32_t left = size, off = data;
            while (left) {
                uint32_t n = left > 16384 ? 16384 : left;
                n -= n % afb;
                if (!n || !rd(&au, off, tmp, n)) break;
                push_pcm(&au, tmp, n / afb);
                au.asamples += n / afb;
                off += n; left -= n;
                if (g_keys & URGENT_KEYS) break;
            }
        }
        try_show();

        /* ---- picture */
        while (!PL.pending && !vid_end && !(g_keys & URGENT_KEYS)) {
            poll_keys();
            uint32_t save_pos = a.pos;
            int kind; uint32_t size, data;
            if (!next_chunk(&a, &kind, &size, &data)) { vid_end = true; break; }
            if (kind != 0) continue;
            busy = true;
            ckpt_record(&a, save_pos);
            uint32_t fno = a.frame++;
            uint64_t due = (uint64_t)fno * a.us_per_frame;
            if (clock_us() > due + a.us_per_frame) continue;           /* late: drop it */
            if (size == 0 || size > FRAME_MAX - 16384 || !rd(&a, data, PL.frame_buf, size)) continue;
            if (!frame_fits(PL.frame_buf, size)) continue;
            PL.pending = true;
            PL.pend_frame = fno; PL.pend_pos = data; PL.pend_size = size;
            try_show();
        }

        /* ---- end: the last frame shown and the sound played out */
        if (vid_end && (!sound || au_end)) {
            if (PL.pending && sound && ring_fill() == 0) show_pending();
            if (!PL.pending && (!sound || ring_fill() == 0)) break;
        }
        if (!busy) HAL_Delay(1);
    }

    fm_menu_hook = NULL;
    g_vtitle = NULL;
    fm_overlay_clear();
    if (sound) {
        pcm_audio_enable(0);
        pcm_audio_set(0, 0);
        audio_stop_playing();
    }
    JPEG_DecodeDeInit();
    fclose(a.f);
}
