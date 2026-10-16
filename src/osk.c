/*
 * File Manager — on-screen keyboard for names (rename, new folder).
 *
 *   D-pad  move        A  type the key      B  delete last character
 *   TIME   letters / symbols                GAME  accept      PAUSE  cancel
 * The bottom row also has Space / Delete / Shift / OK / Cancel keys. Shift: once = the next
 * letter in uppercase (green), twice = caps lock (red), three times = back to lowercase.
 */
#include <string.h>
#include <stdio.h>
#include "fm.h"

#define KCOLS 10
#define KROWS 4                       /* character rows; row KROWS is the special row */

static const char *const PAGES[3][KROWS] = {
    { "1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm_.," },
    { "1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM_.," },
    { "!#$%&'()+=", "@^`{}[]~;,", "-_.       ", "          " },
};
enum { SK_SPACE, SK_DEL, SK_PAGE, SK_OK, SK_CANCEL, SK_COUNT };
static const char *const SPECIAL_LABEL[SK_COUNT] = { "Espacio", "<-Borra", " Mayus", "  OK", "Cancela" };
static const char *const SPECIAL_LABEL_EN[SK_COUNT] = { " Space", "<-Del", " Shift", "  OK", "Cancel" };

extern bool fm_lang_es(void);

static int g_shift;                   /* 0 lowercase, 1 next letter uppercase, 2 caps lock */

static void draw(const char *title, const char *text, int page, int row, int col)
{
    ui_begin();
    ui_text_w(0, 0, FM_COLS, title, C_BAR_TEXT, C_BAR);

    /* Text field: last 36 characters, with a cursor block */
    ui_fill(8, 3 * 8 - 3, FM_W - 16, 8 + 6, 0x2104);
    size_t n = strlen(text);
    const char *shown = n > 36 ? text + n - 36 : text;
    char line[48];
    snprintf(line, sizeof line, "%s_", shown);
    ui_text(2, 3, line, C_TEXT, 0x2104);

    /* Character keys: 4 columns each, 2 rows apart */
    for (int r = 0; r < KROWS; r++)
        for (int c = 0; c < KCOLS; c++) {
            char ch = PAGES[page][r][c];
            char k[5] = { ' ', ch == ' ' ? ' ' : ch, ' ', ' ', 0 };
            bool sel = (r == row && c == col);
            ui_text(c * 4, 7 + r * 3, k, sel ? C_SEL_TEXT : C_TEXT, sel ? C_SEL_BG : 0x2104);
        }
    /* Special keys: 8 columns each */
    for (int s = 0; s < SK_COUNT; s++) {
        bool sel = (row == KROWS && col == s);
        bool lit = (s == SK_PAGE && g_shift);
        uint16_t bg = lit ? (g_shift == 2 ? C_SHIFT_LOCK : C_SHIFT_ONCE) : sel ? C_SEL_BG : 0x2104;
        uint16_t fg = lit ? (sel ? C_SEL_BG : C_TEXT) : sel ? C_SEL_TEXT : C_DIR;
        ui_text_w(s * 8, 7 + KROWS * 3, 7, fm_lang_es() ? SPECIAL_LABEL[s] : SPECIAL_LABEL_EN[s], fg, bg);
    }
    ui_text_w(0, FM_ROWS - 1, FM_COLS, tr(T_OSK_HELP), C_BAR_TEXT, C_BAR);
    ui_end();
}

bool osk_edit(const char *title, char *text, size_t size)
{
    char buf[256];
    strncpy(buf, text, sizeof buf - 1);
    buf[sizeof buf - 1] = '\0';
    int row = 1, col = 0;
    bool sym = false;
    g_shift = 0;

    for (;;) {
        int page = sym ? 2 : g_shift ? 1 : 0;
        draw(title, buf, page, row, col);
        uint32_t k = input_wait();
        size_t n = strlen(buf);
        int maxcol = (row == KROWS) ? SK_COUNT : KCOLS;

        if (k & K_UP)    { row = (row + KROWS) % (KROWS + 1); }
        if (k & K_DOWN)  { row = (row + 1) % (KROWS + 1); }
        if (k & (K_UP | K_DOWN)) {
            /* keep the cursor roughly in place when moving between the two key widths */
            int newmax = (row == KROWS) ? SK_COUNT : KCOLS;
            if (maxcol != newmax) col = (maxcol == KCOLS) ? col * SK_COUNT / KCOLS : col * KCOLS / SK_COUNT;
            if (col >= newmax) col = newmax - 1;
            maxcol = newmax;
        }
        if (k & K_LEFT)  col = (col + maxcol - 1) % maxcol;
        if (k & K_RIGHT) col = (col + 1) % maxcol;
        if (k & K_TIME)  sym = !sym;
        if (k & K_B) {
            if (n) buf[n - 1] = '\0';
        }
        if (k & K_PAUSE)
            return false;
        if (k & K_GAME)
            goto accept;
        if (k & K_A) {
            if (row < KROWS) {
                char ch = PAGES[page][row][col];
                if (ch != ' ' && n + 1 < sizeof buf && n + 1 < size) {
                    buf[n] = ch; buf[n + 1] = '\0';
                    if (g_shift == 1 && !sym) g_shift = 0;       /* one uppercase letter */
                }
            } else switch (col) {
                case SK_SPACE:  if (n + 1 < sizeof buf && n + 1 < size) { buf[n] = ' '; buf[n + 1] = '\0'; } break;
                case SK_DEL:    if (n) buf[n - 1] = '\0'; break;
                case SK_PAGE:   g_shift = (g_shift + 1) % 3; sym = false; break;
                case SK_OK:     goto accept;
                case SK_CANCEL: return false;
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
