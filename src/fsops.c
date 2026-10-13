/*
 * File Manager — paths and file operations (recursive copy / move / delete, sizes).
 *
 * Everything goes through FatFs (f_opendir/f_readdir) and the stdio calls the firmware
 * exposes on its ABI. Recursion shares two path buffers instead of one per level, so deep
 * trees do not eat the ~24 KB stack.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "gw_lcd.h"
#include "odroid_overlay.h"
#include "rg_storage.h"
#include "ff.h"
#include "retro-go/gw_firmware_abi.h"

#include "fm.h"

extern void wdog_refresh(void);
extern uint32_t HAL_GetTick(void);

uint8_t fm_scratch[FM_SCRATCH_SIZE] __attribute__((aligned(32)));

/* ================================================================ paths */

void path_join(char *out, const char *dir, const char *name)
{
    if (strcmp(dir, "/") == 0)
        snprintf(out, FM_PATH_MAX, "/%s", name);
    else
        snprintf(out, FM_PATH_MAX, "%s/%s", dir, name);
}

const char *path_base(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

void path_parent(char *path)
{
    char *s = strrchr(path, '/');
    if (!s || s == path)
        strcpy(path, "/");
    else
        *s = '\0';
}

bool path_is_inside(const char *path, const char *dir)
{
    size_t n = strlen(dir);
    if (strcmp(dir, "/") == 0)
        return true;
    return strncmp(path, dir, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

bool name_valid(const char *name)
{
    if (!name[0] || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return false;
    for (const char *p = name; *p; p++)
        if (strchr("\\/:*?\"<>|", *p) || (uint8_t)*p < 0x20)
            return false;
    size_t n = strlen(name);
    if (name[n - 1] == ' ' || name[n - 1] == '.')   /* FAT strips these: avoid surprises */
        return false;
    return n <= 255;
}

void fmt_size(char *out, uint64_t b)
{
    static const char unit[] = "KMGT";
    if (b < 1000) { sprintf(out, "%u B", (unsigned)b); return; }
    double v = (double)b;
    int u = -1;
    while (v >= 1000.0 && u < 3) { v /= 1024.0; u++; }
    if (v < 10.0) sprintf(out, "%u.%u%c", (unsigned)v, (unsigned)(v * 10) % 10, unit[u]);
    else          sprintf(out, "%u%c", (unsigned)(v + 0.5), unit[u]);
}

/* ================================================================ optional firmware calls */
/* Appended to gw_firmware_abi_t after the 908-byte v2 layout this SDK knows (see README):
 *   +908 bool (*rg_storage_rename)(const char *old_path, const char *new_path);
 *   +912 bool (*rg_storage_free_space)(const char *path, uint64_t *free, uint64_t *total);
 * Present only if the firmware's `size` covers them; older firmware simply lacks them. */
_Static_assert(sizeof(gw_firmware_abi_t) == 908, "SDK ABI layout changed: re-check the ext offsets");
#define EXT_RENAME_OFF  908u
#define EXT_FREE_OFF    912u

static void *abi_ext(uint32_t off)
{
    const gw_firmware_abi_t *abi = gw_firmware_abi();
    if (abi->size < off + sizeof(void *))
        return NULL;
    return *(void *const *)((const uint8_t *)abi + off);
}

bool op_fast_rename_available(void) { return abi_ext(EXT_RENAME_OFF) != NULL; }

bool fs_rename(const char *src, const char *dst)
{
    bool (*fn)(const char *, const char *) = (bool (*)(const char *, const char *))abi_ext(EXT_RENAME_OFF);
    return fn && fn(src, dst);
}

bool fs_free_space(uint64_t *free_bytes, uint64_t *total_bytes)
{
    bool (*fn)(const char *, uint64_t *, uint64_t *) =
        (bool (*)(const char *, uint64_t *, uint64_t *))abi_ext(EXT_FREE_OFF);
    return fn && fn("/", free_bytes, total_bytes);
}

/* ================================================================ queries */

bool fs_is_dir(const char *path)
{
    if (strcmp(path, "/") == 0)
        return true;
    rg_stat_t st = rg_storage_stat(path);
    return st.exists && st.is_dir;
}

bool fs_exists(const char *path)
{
    if (strcmp(path, "/") == 0)
        return true;
    return rg_storage_stat(path).exists;
}

/* ================================================================ progress */

static struct {
    const char *title;
    uint64_t total_bytes, done_bytes;
    uint32_t total_items, done_items;
    int last_pct;
    uint32_t last_draw;
    bool cancelled;
} op;

void op_begin(const char *title, uint64_t total_bytes, uint32_t total_items)
{
    memset(&op, 0, sizeof op);
    op.title = title;
    op.total_bytes = total_bytes;
    op.total_items = total_items;
    op.last_pct = -1;
    /* Both LCD buffers get the current screen, so the bar does not flicker over two backgrounds */
    lcd_sleep_while_swap_pending();
    lcd_clone();
    odroid_overlay_draw_progress_bar_cancellable(title, 0);   /* resets the B-to-cancel latch */
    lcd_swap();
}

/* Called often: polls B (cancel), redraws the bar when the percentage moves. */
static bool op_tick(void)
{
    wdog_refresh();
    if (odroid_overlay_progress_poll_cancel())
        op.cancelled = true;
    if (op.cancelled)
        return false;
    int pct;
    if (op.total_bytes)
        pct = (int)(op.done_bytes * 100 / op.total_bytes);
    else if (op.total_items)
        pct = (int)((uint64_t)op.done_items * 100 / op.total_items);
    else
        pct = 0;
    if (pct > 100) pct = 100;
    uint32_t now = HAL_GetTick();
    if ((pct != op.last_pct || now - op.last_draw > 500) && !lcd_is_swap_pending()) {
        if (!odroid_overlay_draw_progress_bar_cancellable(op.title, (uint8_t)(pct ? pct : 1)))
            op.cancelled = true;
        lcd_swap();
        op.last_pct = pct;
        op.last_draw = now;
    }
    return !op.cancelled;
}

/* ================================================================ counting */

static char g_a[FM_PATH_MAX], g_b[FM_PATH_MAX];   /* shared recursion buffers */

static bool count_rec(fs_count_t *c)   /* g_a = directory ("" = root) */
{
    DIR d;
    FILINFO fi;
    if (f_opendir(&d, g_a[0] ? g_a : "/") != FR_OK)
        return false;
    size_t len = strlen(g_a);
    bool ok = true;
    while (ok && f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        if (!op_tick()) { ok = false; break; }
        if (fi.fattrib & AM_DIR) {
            c->dirs++;
            if (len + 1 + strlen(fi.fname) >= FM_PATH_MAX) { ok = false; break; }
            g_a[len] = '/'; strcpy(g_a + len + 1, fi.fname);
            ok = count_rec(c);
            g_a[len] = '\0';
        } else {
            c->files++;
            c->bytes += fi.fsize;
        }
    }
    f_closedir(&d);
    return ok;
}

bool fs_count(const char *path, fs_count_t *out)
{
    memset(out, 0, sizeof *out);
    if (!fs_is_dir(path)) {
        rg_stat_t st = rg_storage_stat(path);
        if (!st.exists)
            return false;
        out->files = 1;
        out->bytes = st.size;
        return true;
    }
    strncpy(g_a, path, FM_PATH_MAX - 1);
    g_a[FM_PATH_MAX - 1] = '\0';
    if (strcmp(g_a, "/") == 0) g_a[0] = '\0';   /* root: children become "/name" */
    return count_rec(out);
}

/* ================================================================ copy */

static op_result_t copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in)
        return OP_ERROR;
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return OP_ERROR;
    }
    op_result_t r = OP_OK;
    const size_t chunk = 32 * 1024;   /* small enough to keep the bar and B responsive */
    for (;;) {
        size_t n = fread(fm_scratch, 1, chunk, in);
        if (n == 0)
            break;
        if (fwrite(fm_scratch, 1, n, out) != n) { r = OP_ERROR; break; }
        op.done_bytes += n;
        if (!op_tick()) { r = OP_CANCELLED; break; }
    }
    fclose(in);
    fclose(out);
    if (r != OP_OK)
        rg_storage_delete(dst);   /* never leave a half-copied file behind */
    else
        op.done_items++;
    return r;
}

static op_result_t copy_rec(void)   /* g_a = source dir, g_b = destination dir (exists) */
{
    DIR d;
    FILINFO fi;
    if (f_opendir(&d, g_a[0] ? g_a : "/") != FR_OK)
        return OP_ERROR;
    size_t la = strlen(g_a), lb = strlen(g_b);
    op_result_t r = OP_OK;
    while (r == OP_OK && f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        size_t n = strlen(fi.fname);
        if (la + 1 + n >= FM_PATH_MAX || lb + 1 + n >= FM_PATH_MAX) { r = OP_ERROR; break; }
        g_a[la] = '/'; strcpy(g_a + la + 1, fi.fname);
        g_b[lb] = '/'; strcpy(g_b + lb + 1, fi.fname);
        if (fi.fattrib & AM_DIR) {
            if (!rg_storage_mkdir(g_b)) r = OP_ERROR;
            else { op.done_items++; r = copy_rec(); }
        } else {
            r = copy_file(g_a, g_b);
        }
        g_a[la] = '\0';
        g_b[lb] = '\0';
        if (r == OP_OK && !op_tick())
            r = OP_CANCELLED;
    }
    f_closedir(&d);
    return r;
}

op_result_t op_copy(const char *src, const char *dst)
{
    if (!fs_is_dir(src))
        return copy_file(src, dst);
    if (path_is_inside(dst, src))
        return OP_ERROR;   /* a folder into itself would never end */
    if (!rg_storage_mkdir(dst))
        return OP_ERROR;
    op.done_items++;
    strncpy(g_a, src, FM_PATH_MAX - 1); g_a[FM_PATH_MAX - 1] = '\0';
    strncpy(g_b, dst, FM_PATH_MAX - 1); g_b[FM_PATH_MAX - 1] = '\0';
    if (strcmp(g_a, "/") == 0) g_a[0] = '\0';
    return copy_rec();
}

/* ================================================================ delete */

static op_result_t delete_rec(void)   /* g_a = directory: empties it, then removes it */
{
    DIR d;
    FILINFO fi;
    if (f_opendir(&d, g_a) != FR_OK)
        return OP_ERROR;
    size_t la = strlen(g_a);
    op_result_t r = OP_OK;
    /* FatFs allows unlinking entries of a directory while reading it (its own delete_node
     * example does exactly this): entries are only marked as deleted. */
    while (r == OP_OK && f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        size_t n = strlen(fi.fname);
        if (la + 1 + n >= FM_PATH_MAX) { r = OP_ERROR; break; }
        g_a[la] = '/'; strcpy(g_a + la + 1, fi.fname);
        if (fi.fattrib & AM_DIR) {
            r = delete_rec();
        } else {
            if (remove(g_a) != 0 && !rg_storage_delete(g_a)) r = OP_ERROR;
            else { op.done_items++; op.done_bytes += fi.fsize; }
        }
        g_a[la] = '\0';
        if (r == OP_OK && !op_tick())
            r = OP_CANCELLED;
    }
    f_closedir(&d);
    if (r == OP_OK) {
        if (!rg_storage_delete(g_a)) r = OP_ERROR;   /* now empty: rmdir */
        else op.done_items++;
    }
    return r;
}

op_result_t op_delete(const char *path)
{
    if (strcmp(path, "/") == 0)
        return OP_ERROR;
    if (!fs_is_dir(path)) {
        rg_stat_t st = rg_storage_stat(path);
        if (remove(path) != 0 && !rg_storage_delete(path))
            return OP_ERROR;
        op.done_items++;
        op.done_bytes += st.size;
        return op_tick() ? OP_OK : OP_CANCELLED;
    }
    strncpy(g_a, path, FM_PATH_MAX - 1);
    g_a[FM_PATH_MAX - 1] = '\0';
    return delete_rec();
}

/* ================================================================ move */

op_result_t op_move(const char *src, const char *dst)
{
    if (strcmp(src, dst) == 0)
        return OP_OK;
    if (fs_is_dir(src) && path_is_inside(dst, src))
        return OP_ERROR;
    if (fs_rename(src, dst)) {          /* same card: just a directory-entry change */
        op.done_items++;
        return op_tick() ? OP_OK : OP_CANCELLED;
    }
    op_result_t r = op_copy(src, dst);
    if (r != OP_OK)
        return r;
    /* Copy finished: remove the original (its bytes were already counted by the copy) */
    uint64_t keep = op.done_bytes;
    r = op_delete(src);
    op.done_bytes = keep;
    return r;
}
