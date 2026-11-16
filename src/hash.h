/* File Manager — CRC32, MD5, SHA-1 (hash.c) */
#pragma once
#include <stdint.h>
#include <stddef.h>

void crc32_init(uint32_t *crc);
void crc32_update(uint32_t *crc, const uint8_t *p, size_t n);
uint32_t crc32_final(const uint32_t *crc);

typedef struct { uint32_t h[4]; uint64_t len; uint8_t buf[64]; } md5_t;
void md5_init(md5_t *m);
void md5_update(md5_t *m, const uint8_t *p, size_t n);
void md5_final(md5_t *m, uint8_t out[16]);

typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; } sha1_t;
void sha1_init(sha1_t *s);
void sha1_update(sha1_t *s, const uint8_t *p, size_t n);
void sha1_final(sha1_t *s, uint8_t out[20]);

void hex_str(char *out, const uint8_t *p, size_t n);   /* lowercase, NUL-terminated */
