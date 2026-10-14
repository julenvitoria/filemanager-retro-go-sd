/*
 * File Manager — Retro-Go SD GWHB homebrew for the Game & Watch.
 *
 * Browse the SD card; copy, move, rename and delete files and whole folders; create folders;
 * view text, hex and BMP files (the firmware's screenshots); SD card capacity and free space.
 *
 * Controls (list):
 *   Up/Down  move          Left/Right  page         A  open folder / view file
 *   B        parent folder  TIME       select / deselect  GAME  actions menu
 *   PAUSE    Retro-Go menu (brightness, volume, quit, power off)
 *
 * Entry (run_gwhb_homebrew):
 *   void app_main(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
 */
#include <odroid_system.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>

#include "gw_lcd.h"
#include "gw_malloc.h"
#include "common.h"
#include "appid.h"
#include "odroid_overlay.h"
#include "rg_storage.h"
#include "ff.h"

#include "fm.h"

#include "gw_core_bridge.h"
#include "gw_core_i18n.h"

/* ================================================================ strings */

#define S(en, es) { { "en", en }, { "es", es }, GW_I18N_END }
static const gw_i18n_entry_t STR[T_COUNT][3] = {
    [T_TITLE]          = S("File Manager", "Explorador"),
    [T_HELP]           = S("A:Open B:Back TIME:Select GAME:Menu", "A:Abrir B:Atras TIME:Seleccion GAME:Menu"),
    [T_EMPTY]          = S("(empty folder)", "(carpeta vacia)"),
    [T_OPEN]           = S("Open", "Abrir"),
    [T_COPY]           = S("Copy", "Copiar"),
    [T_CUT]            = S("Cut (move)", "Cortar (mover)"),
    [T_PASTE]          = S("Paste here", "Pegar aquí"),
    [T_RENAME]         = S("Rename", "Renombrar"),
    [T_DELETE]         = S("Delete", "Borrar"),
    [T_NEWDIR]         = S("New folder", "Nueva carpeta"),
    [T_INFO]           = S("Information", "Información"),
    [T_VIEW_TEXT]      = S("View as text", "Ver como texto"),
    [T_VIEW_HEX]       = S("View in hex", "Ver en hexadecimal"),
    [T_SDINFO]         = S("SD card", "Tarjeta SD"),
    [T_EXIT]           = S("Exit", "Salir"),
    [T_MARK_ALL]       = S("Select all", "Seleccionar todo"),
    [T_UNMARK_ALL]     = S("Deselect all", "Quitar selección"),
    [T_ACTIONS]        = S("Actions", "Acciones"),
    [T_CONFIRM_DELETE] = S("Delete?", "¿Borrar?"),
    [T_YES]            = S("Yes", "Sí"),
    [T_NO]             = S("No", "No"),
    [T_ITEMS]          = S("items", "elementos"),
    [T_OVERWRITE]      = S("Overwrite", "Sobrescribir"),
    [T_SKIP]           = S("Skip", "Omitir"),
    [T_CANCEL]         = S("Cancel", "Cancelar"),
    [T_EXISTS]         = S("Already exists:", "Ya existe:"),
    [T_COPYING]        = S("Copying", "Copiando"),
    [T_MOVING]         = S("Moving", "Moviendo"),
    [T_DELETING]       = S("Deleting", "Borrando"),
    [T_COUNTING]       = S("Counting", "Contando"),
    [T_ERROR]          = S("Error", "Error"),
    [T_INTO_ITSELF]    = S("Cannot put a folder inside itself", "No se puede meter una carpeta en sí misma"),
    [T_NAME]           = S("Name", "Nombre"),
    [T_SIZE]           = S("Size", "Tamaño"),
    [T_DATE]           = S("Date", "Fecha"),
    [T_TYPE]           = S("Type", "Tipo"),
    [T_FOLDER]         = S("Folder", "Carpeta"),
    [T_FILE]           = S("File", "Archivo"),
    [T_CONTAINS]       = S("Contains", "Contiene"),
    [T_FREE]           = S("Free", "Libre"),
    [T_TOTAL]          = S("Total", "Total"),
    [T_USED]           = S("Used", "Usado"),
    [T_UNKNOWN]        = S("Needs a firmware with free-space support", "Necesita un firmware que lo permita"),
    [T_CLIP_COPY]      = S("to copy", "para copiar"),
    [T_CLIP_CUT]       = S("to move", "para mover"),
    [T_OK]             = S("OK", "Aceptar"),
    [T_BAD_NAME]       = S("Invalid name. Not allowed:", "Nombre no válido. No se permite:"),
    [T_DONE]           = S("Done", "Hecho"),
    [T_FAST_RENAME]    = S("Rename/move: instant", "Renombrar/mover: instantáneo"),
    [T_SLOW_RENAME]    = S("Rename/move: copy + delete", "Renombrar/mover: copia y borra"),
    [T_OSK_HELP]       = S("A:Type B:Del TIME:abc/ABC GAME:OK", "A:Escribe B:Borra TIME:abc/ABC GAME:OK"),
    [T_VIEW_HELP]      = S("Up/Down:Scroll Left/Right:Page B:Back", "Arr/Abj:Mover Izq/Der:Pagina B:Salir"),
    [T_IMG_UNSUPPORTED]= S("Unsupported image format", "Formato de imagen no soportado"),
    [T_CANCELLED]      = S("Cancelled", "Cancelado"),
    [T_CLEAR_CLIP]     = S("Clear clipboard", "Vaciar portapapeles"),
    [T_PATH]           = S("Path", "Ruta"),
};

#ifdef FM_TEST_ES      /* emulator tests: Spanish UI regardless of the firmware setting */
const char *tr(int id) { return STR[id][1].text; }
bool fm_lang_es(void) { return true; }
#else
const char *tr(int id) { return gw_i18n(STR[id]); }

bool fm_lang_es(void)
{
    static const gw_i18n_entry_t probe[] = { { "en", "en" }, { "es", "es" }, GW_I18N_END };
    return strcmp(gw_i18n(probe), "es") == 0;
}
#endif

/* The firmware's 8x8 font only has ASCII: fold UTF-8 Latin letters to their base letter. */
static void ascii_fold(char *out, size_t size, const char *in)
{
    size_t o = 0;
    for (const uint8_t *p = (const uint8_t *)in; *p && o + 1 < size; p++) {
        if (*p < 0x80) { out[o++] = (char)*p; continue; }
        if (*p == 0xC3 && p[1]) {
            static const char map[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
            uint8_t c = p[1];
            out[o++] = (c >= 0x80 && c <= 0xBF) ? map[c - 0x80] : '?';
            p++;
            continue;
        }
        if (*p == 0xC2 && p[1]) {
            out[o++] = (p[1] == 0xBF) ? '?' : (p[1] == 0xA1) ? '!' : '.';
            p++;
            continue;
        }
        if (*p < 0x80 || *p >= 0xC0) out[o++] = '?';   /* lead byte of something else */
    }
    out[o] = '\0';
}

/* ================================================================ drawing */

uint16_t *ui_buffer(void) { return (uint16_t *)lcd_get_active_buffer(); }

void ui_fill(int x, int y, int w, int h, uint16_t c) { odroid_overlay_draw_fill_rect(x, y, w, h, c); }

void ui_begin(void)
{
    lcd_sleep_while_swap_pending();
    uint16_t *fb = ui_buffer();
    for (int i = 0; i < FM_W * FM_H; i++)
        fb[i] = C_BG;
}

void ui_end(void) { lcd_swap(); }

void ui_text(int col, int row, const char *s, uint16_t fg, uint16_t bg)
{
    char buf[FM_COLS * 4 + 1];
    ascii_fold(buf, sizeof buf, s);
    if (col >= FM_COLS || row >= FM_ROWS)
        return;
    buf[FM_COLS - col] = '\0';
    if (buf[0])
        odroid_overlay_draw_text((uint16_t)(col * 8), (uint16_t)(row * 8), 0, buf, fg, bg);
}

void ui_text_w(int col, int row, int width, const char *s, uint16_t fg, uint16_t bg)
{
    char buf[FM_COLS * 4 + 1], pad[FM_COLS + 1];
    ascii_fold(buf, sizeof buf, s);
    if (width > FM_COLS - col) width = FM_COLS - col;
    if (width <= 0) return;
    int n = (int)strlen(buf);
    if (n > width) n = width;
    memcpy(pad, buf, (size_t)n);
    memset(pad + n, ' ', (size_t)(width - n));
    pad[width] = '\0';
    odroid_overlay_draw_text((uint16_t)(col * 8), (uint16_t)(row * 8), (uint16_t)(width * 8), pad, fg, bg);
}

/* Dialog background. The firmware's dialogs redraw the background every frame and then
 * darken it, so it must come from an untouched copy (copying the screen itself would darken
 * it again on each frame until it is black). dialog_prepare() saves what is on screen now. */
static uint16_t g_dialog_bg[FM_W * FM_H];
static void dialog_prepare(void)
{
    lcd_sleep_while_swap_pending();
    memcpy(g_dialog_bg, lcd_get_inactive_buffer(), sizeof g_dialog_bg);
}
static void repaint_current(void)
{
    memcpy(lcd_get_active_buffer(), g_dialog_bg, sizeof g_dialog_bg);
}

int ui_message(const char *title, const char *l1, const char *l2, const char *l3, bool yes_no)
{
    odroid_dialog_choice_t ch[8];
    int n = 0;
    if (l1) ch[n++] = (odroid_dialog_choice_t){ 0, l1, "", -1, NULL };
    if (l2) ch[n++] = (odroid_dialog_choice_t){ 0, l2, "", -1, NULL };
    if (l3) ch[n++] = (odroid_dialog_choice_t){ 0, l3, "", -1, NULL };
    ch[n++] = (odroid_dialog_choice_t)ODROID_DIALOG_CHOICE_SEPARATOR;
    int first = n;
    if (yes_no) {
        ch[n++] = (odroid_dialog_choice_t){ 1, tr(T_YES), "", 1, NULL };
        ch[n++] = (odroid_dialog_choice_t){ 0, tr(T_NO), "", 1, NULL };
    } else {
        ch[n++] = (odroid_dialog_choice_t){ 1, tr(T_OK), "", 1, NULL };
    }
    ch[n++] = (odroid_dialog_choice_t)ODROID_DIALOG_CHOICE_LAST;
    dialog_prepare();
    int r = odroid_overlay_dialog(title, ch, yes_no ? first + 1 : first, &repaint_current, 0);
    return r == 1;
}

/* ================================================================ input */

static odroid_gamepad_state_t joy;
static const odroid_dialog_choice_t no_options[] = { ODROID_DIALOG_CHOICE_LAST };

uint32_t input_poll(void)
{
    odroid_input_read_gamepad(&joy);
    uint32_t k = 0;
    if (joy.values[ODROID_INPUT_UP])     k |= K_UP;
    if (joy.values[ODROID_INPUT_DOWN])   k |= K_DOWN;
    if (joy.values[ODROID_INPUT_LEFT])   k |= K_LEFT;
    if (joy.values[ODROID_INPUT_RIGHT])  k |= K_RIGHT;
    if (joy.values[ODROID_INPUT_A])      k |= K_A;
    if (joy.values[ODROID_INPUT_B])      k |= K_B;
    if (joy.values[ODROID_INPUT_START] || joy.values[ODROID_INPUT_X])  k |= K_GAME;
    if (joy.values[ODROID_INPUT_SELECT] || joy.values[ODROID_INPUT_Y]) k |= K_TIME;
    if (joy.values[ODROID_INPUT_VOLUME]) k |= K_PAUSE;
    if (joy.values[ODROID_INPUT_POWER])  k |= K_POWER;
    return k;
}

/* Next key press. D-pad auto-repeats. PAUSE (and PAUSE combos, POWER) go to the firmware:
 * Retro-Go menu, brightness/volume shortcuts, sleep. Returns 0 if the screen must be redrawn. */
uint32_t input_wait(void)
{
    static uint32_t held = ~0u;   /* ignore keys still down from the previous screen */
    static uint32_t repeat_at = 0;
    for (;;) {
        wdog_refresh();
        uint32_t k = input_poll();
        if (k & (K_PAUSE | K_POWER)) {
            /* The firmware acts on the release of PAUSE (menu) unless a combo was used
             * (PAUSE+B brightness, PAUSE+GAME screenshot...), so feed it every poll until the
             * keys are up, including that last "released" one. */
            dialog_prepare();
            do {
                wdog_refresh();
                common_emu_input_loop(&joy, (odroid_dialog_choice_t *)no_options, &repaint_current);
                HAL_Delay(10);
                k = input_poll();
            } while (k & (K_PAUSE | K_POWER));
            common_emu_input_loop(&joy, (odroid_dialog_choice_t *)no_options, &repaint_current);
            held = ~0u;
            return 0;
        }
        uint32_t now = HAL_GetTick();
        uint32_t fresh = k & ~held;
        if (held == ~0u) fresh = 0;
        held = k;
        if (fresh) {
            repeat_at = now + 350;
            return fresh;
        }
        const uint32_t dpad = K_UP | K_DOWN | K_LEFT | K_RIGHT;
        if ((k & dpad) && (int32_t)(now - repeat_at) >= 0) {
            repeat_at = now + 60;
            return k & dpad;
        }
        HAL_Delay(10);
    }
}

/* ================================================================ directory listing */

typedef struct {
    uint32_t name;          /* offset in g_pool */
    uint32_t size;
    uint16_t date, time;
    uint8_t  dir, mark;
} entry_t;

#define MAX_ENTRIES 4096
#define POOL_SIZE   (96 * 1024)
static entry_t g_ent[MAX_ENTRIES];
static char    g_pool[POOL_SIZE];
static int     g_count, g_cursor, g_top, g_marked;
static bool    g_truncated;
static char    g_cwd[FM_PATH_MAX] = "/";

#define LIST_ROW0  1
#define LIST_ROWS  (FM_ROWS - 3)

static const char *ename(const entry_t *e) { return g_pool + e->name; }

static int cmp_entry(const void *a, const void *b)
{
    const entry_t *x = a, *y = b;
    if (x->dir != y->dir)
        return x->dir ? -1 : 1;
    return strcasecmp(ename(x), ename(y));
}

static void load_dir(const char *select_name)
{
    DIR d;
    FILINFO fi;
    g_count = 0;
    g_marked = 0;
    g_truncated = false;
    size_t used = 0;
    if (f_opendir(&d, g_cwd) == FR_OK) {
        while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
            size_t n = strlen(fi.fname) + 1;
            if (g_count >= MAX_ENTRIES || used + n > POOL_SIZE) { g_truncated = true; break; }
            entry_t *e = &g_ent[g_count++];
            memcpy(g_pool + used, fi.fname, n);
            e->name = (uint32_t)used;
            used += n;
            e->size = (uint32_t)fi.fsize;
            e->date = fi.fdate;
            e->time = fi.ftime;
            e->dir = (fi.fattrib & AM_DIR) != 0;
            e->mark = 0;
            if ((g_count & 63) == 0) wdog_refresh();
        }
        f_closedir(&d);
    }
    qsort(g_ent, (size_t)g_count, sizeof g_ent[0], cmp_entry);
    g_cursor = 0;
    if (select_name)
        for (int i = 0; i < g_count; i++)
            if (strcmp(ename(&g_ent[i]), select_name) == 0) { g_cursor = i; break; }
    if (g_cursor >= g_count) g_cursor = g_count ? g_count - 1 : 0;
    g_top = g_cursor - LIST_ROWS / 2;
    if (g_top < 0) g_top = 0;
}

/* ================================================================ clipboard */

#define CLIP_MAX       128
#define CLIP_POOL_SIZE (32 * 1024)
static char     g_clip_pool[CLIP_POOL_SIZE];
static uint32_t g_clip_off[CLIP_MAX];
static int      g_clip_n;
static bool     g_clip_cut;

static const char *clip_path(int i) { return g_clip_pool + g_clip_off[i]; }

/* Items an action applies to: the marked ones, or the one under the cursor */
static int targets(int *out, int max)
{
    int n = 0;
    if (g_marked) {
        for (int i = 0; i < g_count && n < max; i++)
            if (g_ent[i].mark) out[n++] = i;
    } else if (g_count) {
        out[n++] = g_cursor;
    }
    return n;
}

static void clip_set(bool cut)
{
    static int idx[CLIP_MAX];
    int n = targets(idx, CLIP_MAX);
    size_t used = 0;
    g_clip_n = 0;
    for (int i = 0; i < n; i++) {
        char p[FM_PATH_MAX];
        path_join(p, g_cwd, ename(&g_ent[idx[i]]));
        size_t len = strlen(p) + 1;
        if (used + len > CLIP_POOL_SIZE) break;
        memcpy(g_clip_pool + used, p, len);
        g_clip_off[g_clip_n++] = (uint32_t)used;
        used += len;
    }
    g_clip_cut = cut;
    for (int i = 0; i < g_count; i++) g_ent[i].mark = 0;
    g_marked = 0;
}

/* ================================================================ list screen */

static void fmt_date(char *out, uint16_t d, uint16_t t)
{
    sprintf(out, "%02u/%02u/%04u %02u:%02u", d & 31, (d >> 5) & 15, 1980 + (d >> 9), t >> 11, (t >> 5) & 63);
}

static void draw_list(void)
{
    ui_begin();
    /* Title: the path, keeping its end visible */
    char t[FM_PATH_MAX + 8];
    size_t n = strlen(g_cwd);
    if (n > FM_COLS) snprintf(t, sizeof t, "..%s", g_cwd + n - (FM_COLS - 2));
    else snprintf(t, sizeof t, "%s", g_cwd);
    ui_text_w(0, 0, FM_COLS, t, C_BAR_TEXT, C_BAR);

    if (!g_count)
        ui_text(2, LIST_ROW0 + 1, tr(T_EMPTY), C_DIM, C_BG);
    for (int r = 0; r < LIST_ROWS && g_top + r < g_count; r++) {
        int i = g_top + r;
        const entry_t *e = &g_ent[i];
        bool sel = (i == g_cursor);
        uint16_t bg = sel ? C_SEL_BG : C_BG;
        uint16_t fg = sel ? C_SEL_TEXT : e->mark ? C_MARK : e->dir ? C_DIR : C_TEXT;
        char name[FM_COLS + 8], sz[12];
        snprintf(name, sizeof name, "%c%s%s", e->mark ? '*' : ' ', ename(e), e->dir ? "/" : "");
        if (e->dir) sz[0] = '\0'; else fmt_size(sz, e->size);
        ui_text_w(0, LIST_ROW0 + r, FM_COLS - 7, name, fg, bg);
        char right[8];
        snprintf(right, sizeof right, "%6s ", sz);
        ui_text_w(FM_COLS - 7, LIST_ROW0 + r, 7, right, fg, bg);
    }

    /* Status line: position, marks, clipboard */
    char st[96];
    int p = snprintf(st, sizeof st, " %d/%d", g_count ? g_cursor + 1 : 0, g_count);
    if (g_truncated) p += snprintf(st + p, sizeof st - p, "+");
    if (g_marked) p += snprintf(st + p, sizeof st - p, "  *%d", g_marked);
    if (g_clip_n) p += snprintf(st + p, sizeof st - p, "  [%d %s]", g_clip_n, tr(g_clip_cut ? T_CLIP_CUT : T_CLIP_COPY));
    ui_text_w(0, FM_ROWS - 2, FM_COLS, st, C_DIM, C_BG);
    ui_text_w(0, FM_ROWS - 1, FM_COLS, tr(T_HELP), C_BAR_TEXT, C_BAR);
    ui_end();
}

static void cursor_move(int delta)
{
    if (!g_count) return;
    g_cursor += delta;
    if (g_cursor < 0) g_cursor = (delta == -1) ? g_count - 1 : 0;
    if (g_cursor >= g_count) g_cursor = (delta == 1) ? 0 : g_count - 1;
    if (g_cursor < g_top) g_top = g_cursor;
    if (g_cursor >= g_top + LIST_ROWS) g_top = g_cursor - LIST_ROWS + 1;
}

/* ================================================================ actions */

static void show_result(op_result_t r, const char *what)
{
    if (r == OP_ERROR) ui_message(tr(T_ERROR), what, NULL, NULL, false);
    else if (r == OP_CANCELLED) ui_message(tr(T_CANCELLED), what, NULL, NULL, false);
}

/* "name" -> "name (2)", keeping the extension: for copies into the same folder */
static void unique_name(char *out, const char *dir, const char *name)
{
    char base[256], ext[64] = "";
    snprintf(base, sizeof base, "%s", name);
    char *dot = strrchr(base, '.');
    if (dot && dot != base) { snprintf(ext, sizeof ext, "%s", dot); *dot = '\0'; }
    for (int n = 2; n < 1000; n++) {
        char cand[300];
        snprintf(cand, sizeof cand, "%s (%d)%s", base, n, ext);
        path_join(out, dir, cand);
        if (!fs_exists(out)) return;
    }
}

static void do_paste(void)
{
    if (!g_clip_n) return;
    bool cut = g_clip_cut;
    bool fast = cut && op_fast_rename_available();

    /* Size the job for the progress bar (instant moves only count items) */
    uint64_t bytes = 0;
    uint32_t items = 0;
    op_begin(tr(T_COUNTING), 0, 0);
    for (int i = 0; i < g_clip_n; i++) {
        fs_count_t c;
        if (fast) { items++; continue; }
        if (!fs_count(clip_path(i), &c)) break;
        bytes += c.bytes;
        items += c.files + c.dirs + 1;
    }

    op_result_t r = OP_OK;
    const char *failed = NULL;
    op_begin(tr(cut ? T_MOVING : T_COPYING), bytes, items);
    for (int i = 0; i < g_clip_n && r != OP_CANCELLED; i++) {
        const char *src = clip_path(i);
        char dst[FM_PATH_MAX];
        path_join(dst, g_cwd, path_base(src));
        if (strcmp(src, dst) == 0) {
            if (cut) continue;                 /* moving onto itself: nothing to do */
            unique_name(dst, g_cwd, path_base(src));
        }
        if (fs_is_dir(src) && path_is_inside(dst, src)) {
            ui_message(tr(T_ERROR), tr(T_INTO_ITSELF), path_base(src), NULL, false);
            op_begin(tr(cut ? T_MOVING : T_COPYING), bytes, items);
            continue;
        }
        if (fs_exists(dst)) {
            odroid_dialog_choice_t ch[] = {
                { 0, tr(T_EXISTS), "", -1, NULL },
                { 0, path_base(dst), "", -1, NULL },
                ODROID_DIALOG_CHOICE_SEPARATOR,
                { 1, tr(T_OVERWRITE), "", 1, NULL },
                { 2, tr(T_SKIP), "", 1, NULL },
                { 3, tr(T_CANCEL), "", 1, NULL },
                ODROID_DIALOG_CHOICE_LAST,
            };
            dialog_prepare();
            int a = odroid_overlay_dialog(cut ? tr(T_MOVING) : tr(T_COPYING), ch, 4, &repaint_current, 0);
            if (a == 2) continue;
            if (a != 1) { r = OP_CANCELLED; break; }
            op_begin(tr(cut ? T_MOVING : T_COPYING), bytes, items);
            r = op_delete(dst);
            if (r != OP_OK) { failed = path_base(dst); break; }
        }
        r = cut ? op_move(src, dst) : op_copy(src, dst);
        if (r == OP_ERROR) failed = path_base(src);
    }
    if (cut && r == OP_OK) g_clip_n = 0;
    load_dir(NULL);
    draw_list();
    if (r == OP_ERROR) show_result(r, failed ? failed : "");
    else if (r == OP_CANCELLED) show_result(r, "");
}

static void do_delete(void)
{
    static int idx[MAX_ENTRIES];
    int n = targets(idx, MAX_ENTRIES);
    if (!n) return;
    char l1[96];
    if (n == 1)
        snprintf(l1, sizeof l1, "%s%s", ename(&g_ent[idx[0]]), g_ent[idx[0]].dir ? "/" : "");
    else
        snprintf(l1, sizeof l1, "%d %s", n, tr(T_ITEMS));
    if (!ui_message(tr(T_CONFIRM_DELETE), l1, NULL, NULL, true))
        return;
    op_begin(tr(T_COUNTING), 0, 0);
    uint32_t items = 0;
    for (int i = 0; i < n; i++) {
        char p[FM_PATH_MAX];
        fs_count_t c;
        path_join(p, g_cwd, ename(&g_ent[idx[i]]));
        if (fs_count(p, &c)) items += c.files + c.dirs + (g_ent[idx[i]].dir ? 1 : 0);
    }
    op_begin(tr(T_DELETING), 0, items);
    op_result_t r = OP_OK;
    const char *failed = NULL;
    for (int i = 0; i < n && r != OP_CANCELLED; i++) {
        char p[FM_PATH_MAX];
        path_join(p, g_cwd, ename(&g_ent[idx[i]]));
        r = op_delete(p);
        if (r == OP_ERROR) { failed = ename(&g_ent[idx[i]]); break; }
    }
    char failed_copy[256] = "";
    if (failed) snprintf(failed_copy, sizeof failed_copy, "%s", failed);
    int keep = g_cursor;
    load_dir(NULL);
    g_cursor = keep < g_count ? keep : (g_count ? g_count - 1 : 0);
    cursor_move(0);
    draw_list();
    if (r != OP_OK) show_result(r, failed_copy);
}

static void do_rename(void)
{
    if (!g_count) return;
    entry_t *e = &g_ent[g_cursor];
    char name[256];
    snprintf(name, sizeof name, "%s", ename(e));
    if (!osk_edit(tr(T_RENAME), name, sizeof name) || strcmp(name, ename(e)) == 0) {
        draw_list();
        return;
    }
    char src[FM_PATH_MAX], dst[FM_PATH_MAX];
    path_join(src, g_cwd, ename(e));
    path_join(dst, g_cwd, name);
    /* Only a change of case may "collide" with itself (FAT names are case-insensitive) */
    if (fs_exists(dst) && strcasecmp(name, ename(e)) != 0) {
        draw_list();
        ui_message(tr(T_ERROR), tr(T_EXISTS), name, NULL, false);
        return;
    }
    op_result_t r;
    if (fs_rename(src, dst)) {
        r = OP_OK;
    } else if (strcasecmp(name, ename(e)) == 0) {
        /* case-only change without the firmware's rename: go through a temporary name */
        char tmp[FM_PATH_MAX];
        unique_name(tmp, g_cwd, "~rename");
        op_begin(tr(T_MOVING), 0, 2);
        r = op_move(src, tmp);
        if (r == OP_OK) r = op_move(tmp, dst);
    } else {
        fs_count_t c;
        op_begin(tr(T_COUNTING), 0, 0);
        fs_count(src, &c);
        op_begin(tr(T_MOVING), c.bytes, c.files + c.dirs + 1);
        r = op_move(src, dst);
    }
    load_dir(name);
    draw_list();
    show_result(r, name);
}

static void do_newdir(void)
{
    char name[256];
    snprintf(name, sizeof name, "%s", fm_lang_es() ? "Carpeta nueva" : "New folder");
    if (!osk_edit(tr(T_NEWDIR), name, sizeof name)) { draw_list(); return; }
    char p[FM_PATH_MAX];
    path_join(p, g_cwd, name);
    if (fs_exists(p)) {
        draw_list();
        ui_message(tr(T_ERROR), tr(T_EXISTS), name, NULL, false);
        return;
    }
    bool ok = rg_storage_mkdir(p);
    load_dir(name);
    draw_list();
    if (!ok) ui_message(tr(T_ERROR), name, NULL, NULL, false);
}

static void do_info(void)
{
    if (!g_count) return;
    entry_t *e = &g_ent[g_cursor];
    char p[FM_PATH_MAX], l1[128], l2[96], l3[96], l4[96], sz[16];
    path_join(p, g_cwd, ename(e));
    snprintf(l1, sizeof l1, "%s", ename(e));
    if (e->dir) {
        fs_count_t c;
        op_begin(tr(T_COUNTING), 0, 0);
        bool ok = fs_count(p, &c);
        draw_list();
        fmt_size(sz, c.bytes);
        snprintf(l2, sizeof l2, "%s: %s%s", tr(T_SIZE), sz, ok ? "" : " (?)");
        snprintf(l3, sizeof l3, "%s: %lu %s, %lu %s", tr(T_CONTAINS), (unsigned long)c.files,
                 fm_lang_es() ? "arch." : "files", (unsigned long)c.dirs, fm_lang_es() ? "carp." : "folders");
    } else {
        fmt_size(sz, e->size);
        snprintf(l2, sizeof l2, "%s: %s (%lu B)", tr(T_SIZE), sz, (unsigned long)e->size);
        l3[0] = '\0';
    }
    char d[24];
    fmt_date(d, e->date, e->time);
    snprintf(l4, sizeof l4, "%s: %s", tr(T_DATE), d);
    ui_message(e->dir ? tr(T_FOLDER) : tr(T_FILE), l1, l2, l3[0] ? l3 : l4, false);
}

static void do_sdinfo(void)
{
    uint64_t fr, tot;
    char l1[64], l2[64], l3[64], a[16];
    if (!fs_free_space(&fr, &tot) || !tot) {
        ui_message(tr(T_SDINFO), tr(T_UNKNOWN), tr(op_fast_rename_available() ? T_FAST_RENAME : T_SLOW_RENAME), NULL, false);
        return;
    }
    fmt_size(a, tot);
    snprintf(l1, sizeof l1, "%s: %s", tr(T_TOTAL), a);
    fmt_size(a, tot - fr);
    snprintf(l2, sizeof l2, "%s: %s (%u%%)", tr(T_USED), a, (unsigned)((tot - fr) * 100 / tot));
    fmt_size(a, fr);
    snprintf(l3, sizeof l3, "%s: %s (%u%%)", tr(T_FREE), a, (unsigned)(fr * 100 / tot));
    ui_message(tr(T_SDINFO), l1, l2, l3, false);
}

static void open_entry(bool force_text, bool force_hex)
{
    if (!g_count) return;
    entry_t *e = &g_ent[g_cursor];
    char p[FM_PATH_MAX];
    path_join(p, g_cwd, ename(e));
    if (e->dir && !force_text && !force_hex) {
        if (strlen(p) + 2 >= FM_PATH_MAX) return;
        strcpy(g_cwd, p);
        load_dir(NULL);
        return;
    }
    if (e->dir) return;
    if (force_hex) view_hex(p);
    else if (force_text) view_text(p);
    else if (is_bmp_ext(p)) { if (!view_bmp(p)) { draw_list(); ui_message(tr(T_ERROR), tr(T_IMG_UNSUPPORTED), NULL, NULL, false); } }
    else if (is_text_ext(p)) view_text(p);
    else view_hex(p);
}

enum { A_OPEN = 1, A_TEXT, A_HEX, A_COPY, A_CUT, A_PASTE, A_CLEAR, A_RENAME, A_DELETE, A_NEWDIR,
       A_INFO, A_MARKALL, A_UNMARKALL, A_SD, A_EXIT };

static int actions_menu(void)
{
    odroid_dialog_choice_t ch[20];
    int n = 0;
    static char paste_label[64];
    bool have = g_count > 0;
    bool file = have && !g_ent[g_cursor].dir && !g_marked;
    if (have && !g_marked) ch[n++] = (odroid_dialog_choice_t){ A_OPEN, tr(T_OPEN), "", 1, NULL };
    if (file) {
        ch[n++] = (odroid_dialog_choice_t){ A_TEXT, tr(T_VIEW_TEXT), "", 1, NULL };
        ch[n++] = (odroid_dialog_choice_t){ A_HEX, tr(T_VIEW_HEX), "", 1, NULL };
    }
    if (have) {
        ch[n++] = (odroid_dialog_choice_t){ A_COPY, tr(T_COPY), "", 1, NULL };
        ch[n++] = (odroid_dialog_choice_t){ A_CUT, tr(T_CUT), "", 1, NULL };
    }
    if (g_clip_n) {
        snprintf(paste_label, sizeof paste_label, "%s (%d)", tr(T_PASTE), g_clip_n);
        ch[n++] = (odroid_dialog_choice_t){ A_PASTE, paste_label, "", 1, NULL };
    }
    if (have && !g_marked) ch[n++] = (odroid_dialog_choice_t){ A_RENAME, tr(T_RENAME), "", 1, NULL };
    if (have) ch[n++] = (odroid_dialog_choice_t){ A_DELETE, tr(T_DELETE), "", 1, NULL };
    ch[n++] = (odroid_dialog_choice_t){ A_NEWDIR, tr(T_NEWDIR), "", 1, NULL };
    if (have && !g_marked) ch[n++] = (odroid_dialog_choice_t){ A_INFO, tr(T_INFO), "", 1, NULL };
    ch[n++] = (odroid_dialog_choice_t)ODROID_DIALOG_CHOICE_SEPARATOR;
    if (have) ch[n++] = (odroid_dialog_choice_t){ A_MARKALL, tr(T_MARK_ALL), "", 1, NULL };
    if (g_marked) ch[n++] = (odroid_dialog_choice_t){ A_UNMARKALL, tr(T_UNMARK_ALL), "", 1, NULL };
    if (g_clip_n) ch[n++] = (odroid_dialog_choice_t){ A_CLEAR, tr(T_CLEAR_CLIP), "", 1, NULL };
    ch[n++] = (odroid_dialog_choice_t){ A_SD, tr(T_SDINFO), "", 1, NULL };
    ch[n++] = (odroid_dialog_choice_t){ A_EXIT, tr(T_EXIT), "", 1, NULL };
    ch[n++] = (odroid_dialog_choice_t)ODROID_DIALOG_CHOICE_LAST;
    dialog_prepare();
    return odroid_overlay_dialog(tr(T_ACTIONS), ch, 0, &repaint_current, 0);
}

/* ================================================================ main */

void app_main(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    (void)load_state; (void)start_paused; (void)save_slot;
    ram_start = (uint32_t)(uintptr_t)&__CORE_BSS_END__;

    odroid_system_init(APPID_HOMEBREW, 32000);
    odroid_system_emu_init(NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    common_emu_state.pause_after_frames = 0;
    lcd_clear_buffers();
    printf("File Manager: fast rename %s, free space %s\n",
           op_fast_rename_available() ? "yes" : "no", fs_free_space(NULL, NULL) ? "yes" : "no");

    load_dir(NULL);
    for (;;) {
        draw_list();
        uint32_t k = input_wait();
        if (k & K_UP)    cursor_move(-1);
        if (k & K_DOWN)  cursor_move(1);
        if (k & K_LEFT)  cursor_move(-LIST_ROWS);
        if (k & K_RIGHT) cursor_move(LIST_ROWS);
        if ((k & K_TIME) && g_count) {
            entry_t *e = &g_ent[g_cursor];
            e->mark = !e->mark;
            g_marked += e->mark ? 1 : -1;
            cursor_move(1);
        }
        if (k & K_A) open_entry(false, false);
        if ((k & K_B) && strcmp(g_cwd, "/") != 0) {
            char child[256];
            snprintf(child, sizeof child, "%s", path_base(g_cwd));
            path_parent(g_cwd);
            load_dir(child);
        }
        if (k & K_GAME) {
            int action = actions_menu();
            draw_list();   /* the next dialog's background must be the list, not the closed menu */
            switch (action) {
                case A_OPEN:   open_entry(false, false); break;
                case A_TEXT:   open_entry(true, false); break;
                case A_HEX:    open_entry(false, true); break;
                case A_COPY:   clip_set(false); break;
                case A_CUT:    clip_set(true); break;
                case A_PASTE:  do_paste(); break;
                case A_CLEAR:  g_clip_n = 0; break;
                case A_RENAME: do_rename(); break;
                case A_DELETE: do_delete(); break;
                case A_NEWDIR: do_newdir(); break;
                case A_INFO:   do_info(); break;
                case A_MARKALL:
                    for (int i = 0; i < g_count; i++) g_ent[i].mark = 1;
                    g_marked = g_count;
                    break;
                case A_UNMARKALL:
                    for (int i = 0; i < g_count; i++) g_ent[i].mark = 0;
                    g_marked = 0;
                    break;
                case A_SD:     do_sdinfo(); break;
                case A_EXIT:   odroid_system_switch_app(0); break;
                default: break;
            }
        }
    }
}
