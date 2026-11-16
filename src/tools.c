/*
 * File Manager — tools overlay: check a file (CRC32 / MD5 / SHA-1) and extract .zip archives.
 * The archive code itself (extract.c, unzip.c) is plain C that also
 * builds on a PC (tools/test/run.sh); this file is the console's side of it: dialogs, the
 * progress bar, the result screen.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "fm.h"
#include "hash.h"
#include "extract.h"
#include "rg_storage.h"

extern void wdog_refresh(void);

uint8_t *ex_mem = fm_scratch;
uint32_t ex_mem_size = FM_SCRATCH_SIZE;
static const char *g_title;

/* ================================================================ check a file */

void tool_hash(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { ui_message(tr(T_ERROR), path_base(path), NULL, NULL, false); return; }
    fseek(f, 0, SEEK_END);
    uint64_t size = (uint64_t)ftell(f), done = 0;
    fseek(f, 0, SEEK_SET);
    uint32_t crc;
    static md5_t md5;
    static sha1_t sha;
    crc32_init(&crc);
    md5_init(&md5);
    sha1_init(&sha);
    op_begin(tr(T_HASHING), size, 0);
    bool ok = true;
    for (;;) {
        size_t n = fread(fm_scratch, 1, 32 * 1024, f);
        if (!n) break;
        crc32_update(&crc, fm_scratch, n);
        md5_update(&md5, fm_scratch, n);
        sha1_update(&sha, fm_scratch, n);
        done += n;
        if (!op_progress(done)) { ok = false; break; }
    }
    fclose(f);
    if (!ok) return;
    if (done != size) { ui_message(tr(T_ERROR), path_base(path), NULL, NULL, false); return; }
    uint8_t d5[16], d1[20];
    char h5[33], h1[41], c[16], sz[16], line[64];
    md5_final(&md5, d5);
    sha1_final(&sha, d1);
    hex_str(h5, d5, 16);
    hex_str(h1, d1, 20);
    snprintf(c, sizeof c, "%08lx", (unsigned long)crc32_final(&crc));
    fmt_size(sz, size);

    for (;;) {
        ui_begin();
        ui_banner(path_base(path), tr(T_HASH));
        int row = 5;
        snprintf(line, sizeof line, "%s: %s (%lu B)", tr(T_SIZE), sz, (unsigned long)size);
        ui_text(1, row, line, C_TEXT, C_BG);
        row += 3;
        ui_text(1, row++, "CRC32", C_DIR, C_BG);
        ui_text(1, row, c, C_TEXT, C_BG);
        row += 2;
        ui_text(1, row++, "MD5", C_DIR, C_BG);
        ui_text(1, row, h5, C_TEXT, C_BG);
        row += 2;
        ui_text(1, row++, "SHA-1", C_DIR, C_BG);
        ui_text(0, row, h1, C_TEXT, C_BG);                       /* (40 characters: the whole line) */
        ui_text_w(0, FM_ROWS - 1, FM_COLS, tr(T_BACK_HELP), C_BAR_TEXT, C_BAR);
        ui_end();
        uint32_t k = input_wait();
        if (k & (K_A | K_B | K_GAME)) return;
    }
}

/* ================================================================ extract */

int ex_ask_exists(const char *name)
{
    const char *lines[] = { tr(T_EXISTS), name };
    const char *buttons[] = { tr(T_OVERWRITE), tr(T_OVERWRITE_ALL), tr(T_SKIP), tr(T_CANCEL) };
    fm_draw_list();
    int r = ui_dialog(g_title, lines, 2, buttons, 4);
    fm_draw_list();
    op_resume(g_title);
    return r == 1 ? EXQ_OVERWRITE : r == 2 ? EXQ_OVERWRITE_ALL : r == 3 ? EXQ_SKIP : EXQ_CANCEL;
}

bool ex_path_exists(const char *path, bool *is_dir)
{
    if (strcmp(path, "/") == 0 || !path[0]) { *is_dir = true; return true; }
    rg_stat_t st = rg_storage_stat(path);
    if (!st.exists) return false;
    *is_dir = st.is_dir;
    return true;
}

bool ex_mkdir(const char *path) { return rg_storage_mkdir(path); }
bool ex_remove(const char *path) { return remove(path) == 0 || rg_storage_delete(path); }
bool ex_report(uint64_t done, uint64_t total) { return op_progress_of(done, total); }

/* Extracts archive into dest; true if anything changed in the current folder */
void tool_extract(const char *archive, const char *dest)
{
    static ex_result_t r;
    g_title = tr(T_EXTRACTING);
    op_begin(g_title, 0, 0);
    unzip_extract(archive, dest, &r);
    fm_draw_list();

    char l1[64], l2[64], l3[96];
    const char *lines[4];
    int nl = 0;
    if (r.status == EX_CANCELLED) {
        lines[nl++] = tr(T_CANCELLED);
    } else if (r.status == EX_BAD_ARCHIVE) {
        lines[nl++] = tr(T_ARC_BAD);
        if (r.detail[0]) lines[nl++] = r.detail;
    } else if (r.status == EX_NOT_SUPPORTED) {
        lines[nl++] = tr(T_ARC_UNSUPPORTED);
        if (r.detail[0]) lines[nl++] = r.detail;
    } else if (r.status == EX_NO_MEMORY) {
        lines[nl++] = tr(T_ARC_MEMORY);
        if (r.detail[0]) lines[nl++] = r.detail;
    } else if (r.status == EX_WRITE_ERROR) {
        lines[nl++] = tr(T_ERROR);
        lines[nl++] = dest;
    }
    if (r.status == EX_OK || r.status == EX_CANCELLED) {
        snprintf(l1, sizeof l1, "%s: %lu", tr(T_ARC_FILES), (unsigned long)r.files);
        lines[nl++] = l1;
        if (r.skipped || r.unsupported || r.bad) {
            int k = 0;
            l2[0] = '\0';
            if (r.skipped) k += snprintf(l2 + k, sizeof l2 - k, "%s %lu ", tr(T_ARC_SKIPPED), (unsigned long)r.skipped);
            if (r.unsupported) k += snprintf(l2 + k, sizeof l2 - k, "%s %lu ", tr(T_ARC_UNSUP_N), (unsigned long)r.unsupported);
            if (r.bad) k += snprintf(l2 + k, sizeof l2 - k, "%s %lu", tr(T_ARC_BAD_N), (unsigned long)r.bad);
            lines[nl++] = l2;
        }
        if (r.first_problem[0] && nl < 4) {
            snprintf(l3, sizeof l3, "%s %s", tr(r.unsupported && !r.bad ? T_ARC_UNSUP_N : T_ARC_FIRST), r.first_problem);
            lines[nl++] = l3;
        }
    }
    const char *buttons[] = { tr(T_OK) };
    ui_dialog(r.status == EX_OK && !r.bad && !r.unsupported ? tr(T_DONE) : path_base(archive), lines, nl, buttons, 1);
}
