/*
 * File Manager — text editor with an on-screen keyboard docked at the bottom
 * (same idea as gw-dos-xtulator's keyboard: variable-width keys, Shift latches).
 *
 *   Keyboard shown:  D-pad picks a key, A presses it, B = backspace,
 *                    TIME hides the keyboard (cursor mode), GAME = menu (save / exit)
 *   Cursor mode:     D-pad moves the text cursor, A or TIME shows the keyboard,
 *                    B = backspace, GAME = menu
 *
 * The text lives in a gap buffer inside fm_scratch. Lines are not wrapped: the view scrolls
 * sideways like nano. CRLF files are edited as LF and saved back as CRLF. Non-ASCII UTF-8 is
 * kept untouched (shown folded to ASCII by the 8x8 font) and the cursor never splits it.
 */
#include <stdio.h>
#include <string.h>
#include "rg_storage.h"
#include "fm.h"

extern void wdog_refresh(void);
extern bool fm_lang_es(void);

#define ED_MAX_FILE   (FM_SCRATCH_SIZE - 32 * 1024)   /* keep at least 32 KB to type into */

/* ---------------------------------------------------------------- gap buffer */
static char  *G;
static size_t g_cap, g_gs, g_ge;          /* gap = [g_gs, g_ge) */
static size_t ed_len(void) { return g_cap - (g_ge - g_gs); }
static inline char at(size_t i) { return i < g_gs ? G[i] : G[i + (g_ge - g_gs)]; }
static inline bool cont(char c) { return ((uint8_t)c & 0xC0) == 0x80; }

static void gap_move(size_t p)
{
    if (p < g_gs) {
        size_t n = g_gs - p;
        memmove(G + g_ge - n, G + p, n);
        g_gs -= n; g_ge -= n;
    } else if (p > g_gs) {
        size_t n = p - g_gs;
        memmove(G + g_gs, G + g_ge, n);
        g_gs += n; g_ge += n;
    }
}

static size_t next_cp(size_t p) { size_t n = ed_len(); if (p < n) p++; while (p < n && cont(at(p))) p++; return p; }
static size_t prev_cp(size_t p) { if (p > 0) p--; while (p > 0 && cont(at(p))) p--; return p; }
static size_t line_start(size_t p) { while (p > 0 && at(p - 1) != '\n') p--; return p; }
static size_t line_end(size_t p) { size_t n = ed_len(); while (p < n && at(p) != '\n') p++; return p; }

/* display column of p (tabs to multiples of 4, one column per code point) */
static int col_of(size_t p)
{
    int c = 0;
    for (size_t i = line_start(p); i < p; i = next_cp(i))
        c += at(i) == '\t' ? 4 - (c & 3) : 1;
    return c;
}

/* position on the line starting at ls closest to display column want */
static size_t pos_at_col(size_t ls, int want)
{
    size_t n = ed_len(), p = ls;
    int c = 0;
    while (p < n && at(p) != '\n') {
        int w = at(p) == '\t' ? 4 - (c & 3) : 1;
        if (c + w > want) break;
        c += w;
        p = next_cp(p);
    }
    return p;
}

/* ---------------------------------------------------------------- state */
static size_t g_cur, g_top;               /* cursor, first byte of the first visible line */
static int    g_hs;                       /* first visible column */
static int    g_want = -1;                /* column kept while moving up/down */
static bool   g_dirty, g_crlf;

static bool ins(char c)
{
    if (g_gs == g_ge) return false;
    gap_move(g_cur);
    G[g_gs++] = c;
    g_cur++;
    g_dirty = true;
    return true;
}

static void del_back(void)
{
    if (!g_cur) return;
    size_t p = prev_cp(g_cur);
    gap_move(g_cur);
    g_gs = p;
    g_cur = p;
    g_dirty = true;
}

static void del_fwd(void)
{
    size_t q = next_cp(g_cur);
    if (q == g_cur) return;
    gap_move(g_cur);
    g_ge += q - g_cur;
    g_dirty = true;
}

static void move_lines(int d)
{
    if (g_want < 0) g_want = col_of(g_cur);
    size_t ls = line_start(g_cur);
    for (; d < 0; d++) {
        if (ls == 0) { g_cur = 0; return; }
        ls = line_start(ls - 1);
    }
    for (; d > 0; d--) {
        size_t le = line_end(ls);
        if (le >= ed_len()) { g_cur = le; return; }
        ls = le + 1;
    }
    g_cur = pos_at_col(ls, g_want);
}

/* ---------------------------------------------------------------- keyboard */
enum { KC_BKSP = 1, KC_DEL, KC_ENTER, KC_SHIFT, KC_TAB, KC_SPACE, KC_LEFT, KC_RIGHT, KC_UP, KC_DOWN,
       KC_HOME, KC_END, KC_PGUP, KC_PGDN };
typedef struct { char lo, up; uint8_t code; const char *en, *es; } osk_key_t;
#define CH(l, u) { l, u, 0, NULL, NULL }
#define SP(c, en, es) { 0, 0, c, en, es }
static const osk_key_t R0[] = { CH('1','!'),CH('2','@'),CH('3','#'),CH('4','$'),CH('5','%'),CH('6','^'),CH('7','&'),
                            CH('8','*'),CH('9','('),CH('0',')'),CH('-','_'),CH('=','+'),SP(KC_BKSP,"Bksp","Borra"),{0} };
static const osk_key_t R1[] = { CH('q','Q'),CH('w','W'),CH('e','E'),CH('r','R'),CH('t','T'),CH('y','Y'),CH('u','U'),
                            CH('i','I'),CH('o','O'),CH('p','P'),CH('[','{'),CH(']','}'),SP(KC_DEL,"Del","Supr"),{0} };
static const osk_key_t R2[] = { CH('a','A'),CH('s','S'),CH('d','D'),CH('f','F'),CH('g','G'),CH('h','H'),CH('j','J'),
                            CH('k','K'),CH('l','L'),CH(';',':'),CH('\'','"'),CH('\\','|'),SP(KC_ENTER,"Enter","Intro"),{0} };
static const osk_key_t R3[] = { SP(KC_SHIFT,"Shift","Mayus"),CH('z','Z'),CH('x','X'),CH('c','C'),CH('v','V'),CH('b','B'),
                            CH('n','N'),CH('m','M'),CH(',','<'),CH('.','>'),CH('/','?'),CH('`','~'),SP(KC_TAB,"Tab","Tab"),{0} };
static const osk_key_t R4[] = { SP(KC_SPACE,"Space","Espacio"),SP(KC_LEFT,"<","<"),SP(KC_RIGHT,">",">"),SP(KC_UP,"^","^"),
                            SP(KC_DOWN,"v","v"),SP(KC_HOME,"Home","Ini"),SP(KC_END,"End","Fin"),
                            SP(KC_PGUP,"PgUp","RePag"),SP(KC_PGDN,"PgDn","AvPag"),{0} };
static const osk_key_t *const ROWS[5] = { R0, R1, R2, R3, R4 };
#define KB_ROWS   5
#define KB_KEY_H  10
#define KB_Y      (FM_H - 8 - KB_ROWS * KB_KEY_H)

static int kb_row = 1, kb_col = 0;
static int g_shift;                      /* 0 off, 1 next key, 2 locked */

static int row_len(int r) { int n = 0; while (ROWS[r][n].lo || ROWS[r][n].code) n++; return n; }
static const char *key_label(const osk_key_t *k, char *buf)
{
    if (k->code) {
        if (k->code == KC_SHIFT && g_shift == 2) return fm_lang_es() ? "MAYUS" : "SHIFT";
        return fm_lang_es() ? k->es : k->en;
    }
    buf[0] = g_shift ? k->up : k->lo;
    buf[1] = 0;
    return buf;
}
static int key_w(const osk_key_t *k) { char b[2]; return (int)strlen(key_label(k, b)) * 8 + 8; }
static int row_w(int r) { int w = 0; for (int i = 0; i < row_len(r); i++) w += key_w(&ROWS[r][i]) + 2; return w - 2; }
static int key_x(int r, int c) { int x = (FM_W - row_w(r)) / 2; for (int i = 0; i < c; i++) x += key_w(&ROWS[r][i]) + 2; return x; }

static void kb_move_row(int d)
{
    int cx = key_x(kb_row, kb_col) + key_w(&ROWS[kb_row][kb_col]) / 2;
    kb_row = (kb_row + d + KB_ROWS) % KB_ROWS;
    int best = 0, bd = 1 << 30;
    for (int c = 0; c < row_len(kb_row); c++) {
        int m = key_x(kb_row, c) + key_w(&ROWS[kb_row][c]) / 2;
        int dd = m > cx ? m - cx : cx - m;
        if (dd < bd) { bd = dd; best = c; }
    }
    kb_col = best;
}

static void kb_draw(void)
{
    ui_fill(0, KB_Y - 2, FM_W, KB_ROWS * KB_KEY_H + 2, 0x2104);
    for (int r = 0; r < KB_ROWS; r++)
        for (int c = 0; c < row_len(r); c++) {
            const osk_key_t *k = &ROWS[r][c];
            char b[2];
            const char *l = key_label(k, b);
            bool sel = (r == kb_row && c == kb_col);
            bool lit = (k->code == KC_SHIFT && g_shift);
            uint16_t bg = sel ? C_SEL_BG : lit ? C_BAR : 0x4208;
            uint16_t fg = sel ? C_SEL_TEXT : k->code ? C_DIR : C_TEXT;
            int x = key_x(r, c), w = key_w(k), y = KB_Y + r * KB_KEY_H;
            ui_fill(x, y, w, KB_KEY_H - 1, bg);
            ui_text_px(x + 4, y + 1, l, fg, bg);
        }
}

/* ---------------------------------------------------------------- drawing */
static void view_fix(int rows)
{
    if (g_cur < g_top) g_top = line_start(g_cur);
    /* count lines from g_top to the cursor; scroll down if it is below the window */
    int n = 0;
    for (size_t i = g_top; i < g_cur; i++)
        if (at(i) == '\n' && ++n >= rows) break;
    if (n >= rows) {
        size_t ls = line_start(g_cur);
        for (int k = 0; k < rows - 1 && ls > 0; k++) ls = line_start(ls - 1);
        g_top = ls;
    }
    int c = col_of(g_cur);
    if (c < g_hs) g_hs = c > 8 ? c - 8 : 0;
    if (c >= g_hs + FM_COLS) g_hs = c - FM_COLS + 9;
}

static void draw(const char *path, bool kb)
{
    int rows = kb ? (KB_Y - 2 - 8) / 8 : FM_ROWS - 2;
    view_fix(rows);

    ui_begin();
    /* title: *name  L:n C:m */
    int line = 1;
    for (size_t i = 0; i < g_cur; i++) if (at(i) == '\n') line++;
    char pos[24], hdr[64];
    snprintf(pos, sizeof pos, "%d:%d", line, col_of(g_cur) + 1);
    snprintf(hdr, sizeof hdr, "%s%s", g_dirty ? "*" : "", path_base(path));
    int pw = (int)strlen(pos) + 1;
    ui_text_w(0, 0, FM_COLS - pw, hdr, C_BAR_TEXT, C_BAR);
    ui_text_w(FM_COLS - pw, 0, pw, pos, C_BAR_TEXT, C_BAR);

    size_t n = ed_len(), p = g_top;
    for (int r = 0; r < rows; r++) {
        char out[FM_COLS * 4 + 1];
        int o = 0, c = 0, cur_col = -1;
        char cur_ch[5] = " ";
        while (p < n && at(p) != '\n') {
            if (p == g_cur) cur_col = c;
            size_t q = next_cp(p);
            int w = at(p) == '\t' ? 4 - (c & 3) : 1;
            for (int k = 0; k < w; k++, c++) {
                if (c < g_hs || c >= g_hs + FM_COLS) continue;
                if (at(p) == '\t' || (uint8_t)at(p) < 0x20) out[o++] = at(p) == '\t' ? ' ' : '.';
                else for (size_t b = p; b < q; b++) out[o++] = at(b);
                if (p == g_cur && k == 0) {
                    size_t m = 0;
                    if (at(p) == '\t' || (uint8_t)at(p) < 0x20) cur_ch[m++] = at(p) == '\t' ? ' ' : '.';
                    else for (size_t b = p; b < q && m < 4; b++) cur_ch[m++] = at(b);
                    cur_ch[m] = 0;
                }
            }
            p = q;
        }
        if (p == g_cur) cur_col = c;        /* cursor at the end of the line */
        out[o] = 0;
        ui_text(0, 1 + r, out, C_TEXT, C_BG);
        if (cur_col >= g_hs && cur_col < g_hs + FM_COLS)
            ui_text(cur_col - g_hs, 1 + r, cur_ch, C_SEL_TEXT, C_SEL_BG);
        if (p >= n) break;                  /* that was the last line */
        p++;                                /* skip '\n' */
    }
    if (kb) kb_draw();
    ui_text_w(0, FM_ROWS - 1, FM_COLS, tr(kb ? T_ED_KB_HELP : T_ED_CUR_HELP), C_BAR_TEXT, C_BAR);
    ui_end();
}

/* ---------------------------------------------------------------- load / save */
static bool ed_load(const char *path)
{
    G = (char *)fm_scratch;
    g_cap = FM_SCRATCH_SIZE;
    size_t len = 0;
    FILE *f = fopen(path, "rb");
    if (f) {
        len = fread(G, 1, ED_MAX_FILE + 1, f);
        fclose(f);
        if (len > ED_MAX_FILE) return false;
    }
    /* CRLF -> LF (remember to write it back) */
    size_t w = 0;
    g_crlf = false;
    for (size_t r = 0; r < len; r++) {
        if (G[r] == '\r' && r + 1 < len && G[r + 1] == '\n') { g_crlf = true; continue; }
        G[w++] = G[r];
    }
    g_gs = w;
    g_ge = g_cap;
    g_cur = g_top = 0;
    g_hs = 0;
    g_want = -1;
    g_dirty = false;
    g_shift = 0;
    return true;
}

static bool write_all(FILE *f)
{
    char buf[512];
    size_t o = 0, n = ed_len();
    for (size_t i = 0; i < n; i++) {
        char c = at(i);
        if (c == '\n' && g_crlf) buf[o++] = '\r';
        buf[o++] = c;
        if (o >= sizeof buf - 2) {
            if (fwrite(buf, 1, o, f) != o) return false;
            o = 0;
            wdog_refresh();
        }
    }
    return o == 0 || fwrite(buf, 1, o, f) == o;
}

/* Write to "<path>.fmtmp" and swap it in, so a failed write never leaves half a file.
 * Without fast rename the swap would be a copy, so write the file directly instead. */
static bool ed_save(const char *path)
{
    char tmp[FM_PATH_MAX + 8];
    bool swap = op_fast_rename_available() && strlen(path) + 7 < sizeof tmp;
    if (swap) snprintf(tmp, sizeof tmp, "%s.fmtmp", path);
    FILE *f = fopen(swap ? tmp : path, "wb");
    if (!f) return false;
    bool ok = write_all(f);
    if (fclose(f) != 0) ok = false;
    if (!swap) { if (ok) g_dirty = false; return ok; }
    if (!ok) { rg_storage_delete(tmp); return false; }
    if (!fs_rename(tmp, path)) { rg_storage_delete(tmp); return false; }
    g_dirty = false;
    return true;
}

/* ---------------------------------------------------------------- main loop */
static void press(int code, char ch, int rows)
{
    switch (code) {
        case 0:
            ins(ch);
            if (g_shift == 1) g_shift = 0;
            break;
        case KC_BKSP:  del_back(); break;
        case KC_DEL:   del_fwd(); break;
        case KC_ENTER: ins('\n'); break;
        case KC_TAB:   ins('\t'); break;
        case KC_SPACE: ins(' '); break;
        case KC_SHIFT: g_shift = (g_shift + 1) % 3; break;
        case KC_LEFT:  g_cur = prev_cp(g_cur); break;
        case KC_RIGHT: g_cur = next_cp(g_cur); break;
        case KC_UP:    move_lines(-1); return;
        case KC_DOWN:  move_lines(1); return;
        case KC_HOME:  g_cur = line_start(g_cur); break;
        case KC_END:   g_cur = line_end(g_cur); break;
        case KC_PGUP:  move_lines(-(rows - 1)); return;
        case KC_PGDN:  move_lines(rows - 1); return;
    }
    g_want = -1;
}

bool edit_text(const char *path)
{
    if (!ed_load(path)) {
        char sz[16];
        fmt_size(sz, ED_MAX_FILE);
        ui_message(path_base(path), tr(T_ED_TOO_BIG), sz, NULL, false);
        return false;
    }
    bool kb = true, saved = false;
    for (;;) {
        int rows = kb ? (KB_Y - 2 - 8) / 8 : FM_ROWS - 2;
        draw(path, kb);
        uint32_t k = input_wait();
        if (k & K_B) { del_back(); g_want = -1; }
        if (k & K_TIME) kb = !kb;
        if (kb) {
            if (k & K_UP)    kb_move_row(-1);
            if (k & K_DOWN)  kb_move_row(1);
            if (k & K_LEFT)  kb_col = (kb_col + row_len(kb_row) - 1) % row_len(kb_row);
            if (k & K_RIGHT) kb_col = (kb_col + 1) % row_len(kb_row);
            if (k & K_A) {
                const osk_key_t *key = &ROWS[kb_row][kb_col];
                press(key->code, g_shift ? key->up : key->lo, rows);
                if (g_gs == g_ge) ui_message(path_base(path), tr(T_ED_FULL), NULL, NULL, false);
            }
        } else {
            if (k & K_LEFT)  { g_cur = prev_cp(g_cur); g_want = -1; }
            if (k & K_RIGHT) { g_cur = next_cp(g_cur); g_want = -1; }
            if (k & K_UP)    move_lines(-1);
            if (k & K_DOWN)  move_lines(1);
            if (k & K_A)     kb = true;
        }
        if (k & K_GAME) {
            int a = g_dirty ? ui_choice(tr(T_ED_MENU), path_base(path), tr(T_SAVE), tr(T_ED_SAVE_EXIT), tr(T_ED_EXIT_NOSAVE))
                            : ui_choice(tr(T_ED_MENU), path_base(path), tr(T_ED_EXIT), NULL, NULL);
            if (!g_dirty) { if (a == 1) return saved; continue; }
            if (a == 1 || a == 2) {
                if (!ed_save(path)) { ui_message(path_base(path), tr(T_ERROR), NULL, NULL, false); continue; }
                saved = true;
                if (a == 2) return saved;
            } else if (a == 3) {
                if (ui_message(tr(T_ED_EXIT_NOSAVE), tr(T_ED_LOSE), NULL, NULL, true)) return saved;
            }
        }
    }
}
