/*
 * File Manager — on-screen keyboard for names (rename, new folder).
 *
 *   D-pad  move        A  type the key (hold to repeat)     B  delete (hold to repeat)
 *   TIME   letters / symbols                GAME  accept      PAUSE  cancel
 * The bottom row has Space / < / > / Delete / Shift / OK / Cancel keys: < and > move the text
 * cursor, and typing or deleting happens at the cursor. Shift: once = the next letter in
 * uppercase (green), twice = caps lock (red), three times = back to lowercase.
 */
#include <string.h>
#include <stdio.h>
#include "fm.h"

#define KCOLS 10
#define KROWS 4                       /* character rows; row KROWS is the special row */
#define FIELD 36                      /* characters of the name shown at once */

static const char *const PAGES[3][KROWS] = {
    { "1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm_.," },
    { "1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM_.," },
    { "!#$%&'()+=", "@^`{}[]~;,", "-_.       ", "          " },
};
enum { SK_SPACE, SK_LEFT, SK_RIGHT, SK_DEL, SK_PAGE, SK_OK, SK_CANCEL, SK_COUNT };
static const char *const SPECIAL_LABEL[SK_COUNT]    = { "Espacio", "<", ">", "Borra", "Mayus", "OK", "Cancela" };
static const char *const SPECIAL_LABEL_EN[SK_COUNT] = { "Space",   "<", ">", "Del",   "Shift", "OK", "Cancel" };

extern bool fm_lang_es(void);

static int g_shift;                   /* 0 lowercase, 1 next letter uppercase, 2 caps lock */

/* special keys: as wide as their label, centred, 2 px apart (like the editor's keyboard) */
static const char *sk_label(int s) { return fm_lang_es() ? SPECIAL_LABEL[s] : SPECIAL_LABEL_EN[s]; }
static int sk_w(int s) { return (int)strlen(sk_label(s)) * 8 + 8; }
static int sk_x(int s)
{
    int total = -2;
    for (int i = 0; i < SK_COUNT; i++) total += sk_w(i) + 2;
    int x = (FM_W - total) / 2;
    for (int i = 0; i < s; i++) x += sk_w(i) + 2;
    return x;
}
static int sk_mid(int s) { return sk_x(s) + sk_w(s) / 2; }
static int ck_mid(int c) { return c * 32 + 12; }         /* character keys: 4 columns each */

static void draw(const char *title, const char *text, size_t cur, int page, int row, int col)
{
    ui_begin();
    ui_text_w(0, 0, FM_COLS, title, C_BAR_TEXT, C_BAR);

    /* Text field: FIELD characters around the cursor, the cursor as a highlighted block */
    ui_fill(8, 3 * 8 - 3, FM_W - 16, 8 + 6, 0x2104);
    size_t n = strlen(text);
    size_t first = 0;
    if (cur >= FIELD) first = cur - FIELD + 1;
    if (n + 1 > FIELD && first > n + 1 - FIELD) first = n + 1 - FIELD;
    char line[FIELD + 1];
    size_t m = 0;
    for (size_t i = first; i < n && m < FIELD; i++) line[m++] = text[i];
    line[m] = 0;
    ui_text(2, 3, line, C_TEXT, 0x2104);
    char cc[2] = { cur < n ? text[cur] : ' ', 0 };
    ui_text(2 + (int)(cur - first), 3, cc, C_SEL_TEXT, C_SEL_BG);

    /* Character keys: 4 columns each, 2 rows apart */
    for (int r = 0; r < KROWS; r++)
        for (int c = 0; c < KCOLS; c++) {
            char ch = PAGES[page][r][c];
            char k[5] = { ' ', ch == ' ' ? ' ' : ch, ' ', ' ', 0 };
            bool sel = (r == row && c == col);
            ui_text(c * 4, 7 + r * 3, k, sel ? C_SEL_TEXT : C_TEXT, sel ? C_SEL_BG : 0x2104);
        }
    /* Special keys */
    int y = (7 + KROWS * 3) * 8;
    for (int s = 0; s < SK_COUNT; s++) {
        bool sel = (row == KROWS && col == s);
        bool lit = (s == SK_PAGE && g_shift);
        uint16_t bg = lit ? (g_shift == 2 ? C_SHIFT_LOCK : C_SHIFT_ONCE) : sel ? C_SEL_BG : 0x2104;
        uint16_t fg = lit ? (sel ? C_SEL_BG : C_TEXT) : sel ? C_SEL_TEXT : C_DIR;
        ui_fill(sk_x(s), y - 1, sk_w(s), 10, bg);
        ui_text_px(sk_x(s) + 4, y, sk_label(s), fg, bg);
    }
    ui_text_w(0, FM_ROWS - 1, FM_COLS, tr(T_OSK_HELP), C_BAR_TEXT, C_BAR);
    ui_end();
}

static bool insert(char *buf, size_t bufsize, size_t size, size_t *cur, char ch)
{
    size_t n = strlen(buf);
    if (n + 1 >= bufsize || n + 1 >= size) return false;
    memmove(buf + *cur + 1, buf + *cur, n - *cur + 1);
    buf[(*cur)++] = ch;
    return true;
}

static void erase_before(char *buf, size_t *cur)
{
    if (!*cur) return;
    memmove(buf + *cur - 1, buf + *cur, strlen(buf + *cur) + 1);
    (*cur)--;
}

static bool osk_loop(const char *title, char *text, size_t size)
{
    char buf[256];
    strncpy(buf, text, sizeof buf - 1);
    buf[sizeof buf - 1] = '\0';
    size_t cur = strlen(buf);
    int row = 1, col = 0;
    bool sym = false;
    g_shift = 0;

    for (;;) {
        int page = sym ? 2 : g_shift ? 1 : 0;
        draw(title, buf, cur, page, row, col);
        uint32_t k = input_wait();
        bool rep = input_was_repeat();
        int maxcol = (row == KROWS) ? SK_COUNT : KCOLS;

        if (k & (K_UP | K_DOWN)) {
            int from_special = (row == KROWS);
            int x = from_special ? sk_mid(col) : ck_mid(col);
            if (k & K_UP)   row = (row + KROWS) % (KROWS + 1);
            if (k & K_DOWN) row = (row + 1) % (KROWS + 1);
            /* keep the cursor over the nearest key when moving between the two key widths */
            if ((row == KROWS) != from_special) {
                int best = 0, bd = 1 << 30, cnt = (row == KROWS) ? SK_COUNT : KCOLS;
                for (int c = 0; c < cnt; c++) {
                    int mm = (row == KROWS) ? sk_mid(c) : ck_mid(c);
                    int d = mm > x ? mm - x : x - mm;
                    if (d < bd) { bd = d; best = c; }
                }
                col = best;
            }
            maxcol = (row == KROWS) ? SK_COUNT : KCOLS;
        }
        if (k & K_LEFT)  col = (col + maxcol - 1) % maxcol;
        if (k & K_RIGHT) col = (col + 1) % maxcol;
        if (k & K_TIME)  sym = !sym;
        if (k & K_B)     erase_before(buf, &cur);
        if (k & K_PAUSE)
            return false;
        if (k & K_GAME)
            goto accept;
        if (k & K_A) {
            if (row < KROWS) {
                char ch = PAGES[page][row][col];
                if (ch != ' ' && insert(buf, sizeof buf, size, &cur, ch))
                    if (g_shift == 1 && !sym) g_shift = 0;       /* one uppercase letter */
            } else switch (col) {
                case SK_SPACE:  insert(buf, sizeof buf, size, &cur, ' '); break;
                case SK_LEFT:   if (cur) cur--; break;
                case SK_RIGHT:  if (cur < strlen(buf)) cur++; break;
                case SK_DEL:    erase_before(buf, &cur); break;
                case SK_PAGE:   if (!rep) { g_shift = (g_shift + 1) % 3; sym = false; } break;
                case SK_OK:     if (!rep) goto accept; break;
                case SK_CANCEL: if (!rep) return false; break;
            }
        }
        continue;
    accept:
        if (!name_valid(buf)) {
            ui_message(title, tr(T_BAD_NAME), "\\ / : * ? \" < > |", NULL, false);
            continue;
        }
        strncpy(text, buf, size - 1);
        text[size - 1] = '\0';
        return true;
    }
}

bool osk_edit(const char *title, char *text, size_t size)
{
    input_set_repeat(K_A | K_B);      /* hold a key (or B) to repeat it, like a PC keyboard */
    bool r = osk_loop(title, text, size);
    input_set_repeat(0);
    return r;
}
