/*
 * File Manager — on-screen keyboard docked at the bottom of the screen (in the style of
 * gw-dos-xtulator): variable-width keys, Shift latches for one key or locks.
 * Shared by the text editor (KB_TEXT) and the hex editor (KB_HEX / KB_ASCII).
 */
#include <string.h>
#include "fm.h"

extern bool fm_lang_es(void);

typedef struct { char lo, up; uint8_t code; const char *en, *es; } kb_key_t;
#define CH(l, u) { l, u, 0, NULL, NULL }
#define SP(c, en, es) { 0, 0, c, en, es }
#define END { 0, 0, 0, NULL, NULL }

/* full keyboard */
static const kb_key_t T0[] = { CH('1','!'),CH('2','@'),CH('3','#'),CH('4','$'),CH('5','%'),CH('6','^'),CH('7','&'),
                               CH('8','*'),CH('9','('),CH('0',')'),CH('-','_'),CH('=','+'),SP(KC_BKSP,"Bksp","Borra"),END };
static const kb_key_t T1[] = { CH('q','Q'),CH('w','W'),CH('e','E'),CH('r','R'),CH('t','T'),CH('y','Y'),CH('u','U'),
                               CH('i','I'),CH('o','O'),CH('p','P'),CH('[','{'),CH(']','}'),SP(KC_DEL,"Del","Supr"),END };
static const kb_key_t T2[] = { CH('a','A'),CH('s','S'),CH('d','D'),CH('f','F'),CH('g','G'),CH('h','H'),CH('j','J'),
                               CH('k','K'),CH('l','L'),CH(';',':'),CH('\'','"'),CH('\\','|'),SP(KC_ENTER,"Enter","Intro"),END };
static const kb_key_t T3[] = { SP(KC_SHIFT,"Shift","Mayus"),CH('z','Z'),CH('x','X'),CH('c','C'),CH('v','V'),CH('b','B'),
                               CH('n','N'),CH('m','M'),CH(',','<'),CH('.','>'),CH('/','?'),CH('`','~'),SP(KC_TAB,"Tab","Tab"),END };
static const kb_key_t T4[] = { SP(KC_SPACE,"Space","Espacio"),SP(KC_LEFT,"<","<"),SP(KC_RIGHT,">",">"),SP(KC_UP,"^","^"),
                               SP(KC_DOWN,"v","v"),SP(KC_HOME,"Home","Ini"),SP(KC_END,"End","Fin"),
                               SP(KC_PGUP,"PgUp","RePag"),SP(KC_PGDN,"PgDn","AvPag"),END };
/* text column of the hex editor: same, last row ends with the switch back to hex */
static const kb_key_t A4[] = { SP(KC_SPACE,"Space","Espacio"),SP(KC_LEFT,"<","<"),SP(KC_RIGHT,">",">"),SP(KC_UP,"^","^"),
                               SP(KC_DOWN,"v","v"),SP(KC_PGUP,"PgUp","RePag"),SP(KC_PGDN,"PgDn","AvPag"),
                               SP(KC_SWAP,"Hex","Hex"),END };
/* hex keypad */
static const kb_key_t H0[] = { CH('0','0'),CH('1','1'),CH('2','2'),CH('3','3'),CH('4','4'),CH('5','5'),CH('6','6'),CH('7','7'),
                               SP(KC_DEC,"-1","-1"),SP(KC_INC,"+1","+1"),END };
static const kb_key_t H1[] = { CH('8','8'),CH('9','9'),CH('A','A'),CH('B','B'),CH('C','C'),CH('D','D'),CH('E','E'),CH('F','F'),
                               SP(KC_BKSP,"Bksp","Borra"),END };
static const kb_key_t H2[] = { SP(KC_LEFT,"<","<"),SP(KC_RIGHT,">",">"),SP(KC_UP,"^","^"),SP(KC_DOWN,"v","v"),
                               SP(KC_PGUP,"PgUp","RePag"),SP(KC_PGDN,"PgDn","AvPag"),SP(KC_SWAP,"Text","Texto"),END };

static const kb_key_t *const L_TEXT[]  = { T0, T1, T2, T3, T4 };
static const kb_key_t *const L_ASCII[] = { T0, T1, T2, T3, A4 };
static const kb_key_t *const L_HEX[]   = { H0, H1, H2 };

static const kb_key_t *const *g_rows = L_TEXT;
static int g_nrows = 5;
static int kb_row = 1, kb_col = 0;
static int g_shift;                       /* 0 off, 1 next key, 2 locked */

#define KEY_H 10

void kb_set_layout(int layout)
{
    switch (layout) {
        case KB_HEX:   g_rows = L_HEX;   g_nrows = 3; break;
        case KB_ASCII: g_rows = L_ASCII; g_nrows = 5; break;
        default:       g_rows = L_TEXT;  g_nrows = 5; break;
    }
    kb_row = (layout == KB_HEX) ? 0 : 1;
    kb_col = 0;
    g_shift = 0;
}

int kb_top(void) { return FM_H - 8 - g_nrows * KEY_H - 2; }

static int row_len(int r) { int n = 0; while (g_rows[r][n].lo || g_rows[r][n].code) n++; return n; }
static const char *key_label(const kb_key_t *k, char *buf)
{
    if (k->code) {
        if (k->code == KC_SHIFT && g_shift == 2) return fm_lang_es() ? "MAYUS" : "SHIFT";
        return fm_lang_es() ? k->es : k->en;
    }
    buf[0] = g_shift ? k->up : k->lo;
    buf[1] = 0;
    return buf;
}
static int key_w(const kb_key_t *k) { char b[2]; return (int)strlen(key_label(k, b)) * 8 + 8; }
static int row_w(int r) { int w = 0; for (int i = 0; i < row_len(r); i++) w += key_w(&g_rows[r][i]) + 2; return w - 2; }
static int key_x(int r, int c) { int x = (FM_W - row_w(r)) / 2; for (int i = 0; i < c; i++) x += key_w(&g_rows[r][i]) + 2; return x; }

static void move_row(int d)
{
    int cx = key_x(kb_row, kb_col) + key_w(&g_rows[kb_row][kb_col]) / 2;
    kb_row = (kb_row + d + g_nrows) % g_nrows;
    int best = 0, bd = 1 << 30;
    for (int c = 0; c < row_len(kb_row); c++) {
        int m = key_x(kb_row, c) + key_w(&g_rows[kb_row][c]) / 2;
        int dd = m > cx ? m - cx : cx - m;
        if (dd < bd) { bd = dd; best = c; }
    }
    kb_col = best;
}

void kb_nav(uint32_t k)
{
    if (k & K_UP)    move_row(-1);
    if (k & K_DOWN)  move_row(1);
    if (k & K_LEFT)  kb_col = (kb_col + row_len(kb_row) - 1) % row_len(kb_row);
    if (k & K_RIGHT) kb_col = (kb_col + 1) % row_len(kb_row);
}

int kb_activate(char *ch)
{
    const kb_key_t *key = &g_rows[kb_row][kb_col];
    if (key->code == KC_SHIFT) {
        g_shift = (g_shift + 1) % 3;
        return -1;
    }
    if (key->code)
        return key->code;
    *ch = g_shift ? key->up : key->lo;
    if (g_shift == 1) g_shift = 0;
    return 0;
}

void kb_draw(void)
{
    int y0 = kb_top() + 2;
    ui_fill(0, y0 - 2, FM_W, g_nrows * KEY_H + 2, 0x2104);
    for (int r = 0; r < g_nrows; r++)
        for (int c = 0; c < row_len(r); c++) {
            const kb_key_t *k = &g_rows[r][c];
            char b[2];
            const char *l = key_label(k, b);
            bool sel = (r == kb_row && c == kb_col);
            bool lit = (k->code == KC_SHIFT && g_shift);
            /* Shift: green = next key only, red = locked; the colour stays under the cursor */
            uint16_t bg = lit ? (g_shift == 2 ? C_SHIFT_LOCK : C_SHIFT_ONCE) : sel ? C_SEL_BG : 0x4208;
            uint16_t fg = lit ? (sel ? C_SEL_BG : C_TEXT) : sel ? C_SEL_TEXT : k->code ? C_DIR : C_TEXT;
            int x = key_x(r, c), w = key_w(k), y = y0 + r * KEY_H;
            ui_fill(x, y, w, KEY_H - 1, bg);
            ui_text_px(x + 4, y + 1, l, fg, bg);
        }
}
