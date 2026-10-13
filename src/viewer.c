/*
 * File Manager — viewers: text, hexadecimal, BMP images (e.g. the firmware's screenshots).
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "rg_storage.h"
#include "fm.h"

extern void wdog_refresh(void);

static bool ext_is(const char *name, const char *const *list)
{
    const char *dot = strrchr(name, '.');
    if (!dot)
        return false;
    for (; *list; list++)
        if (strcasecmp(dot + 1, *list) == 0)
            return true;
    return false;
}

bool is_text_ext(const char *name)
{
    static const char *const L[] = { "txt", "cfg", "ini", "log", "md", "json", "xml", "csv", "nfo",
                                     "c", "h", "py", "sh", "lua", "p8", "cue", "m3u", "dat", "pceplus",
                                     "ggcodes", "mcf", "yml", "yaml", "htm", "html", NULL };
    return ext_is(name, L);
}

bool is_bmp_ext(const char *name)
{
    static const char *const L[] = { "bmp", NULL };
    return ext_is(name, L);
}

/* ================================================================ text */

#define TEXT_ROWS (FM_ROWS - 2)
#define MAX_LINES 8192
static uint32_t g_lines[MAX_LINES];   /* start offset of every wrapped line */

void view_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ui_message(path_base(path), tr(T_ERROR), NULL, NULL, false);
        return;
    }
    size_t len = fread(fm_scratch, 1, FM_SCRATCH_SIZE - 1, f);
    bool truncated = fread(fm_scratch + len, 1, 1, f) == 1;
    fclose(f);
    char *t = (char *)fm_scratch;
    t[len] = '\0';

    /* Wrap at FM_COLS columns; \n, \r\n and \r end lines; tabs count as 4 spaces */
    int nlines = 0;
    size_t i = 0;
    while (i <= len && nlines < MAX_LINES) {
        g_lines[nlines++] = (uint32_t)i;
        int col = 0;
        while (i < len && t[i] != '\n' && t[i] != '\r' && col < FM_COLS) {
            col += (t[i] == '\t') ? 4 - (col & 3) : 1;
            i++;
        }
        if (i >= len) break;
        if (t[i] == '\r') { i++; if (i < len && t[i] == '\n') i++; }
        else if (t[i] == '\n') i++;
        if ((nlines & 255) == 0) wdog_refresh();
    }

    int top = 0;
    for (;;) {
        ui_begin();
        char hdr[64];
        snprintf(hdr, sizeof hdr, "%s%s", path_base(path), truncated ? " (...)" : "");
        ui_text_w(0, 0, FM_COLS - 12, hdr, C_BAR_TEXT, C_BAR);
        char pos[16];
        snprintf(pos, sizeof pos, "%d/%d", nlines ? top + 1 : 0, nlines);
        ui_text_w(FM_COLS - 12, 0, 12, pos, C_BAR_TEXT, C_BAR);
        for (int r = 0; r < TEXT_ROWS && top + r < nlines; r++) {
            char line[FM_COLS + 1];
            uint32_t s = g_lines[top + r];
            uint32_t e = (top + r + 1 < nlines) ? g_lines[top + r + 1] : len;
            int c = 0;
            for (uint32_t k = s; k < e && c < FM_COLS; k++) {
                char ch = t[k];
                if (ch == '\r' || ch == '\n') break;
                if (ch == '\t') { do line[c++] = ' '; while ((c & 3) && c < FM_COLS); continue; }
                line[c++] = ((uint8_t)ch < 0x20 || (uint8_t)ch > 0x7e) ? '.' : ch;
            }
            line[c] = '\0';
            ui_text(0, 1 + r, line, C_TEXT, C_BG);
        }
        ui_text_w(0, FM_ROWS - 1, FM_COLS, tr(T_VIEW_HELP), C_BAR_TEXT, C_BAR);
        ui_end();

        uint32_t k = input_wait();
        int max_top = nlines > TEXT_ROWS ? nlines - TEXT_ROWS : 0;
        if (k & K_UP)    top--;
        if (k & K_DOWN)  top++;
        if (k & K_LEFT)  top -= TEXT_ROWS;
        if (k & K_RIGHT) top += TEXT_ROWS;
        if (top > max_top) top = max_top;
        if (top < 0) top = 0;
        if (k & (K_B | K_PAUSE)) return;
    }
}

/* ================================================================ hex */

#define HEX_PER_ROW 8

void view_hex(const char *path)
{
    rg_stat_t st = rg_storage_stat(path);
    FILE *f = fopen(path, "rb");
    if (!f) {
        ui_message(path_base(path), tr(T_ERROR), NULL, NULL, false);
        return;
    }
    uint64_t size = st.size;
    uint64_t rows = (size + HEX_PER_ROW - 1) / HEX_PER_ROW;
    uint64_t top = 0;
    for (;;) {
        uint8_t buf[TEXT_ROWS * HEX_PER_ROW];
        fseek(f, (long)(top * HEX_PER_ROW), SEEK_SET);
        size_t got = fread(buf, 1, sizeof buf, f);

        ui_begin();
        char hdr[64], sz[16];
        fmt_size(sz, size);
        ui_text_w(0, 0, FM_COLS - 10, path_base(path), C_BAR_TEXT, C_BAR);
        ui_text_w(FM_COLS - 10, 0, 10, sz, C_BAR_TEXT, C_BAR);
        for (int r = 0; r < TEXT_ROWS; r++) {
            size_t o = (size_t)r * HEX_PER_ROW;
            if (o >= got) break;
            char line[FM_COLS + 1];
            int p = snprintf(line, sizeof line, "%06lX ", (unsigned long)((top + r) * HEX_PER_ROW));
            for (int b = 0; b < HEX_PER_ROW; b++)
                p += (o + b < got) ? snprintf(line + p, sizeof line - p, "%02X ", buf[o + b])
                                   : snprintf(line + p, sizeof line - p, "   ");
            for (int b = 0; b < HEX_PER_ROW && o + b < got; b++) {
                uint8_t ch = buf[o + b];
                line[p++] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '.';
            }
            line[p] = '\0';
            ui_text(0, 1 + r, line, C_TEXT, C_BG);
        }
        snprintf(hdr, sizeof hdr, "%s", tr(T_VIEW_HELP));
        ui_text_w(0, FM_ROWS - 1, FM_COLS, hdr, C_BAR_TEXT, C_BAR);
        ui_end();

        uint32_t k = input_wait();
        uint64_t max_top = rows > TEXT_ROWS ? rows - TEXT_ROWS : 0;
        if ((k & K_UP) && top > 0) top--;
        if (k & K_DOWN) top++;
        if (k & K_LEFT) top = top > TEXT_ROWS ? top - TEXT_ROWS : 0;
        if (k & K_RIGHT) top += TEXT_ROWS;
        if (top > max_top) top = max_top;
        if (k & (K_B | K_PAUSE)) break;
    }
    fclose(f);
}

/* ================================================================ BMP */

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int mask_shift(uint32_t m) { int s = 0; if (!m) return 0; while (!(m & 1)) { m >>= 1; s++; } return s; }
static int mask_bits(uint32_t m)  { int b = 0; m >>= mask_shift(m); while (m & 1) { m >>= 1; b++; } return b; }

bool view_bmp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    uint8_t h[138];
    memset(h, 0, sizeof h);
    size_t got = fread(h, 1, sizeof h, f);
    if (got < 54 || h[0] != 'B' || h[1] != 'M') { fclose(f); return false; }
    uint32_t data_off = rd32(h + 10);
    uint32_t dib = rd32(h + 14);
    int32_t w = (int32_t)rd32(h + 18), hgt = (int32_t)rd32(h + 22);
    uint16_t bpp = rd16(h + 28);
    uint32_t comp = rd32(h + 30);
    uint32_t ncolors = rd32(h + 46);
    bool top_down = hgt < 0;
    if (top_down) hgt = -hgt;
    if (w <= 0 || hgt <= 0 || w > 8192 || hgt > 8192 || (comp != 0 && comp != 3) ||
        (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32)) {
        fclose(f);
        return false;
    }
    uint32_t rm, gm, bm;
    if (comp == 3) {                      /* BI_BITFIELDS: masks after the 40-byte header */
        rm = rd32(h + 54); gm = rd32(h + 58); bm = rd32(h + 62);
    } else if (bpp == 16) { rm = 0x7C00; gm = 0x03E0; bm = 0x001F; }
    else { rm = 0xFF0000; gm = 0x00FF00; bm = 0x0000FF; }
    int rs = mask_shift(rm), gs = mask_shift(gm), bs = mask_shift(bm);
    int rb = mask_bits(rm), gb = mask_bits(gm), bb = mask_bits(bm);

    /* 8-bit palette, converted to RGB565 */
    static uint16_t pal[256];
    if (bpp == 8) {
        if (!ncolors || ncolors > 256) ncolors = 256;
        uint8_t q[4 * 256];
        fseek(f, 14 + (long)dib, SEEK_SET);
        if (fread(q, 4, ncolors, f) != ncolors) { fclose(f); return false; }
        for (uint32_t i = 0; i < ncolors; i++)
            pal[i] = (uint16_t)(((q[4 * i + 2] >> 3) << 11) | ((q[4 * i + 1] >> 2) << 5) | (q[4 * i] >> 3));
    }

    uint32_t stride = ((uint32_t)w * bpp / 8 + 3) & ~3u;
    if (stride > FM_SCRATCH_SIZE) { fclose(f); return false; }

    /* Fit inside the LCD keeping the aspect ratio (never enlarge) */
    int dw = w, dh = hgt;
    if (dw > FM_W) { dh = (int)((int64_t)dh * FM_W / dw); dw = FM_W; }
    if (dh > FM_H) { dw = (int)((int64_t)dw * FM_H / dh); dh = FM_H; }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    int ox = (FM_W - dw) / 2, oy = (FM_H - dh) / 2;

    ui_begin();
    uint16_t *fb = ui_buffer();
    int last_sy = -1;
    for (int y = 0; y < dh; y++) {
        int sy = (int)((int64_t)y * hgt / dh);
        if (sy != last_sy) {
            int file_row = top_down ? sy : hgt - 1 - sy;
            fseek(f, (long)(data_off + (uint64_t)file_row * stride), SEEK_SET);
            if (fread(fm_scratch, 1, stride, f) != stride) break;
            last_sy = sy;
        }
        uint16_t *d = fb + (oy + y) * FM_W + ox;
        for (int x = 0; x < dw; x++) {
            int sx = (int)((int64_t)x * w / dw);
            uint16_t px;
            if (bpp == 8) px = pal[fm_scratch[sx]];
            else {
                uint32_t v;
                const uint8_t *s = fm_scratch + (uint32_t)sx * (bpp / 8);
                v = (bpp == 16) ? rd16(s) : (bpp == 24) ? (uint32_t)(s[0] | s[1] << 8 | s[2] << 16) : rd32(s);
                uint32_t r = (v & rm) >> rs, g = (v & gm) >> gs, b = (v & bm) >> bs;
                /* scale each channel to 5/6/5 bits */
                r = rb >= 5 ? r >> (rb - 5) : r << (5 - rb);
                g = gb >= 6 ? g >> (gb - 6) : g << (6 - gb);
                b = bb >= 5 ? b >> (bb - 5) : b << (5 - bb);
                px = (uint16_t)((r << 11) | (g << 5) | b);
            }
            d[x] = px;
        }
        if ((y & 15) == 0) wdog_refresh();
    }
    fclose(f);
    ui_end();

    /* Any key returns; the image stays clean (no bars over it) */
    input_wait();
    return true;
}
