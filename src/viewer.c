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

bool view_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ui_message(path_base(path), tr(T_ERROR), NULL, NULL, false);
        return false;
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
            col += (t[i] == '\t') ? 4 - (col & 3) : (((uint8_t)t[i] & 0xC0) == 0x80) ? 0 : 1;
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
            char line[FM_COLS * 4 + 1];        /* UTF-8 is passed through: ui_text folds it */
            uint32_t s = g_lines[top + r];
            uint32_t e = (top + r + 1 < nlines) ? g_lines[top + r + 1] : len;
            int c = 0, o = 0;
            for (uint32_t k = s; k < e && c < FM_COLS && o < FM_COLS * 4 - 4; k++) {
                char ch = t[k];
                if (ch == '\r' || ch == '\n') break;
                if (ch == '\t') { do { line[o++] = ' '; c++; } while ((c & 3) && c < FM_COLS); continue; }
                if ((uint8_t)ch >= 0x80) {
                    if (((uint8_t)ch & 0xC0) != 0x80) c++;
                    line[o++] = ch;
                    continue;
                }
                line[o++] = ((uint8_t)ch < 0x20 || ch == 0x7f) ? '.' : ch;
                c++;
            }
            line[o] = '\0';
            ui_text(0, 1 + r, line, C_TEXT, C_BG);
        }
        ui_text_w(0, FM_ROWS - 1, FM_COLS, tr(T_TEXT_HELP), C_BAR_TEXT, C_BAR);
        ui_end();

        uint32_t k = input_wait();
        if (k & K_A)        /* edit: the editor reloads the file into the same scratch buffer */
            return edit_text(path);
        int max_top = nlines > TEXT_ROWS ? nlines - TEXT_ROWS : 0;
        if (k & K_UP)    top--;
        if (k & K_DOWN)  top++;
        if (k & K_LEFT)  top -= TEXT_ROWS;
        if (k & K_RIGHT) top += TEXT_ROWS;
        if (top > max_top) top = max_top;
        if (top < 0) top = 0;
        if (k & (K_B | K_PAUSE)) return false;
    }
}

/* ================================================================ hex view / edit */

#define HEX_PER_ROW 8
#define HEX_COL     7                     /* "XXXXXX " offset column */
#define ASC_COL     (HEX_COL + 3 * HEX_PER_ROW)
#define MAX_EDITS   2048                  /* pending byte changes before a save is needed */

/* Pending changes (overwrite only: the file never grows or shrinks) */
static uint32_t g_ed_off[MAX_EDITS];
static uint8_t  g_ed_val[MAX_EDITS], g_ed_orig[MAX_EDITS];
static int      g_ed_n;

static int ed_find(uint32_t off)
{
    for (int i = 0; i < g_ed_n; i++)
        if (g_ed_off[i] == off) return i;
    return -1;
}

/* false = no room left */
static bool ed_set(uint32_t off, uint8_t orig, uint8_t val)
{
    int i = ed_find(off);
    if (i >= 0) {
        if (val == g_ed_orig[i]) {        /* back to the original value: forget it */
            g_ed_n--;
            g_ed_off[i] = g_ed_off[g_ed_n]; g_ed_val[i] = g_ed_val[g_ed_n]; g_ed_orig[i] = g_ed_orig[g_ed_n];
        } else {
            g_ed_val[i] = val;
        }
        return true;
    }
    if (val == orig) return true;
    if (g_ed_n >= MAX_EDITS) return false;
    g_ed_off[g_ed_n] = off; g_ed_orig[g_ed_n] = orig; g_ed_val[g_ed_n] = val;
    g_ed_n++;
    return true;
}

/* Write the pending changes in place. The read handle is closed and reopened around it. */
static bool ed_save(const char *path, FILE **f)
{
    fclose(*f);
    *f = NULL;
    bool ok = false;
    FILE *w = fopen(path, "r+b");
    if (w) {
        ok = true;
        for (int i = 0; i < g_ed_n && ok; i++) {
            if (fseek(w, (long)g_ed_off[i], SEEK_SET) != 0 || fwrite(&g_ed_val[i], 1, 1, w) != 1)
                ok = false;
            if ((i & 63) == 0) wdog_refresh();
        }
        if (fclose(w) != 0) ok = false;
    }
    if (ok) g_ed_n = 0;
    *f = fopen(path, "rb");
    return ok && *f;
}

static void hex_digit(char *out, uint8_t v) { static const char H[] = "0123456789ABCDEF"; out[0] = H[v >> 4]; out[1] = H[v & 15]; out[2] = 0; }

/* byte as it is in the file (pending changes not applied) */
static uint8_t file_byte(FILE *f, uint64_t off)
{
    uint8_t b = 0;
    if (f && fseek(f, (long)off, SEEK_SET) == 0 && fread(&b, 1, 1, f) == 1)
        return b;
    return 0;
}
/* byte as the user sees it (pending change if any) */
static uint8_t cur_byte(FILE *f, uint64_t off)
{
    int i = ed_find((uint32_t)off);
    return i >= 0 ? g_ed_val[i] : file_byte(f, off);
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

void view_hex(const char *path)
{
    rg_stat_t st = rg_storage_stat(path);
    FILE *f = fopen(path, "rb");
    if (!f) {
        ui_message(path_base(path), tr(T_ERROR), NULL, NULL, false);
        return;
    }
    uint64_t size = st.size;
    uint64_t total_rows = (size + HEX_PER_ROW - 1) / HEX_PER_ROW;
    uint64_t top = 0;
    bool editing = false;                 /* edit mode */
    bool kb = true;                       /* keyboard shown (else D-pad moves the cursor) */
    bool ascii = false;                   /* cursor in the text column instead of the hex one */
    uint64_t cur = 0;                     /* cursor (byte offset) */
    int nib = 0;                          /* 0 = high digit, 1 = low digit */
    g_ed_n = 0;

    for (;;) {
        int rows = (editing && kb) ? (kb_top() - 8) / 8 : TEXT_ROWS;
        uint64_t max_top = total_rows > (uint64_t)rows ? total_rows - rows : 0;
        if (editing) {
            uint64_t row = cur / HEX_PER_ROW;
            if (row < top) top = row;
            if (row >= top + rows) top = row - rows + 1;
        }
        if (top > max_top) top = max_top;

        uint8_t buf[TEXT_ROWS * HEX_PER_ROW];
        size_t got = 0;
        if (f && fseek(f, (long)(top * HEX_PER_ROW), SEEK_SET) == 0)
            got = fread(buf, 1, (size_t)rows * HEX_PER_ROW, f);
        bool changed[sizeof buf];
        memset(changed, 0, sizeof changed);
        for (int i = 0; i < g_ed_n; i++) {
            uint64_t o = g_ed_off[i];
            if (o >= top * HEX_PER_ROW && o < top * HEX_PER_ROW + got) {
                buf[o - top * HEX_PER_ROW] = g_ed_val[i];
                changed[o - top * HEX_PER_ROW] = true;
            }
        }

        ui_begin();
        char hdr[48], sz[24];
        if (editing) {
            snprintf(hdr, sizeof hdr, "%s%s", g_ed_n ? "*" : "", path_base(path));
            snprintf(sz, sizeof sz, "%s @%06lX", ascii ? "TXT" : "HEX", (unsigned long)cur);
            ui_text_w(0, 0, FM_COLS - 14, hdr, C_BAR_TEXT, C_BAR);
            ui_text_w(FM_COLS - 14, 0, 14, sz, C_BAR_TEXT, C_BAR);
        } else {
            fmt_size(sz, size);
            ui_text_w(0, 0, FM_COLS - 10, path_base(path), C_BAR_TEXT, C_BAR);
            ui_text_w(FM_COLS - 10, 0, 10, sz, C_BAR_TEXT, C_BAR);
        }
        for (int r = 0; r < rows; r++) {
            size_t o = (size_t)r * HEX_PER_ROW;
            if (o >= got) break;
            char t[12];
            snprintf(t, sizeof t, "%06lX", (unsigned long)((top + r) * HEX_PER_ROW));
            ui_text(0, 1 + r, t, C_DIM, C_BG);
            for (int b = 0; b < HEX_PER_ROW && o + b < got; b++) {
                size_t k = o + b;
                bool sel = editing && (top * HEX_PER_ROW + k == cur);
                uint16_t fg = changed[k] ? C_MARK : C_TEXT;
                hex_digit(t, buf[k]);
                char c[2] = { (buf[k] >= 0x20 && buf[k] < 0x7f) ? (char)buf[k] : '.', 0 };
                if (!sel) {
                    ui_text(HEX_COL + 3 * b, 1 + r, t, fg, C_BG);
                    ui_text(ASC_COL + b, 1 + r, c, fg, C_BG);
                    continue;
                }
                /* cursor: the active column in orange (active digit in red), the other one dim */
                if (ascii) {
                    ui_text(HEX_COL + 3 * b, 1 + r, t, C_BAR_TEXT, C_BAR);
                    ui_text(ASC_COL + b, 1 + r, c, C_SEL_TEXT, C_SEL_BG);
                } else {
                    char d[2] = { t[nib], 0 }, e[2] = { t[!nib], 0 };
                    ui_text(HEX_COL + 3 * b + nib, 1 + r, d, C_BAR_TEXT, C_WARN);
                    ui_text(HEX_COL + 3 * b + !nib, 1 + r, e, C_SEL_TEXT, C_SEL_BG);
                    ui_text(ASC_COL + b, 1 + r, c, C_BAR_TEXT, C_BAR);
                }
            }
        }
        if (editing && kb) kb_draw();
        ui_text_w(0, FM_ROWS - 1, FM_COLS, tr(!editing ? T_HEX_HELP : kb ? T_ED_KB_HELP : T_ED_CUR_HELP),
                  C_BAR_TEXT, C_BAR);
        ui_end();

        uint32_t k = input_wait();

        if (!editing) {
            if ((k & K_UP) && top > 0) top--;
            if (k & K_DOWN) top++;
            if (k & K_LEFT) top = top > (uint64_t)rows ? top - rows : 0;
            if (k & K_RIGHT) top += rows;
            if ((k & K_A) && size > 0) {
                editing = true;
                kb = true;
                ascii = false;
                nib = 0;
                cur = top * HEX_PER_ROW;
                kb_set_layout(KB_HEX);
            }
            if (k & (K_B | K_PAUSE)) break;
            continue;
        }

        /* ---- editing: turn the input into one action */
        int act = -2;                     /* -2 nothing, 0 character, else KC_* */
        char ch = 0;
        if (k & K_TIME) kb = !kb;
        if (k & K_B) act = KC_BKSP;
        if (kb) {
            kb_nav(k);
            if (k & K_A) act = kb_activate(&ch);
        } else {
            if (k & K_LEFT)  act = KC_LEFT;
            if (k & K_RIGHT) act = KC_RIGHT;
            if (k & K_UP)    act = KC_UP;
            if (k & K_DOWN)  act = KC_DOWN;
            if (k & K_A)     kb = true;
        }

        bool full = false;
        switch (act) {
            case 0:
                if (ascii) {
                    full = !ed_set((uint32_t)cur, file_byte(f, cur), (uint8_t)ch);
                    if (!full && cur + 1 < size) cur++;
                } else if (hex_val(ch) >= 0) {
                    uint8_t v = cur_byte(f, cur);
                    int sh = nib ? 0 : 4;
                    v = (uint8_t)((v & ~(15 << sh)) | (hex_val(ch) << sh));
                    full = !ed_set((uint32_t)cur, file_byte(f, cur), v);
                    if (!full) {
                        if (nib == 0) nib = 1;
                        else if (cur + 1 < size) { cur++; nib = 0; }
                    }
                }
                break;
            case KC_SPACE: case KC_TAB:
                if (ascii) {
                    full = !ed_set((uint32_t)cur, file_byte(f, cur), act == KC_SPACE ? ' ' : '\t');
                    if (!full && cur + 1 < size) cur++;
                }
                break;
            case KC_INC: case KC_DEC: {
                uint8_t v = (uint8_t)(cur_byte(f, cur) + (act == KC_INC ? 1 : -1));
                full = !ed_set((uint32_t)cur, file_byte(f, cur), v);
                break;
            }
            case KC_BKSP:                 /* step back and put the original value back */
                if (ascii || nib == 0) {
                    if (ascii && cur == 0) break;
                    if (!ascii && cur == 0) break;
                    cur--;
                    if (!ascii) nib = 1;
                }
                else nib = 0;
                if (ascii) {
                    ed_set((uint32_t)cur, file_byte(f, cur), file_byte(f, cur));
                } else {
                    uint8_t o = file_byte(f, cur), v = cur_byte(f, cur);
                    uint8_t m = nib ? 0x0F : 0xF0;
                    ed_set((uint32_t)cur, o, (uint8_t)((v & ~m) | (o & m)));
                }
                break;
            case KC_DEL:
                ed_set((uint32_t)cur, file_byte(f, cur), file_byte(f, cur));
                break;
            case KC_LEFT:
                if (!ascii && nib == 1) nib = 0;
                else if (cur > 0) { cur--; nib = ascii ? 0 : 1; }
                break;
            case KC_RIGHT:
                if (!ascii && nib == 0) nib = 1;
                else if (cur + 1 < size) { cur++; nib = 0; }
                break;
            case KC_UP:   if (cur >= HEX_PER_ROW) cur -= HEX_PER_ROW; break;
            case KC_DOWN: if (cur + HEX_PER_ROW < size) cur += HEX_PER_ROW; break;
            case KC_PGUP: cur = cur >= (uint64_t)rows * HEX_PER_ROW ? cur - (uint64_t)rows * HEX_PER_ROW : cur % HEX_PER_ROW; break;
            case KC_PGDN:
                if (cur + (uint64_t)rows * HEX_PER_ROW < size) cur += (uint64_t)rows * HEX_PER_ROW;
                else while (cur + HEX_PER_ROW < size) cur += HEX_PER_ROW;
                break;
            case KC_SWAP:
                ascii = !ascii;
                nib = 0;
                kb_set_layout(ascii ? KB_ASCII : KB_HEX);
                break;
        }
        if (full)
            ui_message(path_base(path), tr(T_TOO_MANY), NULL, NULL, false);

        if (k & K_GAME) {
            char l[48];
            snprintf(l, sizeof l, "%s: %d", tr(T_CHANGES), g_ed_n);
            int a = g_ed_n ? ui_choice(tr(T_SAVE_CHANGES), l, tr(T_SAVE), tr(T_ED_SAVE_EXIT), tr(T_ED_EXIT_NOSAVE))
                           : ui_choice(tr(T_VIEW_HEX), path_base(path), tr(T_ED_EXIT), NULL, NULL);
            if (!g_ed_n) {
                if (a == 1) editing = false;
            } else if (a == 1 || a == 2) {
                if (!ed_save(path, &f)) {
                    ui_message(path_base(path), tr(T_ERROR), NULL, NULL, false);
                    if (!f) return;
                } else if (a == 2) {
                    editing = false;
                }
            } else if (a == 3) {
                if (ui_message(tr(T_ED_EXIT_NOSAVE), tr(T_ED_LOSE), NULL, NULL, true)) {
                    g_ed_n = 0;
                    editing = false;
                }
            }
        }
    }
    if (f) fclose(f);
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
