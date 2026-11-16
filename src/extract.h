/* File Manager — archive extraction (tools overlay): extract.c (common), unzip.c.
 * Plain C over stdio plus the few calls below, so the same files build on a PC to be tested. */
#pragma once
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

enum { EX_OK, EX_CANCELLED, EX_BAD_ARCHIVE, EX_NOT_SUPPORTED, EX_WRITE_ERROR, EX_NO_MEMORY };

typedef struct {
    int status;              /* EX_* for the archive as a whole */
    uint32_t files, dirs;    /* written */
    uint32_t skipped;        /* already there, the user said skip */
    uint32_t unsupported;    /* entries in a method this cannot unpack (or encrypted) */
    uint32_t bad;            /* entries that came out damaged (CRC) or could not be written */
    char first_problem[96];  /* the first unsupported/bad entry's name */
    char detail[64];         /* what kind of archive problem (EX_BAD_ARCHIVE / EX_NOT_SUPPORTED) */
} ex_result_t;

/* Extract everything in the archive into the folder dest (created if needed). */
void unzip_extract(const char *archive, const char *dest, ex_result_t *r);

/* ---------------------------------------------------------------- extract.c */
typedef struct {
    const char *dest;
    ex_result_t *r;
    bool overwrite_all;
    uint64_t total, done;    /* uncompressed bytes, for the progress bar */
} ex_job_t;

void ex_begin(ex_job_t *j, const char *dest, ex_result_t *r, uint64_t total);
/* The path inside the archive -> the path on the card under dest, made safe (no "..", no
 * absolute paths, no characters FAT refuses). cp437: the name is in the DOS code page (zip
 * without the UTF-8 flag). false = nothing usable. */
bool ex_target(ex_job_t *j, const char *name, bool cp437, char *out, size_t size);
bool ex_mkdirs(const char *path, bool last_too);   /* the folders up to path (and path itself) */
/* Opens path for writing: NULL with *skip set when the user chose to keep the existing file,
 * NULL with r->status = EX_CANCELLED when cancelled, NULL otherwise = cannot write. */
FILE *ex_create(ex_job_t *j, const char *path, bool *skip);
void ex_problem(ex_job_t *j, const char *name);    /* note the first problem's name */
bool ex_progress(ex_job_t *j, uint64_t add);       /* false = cancelled */

/* Provided by the app (main.c on the console, the test harness on a PC) */
enum { EXQ_OVERWRITE = 1, EXQ_OVERWRITE_ALL, EXQ_SKIP, EXQ_CANCEL };
int  ex_ask_exists(const char *name);
bool ex_path_exists(const char *path, bool *is_dir);
bool ex_mkdir(const char *path);
bool ex_remove(const char *path);
bool ex_report(uint64_t done, uint64_t total);      /* progress; false = cancelled */
extern uint8_t *ex_mem;                             /* work memory (fm_scratch) */
extern uint32_t ex_mem_size;
