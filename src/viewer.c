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

