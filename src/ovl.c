/*
 * File Manager — overlays: the viewers and players are not resident. Each group is linked to the
 * same RAM window (see ld/filemanager.ld) and stored after the packed homebrew in FileManager.bin
 * (tools/append_overlays.py); the one a file needs is read into the window when it is opened.
 *
 *   OVL_TEXT   text viewer, hex viewer/editor, text editor, editor keyboard
 *   OVL_IMAGE  image viewer (JPEG, PNG, GIF, BMP)
 *   OVL_MEDIA  video and audio players (MP3, FLAC, WAV, AIFF decoders)
 *
 * Footer at the very end of the file (little-endian u32):
 *   magic 'FMOV', version, count, then FM_OVL_MAX x { file offset, load size, total size }
 * (total = load + zeroed BSS).
 */
#include <stdio.h>
#include <string.h>
#include "fm.h"

#include <odroid_system.h>
#include "rg_storage.h"
#include "rom_manager.h"
#include "gw_core_bridge.h"

extern uint8_t __FM_OVL_START__[];
extern uint8_t __FM_OVL_END__[];

#define FM_OVL_MAGIC   0x564F4D46u          /* "FMOV" */
#define FM_OVL_VERSION 1u
#define FM_OVL_MAX     4
#define FOOTER_WORDS   (3 + FM_OVL_MAX * 3)

static struct { uint32_t off, load, total; } g_tab[FM_OVL_MAX];
static int g_ready;                         /* 0 not read yet, 1 ok, -1 missing */
static int g_loaded = -1;
static char g_path[256];

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static bool read_footer(void)
{
    if (g_ready) return g_ready > 0;
    g_ready = -1;
    retro_emulator_file_t *self = ACTIVE_FILE;
    if (!self || !self->path[0]) return false;
    snprintf(g_path, sizeof g_path, "%s", self->path);
    FILE *f = fopen(g_path, "rb");
    if (!f) return false;
    uint8_t b[FOOTER_WORDS * 4];
    bool ok = fseek(f, 0, SEEK_END) == 0;
    long size = ok ? ftell(f) : 0;
    ok = ok && size > (long)sizeof b && fseek(f, size - (long)sizeof b, SEEK_SET) == 0 &&
         fread(b, 1, sizeof b, f) == sizeof b;
    fclose(f);
    if (!ok || le32(b) != FM_OVL_MAGIC || le32(b + 4) != FM_OVL_VERSION) return false;
    uint32_t n = le32(b + 8);
    if (n > FM_OVL_MAX) return false;
    for (uint32_t i = 0; i < n; i++) {
        g_tab[i].off = le32(b + 12 + i * 12);
        g_tab[i].load = le32(b + 16 + i * 12);
        g_tab[i].total = le32(b + 20 + i * 12);
        if ((uint64_t)g_tab[i].off + g_tab[i].load > (uint64_t)size) return false;
    }
    g_ready = 1;
    return true;
}

/* The window holds code: clean the data cache over it and drop stale instructions. */
static void sync_caches(void *p, uint32_t n)
{
    volatile uint32_t *const DCCMVAC = (volatile uint32_t *)0xE000EF68u;
    volatile uint32_t *const ICIALLU = (volatile uint32_t *)0xE000EF50u;
    uintptr_t a = (uintptr_t)p & ~31u, e = (uintptr_t)p + n;
    __asm volatile("dsb" ::: "memory");
    for (; a < e; a += 32) *DCCMVAC = (uint32_t)a;
    __asm volatile("dsb" ::: "memory");
    *ICIALLU = 0;
    __asm volatile("dsb\n\tisb" ::: "memory");
}

bool fm_ovl_use(int id)
{
    if (id == g_loaded) return true;
    uint32_t room = (uint32_t)(__FM_OVL_END__ - __FM_OVL_START__);
    if (id < 0 || id >= FM_OVL_MAX || !read_footer() || !g_tab[id].load || g_tab[id].total > room ||
        g_tab[id].load > g_tab[id].total) {
        ui_message(tr(T_ERROR), tr(T_OVL_MISSING), NULL, NULL, false);
        return false;
    }
    g_loaded = -1;
    size_t got = rg_storage_copy_file_range_to_ram(g_path, __FM_OVL_START__, g_tab[id].off, g_tab[id].load, NULL);
    if (got != g_tab[id].load) {
        ui_message(tr(T_ERROR), tr(T_OVL_MISSING), NULL, NULL, false);
        return false;
    }
    memset(__FM_OVL_START__ + g_tab[id].load, 0, g_tab[id].total - g_tab[id].load);
    sync_caches(__FM_OVL_START__, g_tab[id].total);
    g_loaded = id;
    return true;
}
