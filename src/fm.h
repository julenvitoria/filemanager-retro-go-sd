/*
 * File Manager for Retro-Go SD (Game & Watch) — shared declarations.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define FM_PATH_MAX   512           /* longest path we handle (FatFs LFN: 255 per component) */
#define FM_COLS       40            /* 8x8 firmware font on a 320x240 LCD */
#define FM_ROWS       30
#define FM_W          320
#define FM_H          240

/* Colours (RGB565) */
#define C_BG          0x0000
#define C_TEXT        0xFFFF
#define C_DIR         0xFFE0        /* yellow */
#define C_DIM         0x8410        /* grey */
#define C_BAR         0x0320        /* dark green (title / help bars) */
#define C_BAR_TEXT    0xFFFF
#define C_SEL_BG      0xFC00        /* orange */
#define C_SEL_TEXT    0x0000
#define C_MARK        0x07FF        /* cyan */
#define C_WARN        0xF800

/* ------------------------------------------------------------------ text */
const char *tr(int id);             /* translated UI string (main.c) */
enum {
    T_TITLE, T_HELP, T_EMPTY, T_OPEN, T_COPY, T_CUT, T_PASTE, T_RENAME, T_DELETE, T_NEWDIR,
    T_INFO, T_VIEW_TEXT, T_VIEW_HEX, T_SDINFO, T_EXIT, T_MARK_ALL, T_UNMARK_ALL, T_ACTIONS,
    T_CONFIRM_DELETE, T_YES, T_NO, T_ITEMS, T_OVERWRITE, T_SKIP, T_CANCEL, T_EXISTS,
    T_COPYING, T_MOVING, T_DELETING, T_COUNTING, T_ERROR, T_INTO_ITSELF, T_NAME, T_SIZE,
    T_DATE, T_TYPE, T_FOLDER, T_FILE, T_CONTAINS, T_FREE, T_TOTAL, T_USED, T_UNKNOWN,
    T_CLIP_COPY, T_CLIP_CUT, T_OK, T_BAD_NAME, T_DONE, T_FAST_RENAME, T_SLOW_RENAME,
    T_OSK_HELP, T_VIEW_HELP, T_IMG_UNSUPPORTED, T_CANCELLED, T_CLEAR_CLIP, T_PATH,
    T_HEX_HELP, T_EDIT_HELP, T_BYTE_HELP, T_SAVE_CHANGES, T_SAVE, T_DISCARD, T_SAVED,
    T_TOO_MANY, T_CHANGES,
    T_COUNT
};

/* ------------------------------------------------------------------ drawing (main.c) */
void ui_begin(void);                /* wait for the LCD, clear the back buffer */
void ui_end(void);                  /* show it */
void ui_text(int col, int row, const char *s, uint16_t fg, uint16_t bg);
void ui_text_w(int col, int row, int width, const char *s, uint16_t fg, uint16_t bg); /* padded/clipped */
void ui_fill(int x, int y, int w, int h, uint16_t c);
uint16_t *ui_buffer(void);
int  ui_message(const char *title, const char *l1, const char *l2, const char *l3, bool yes_no);
/* Up to three buttons; returns 1..3, or 0 if closed with B. Default = first button. */
int  ui_choice(const char *title, const char *l1, const char *b1, const char *b2, const char *b3);

/* ------------------------------------------------------------------ input (main.c) */
enum { K_UP = 1, K_DOWN = 2, K_LEFT = 4, K_RIGHT = 8, K_A = 16, K_B = 32, K_GAME = 64, K_TIME = 128,
       K_PAUSE = 256, K_POWER = 512 };
uint32_t input_wait(void);          /* next key press (with auto-repeat for the D-pad) */
uint32_t input_poll(void);          /* currently held keys, no waiting */

/* ------------------------------------------------------------------ paths (fsops.c) */
void path_join(char *out, const char *dir, const char *name);
const char *path_base(const char *path);
void path_parent(char *path);       /* in place; "/" stays "/" */
bool path_is_inside(const char *path, const char *dir);   /* path == dir or below it */
bool name_valid(const char *name);  /* FAT-legal, not empty, not "." / ".." */
void fmt_size(char *out, uint64_t bytes);   /* "999 B", "12.3K", "4.5M", "1.2G" */

/* ------------------------------------------------------------------ file operations (fsops.c) */
typedef struct { uint64_t bytes; uint32_t files, dirs; } fs_count_t;
bool fs_is_dir(const char *path);
bool fs_exists(const char *path);
bool fs_count(const char *path, fs_count_t *out);        /* recursive; cancellable */
typedef enum { OP_OK, OP_ERROR, OP_CANCELLED } op_result_t;
/* Progress for a batch: set the total once, then every copy/delete reports into it. */
void op_begin(const char *title, uint64_t total_bytes, uint32_t total_items);
op_result_t op_copy(const char *src, const char *dst);   /* file or folder, recursive */
op_result_t op_move(const char *src, const char *dst);   /* rename if possible, else copy+delete */
op_result_t op_delete(const char *path);                 /* file or folder, recursive */
bool op_fast_rename_available(void);
bool fs_rename(const char *src, const char *dst);        /* firmware rename (fast) or false */
bool fs_free_space(uint64_t *free_bytes, uint64_t *total_bytes);

/* ------------------------------------------------------------------ keyboard (osk.c) */
bool osk_edit(const char *title, char *text, size_t size);   /* false = cancelled */

/* ------------------------------------------------------------------ viewers (viewer.c) */
void view_text(const char *path);
void view_hex(const char *path);
bool view_bmp(const char *path);    /* false if not a BMP we can show */
bool is_text_ext(const char *name);
bool is_bmp_ext(const char *name);

/* Scratch memory shared by the copy buffer and the viewers (never used at the same time) */
#define FM_SCRATCH_SIZE (192 * 1024)
extern uint8_t fm_scratch[FM_SCRATCH_SIZE];
