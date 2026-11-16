/* PC test of the tools overlay's portable parts (hash.c, extract.c, unzip.c):
 *   tools/test/run.sh        builds this and checks it against Python's zipfile / hashlib */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "extract.h"
#include "hash.h"

static uint8_t mem[192 * 1024];
uint8_t *ex_mem = mem;
uint32_t ex_mem_size = sizeof mem;
static int answer = EXQ_OVERWRITE_ALL;
int ex_ask_exists(const char *name) { printf("exists: %s\n", name); return answer; }
bool ex_path_exists(const char *path, bool *is_dir)
{
    struct stat st;
    if (stat(path, &st) != 0) return false;
    *is_dir = S_ISDIR(st.st_mode);
    return true;
}
bool ex_mkdir(const char *path) { return mkdir(path, 0777) == 0; }
bool ex_remove(const char *path) { return remove(path) == 0; }
bool ex_report(uint64_t done, uint64_t total) { (void)done; (void)total; return true; }

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "hash") == 0) {
        FILE *f = fopen(argv[2], "rb");
        if (!f) return 1;
        uint32_t crc; md5_t m; sha1_t s;
        crc32_init(&crc); md5_init(&m); sha1_init(&s);
        size_t n;
        while ((n = fread(mem, 1, 4093, f)) > 0) { crc32_update(&crc, mem, n); md5_update(&m, mem, n); sha1_update(&s, mem, n); }
        fclose(f);
        uint8_t d5[16], d1[20]; char h5[33], h1[41];
        md5_final(&m, d5); sha1_final(&s, d1); hex_str(h5, d5, 16); hex_str(h1, d1, 20);
        printf("%08x %s %s\n", crc32_final(&crc), h5, h1);
        return 0;
    }
    if (argc >= 4) {
        if (argc >= 5) answer = atoi(argv[4]);
        ex_result_t r;
        unzip_extract(argv[2], argv[3], &r);
        printf("status %d files %u dirs %u skipped %u unsupported %u bad %u problem '%s' detail '%s'\n",
               r.status, r.files, r.dirs, r.skipped, r.unsupported, r.bad, r.first_problem, r.detail);
        return r.status;
    }
    return 2;
}
