/*
 * File Manager — which viewer or player opens a file. Resident (not in an overlay): the list
 * uses these checks to pick the overlay to load and to browse the folder from the viewers.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "fm.h"

static bool ext_in(const char *name, const char *const *list)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    for (; *list; list++)
        if (strcasecmp(dot + 1, *list) == 0) return true;
    return false;
}

/* macOS writes a "._name" companion (AppleDouble metadata, not a picture) for every file it
 * copies to a FAT card: never treat those as images, videos or music. */
bool is_macos_meta(const char *name)
{
    const char *b = strrchr(name, '/');
    b = b ? b + 1 : name;
    return b[0] == '.' && b[1] == '_';
}

bool is_text_ext(const char *name)
{
    static const char *const L[] = { "txt", "cfg", "ini", "log", "md", "json", "xml", "csv", "nfo",
                                     "c", "h", "py", "sh", "lua", "p8", "cue", "m3u", "dat", "pceplus",
                                     "ggcodes", "mcf", "yml", "yaml", "htm", "html", NULL };
    return ext_in(name, L);
}

bool is_image_ext(const char *name)
{
    if (is_macos_meta(name)) return false;
    static const char *const L[] = { "jpg", "jpeg", "jpe", "png", "gif", "bmp", NULL };
    return ext_in(name, L);
}

/* Also Retro-Go's ".img" cover thumbnails (JPEG inside). ".img" is a disk image too, so for that
 * extension the first bytes decide. */
bool is_image_file(const char *path)
{
    if (is_image_ext(path)) return true;
    static const char *const I[] = { "img", NULL };
    if (is_macos_meta(path) || !ext_in(path, I)) return false;
    uint8_t m[4] = { 0 };
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(m, 1, sizeof m, f);
    fclose(f);
    if (n < 4) return false;
    return (m[0] == 0xFF && m[1] == 0xD8 && m[2] == 0xFF) || (m[0] == 0x89 && m[1] == 'P' && m[2] == 'N' && m[3] == 'G') ||
           (m[0] == 'G' && m[1] == 'I' && m[2] == 'F') || (m[0] == 'B' && m[1] == 'M');
}

bool is_video_ext(const char *name)
{
    if (is_macos_meta(name)) return false;
    static const char *const L[] = { "avi", "mp4", "m4v", "mkv", "mov", "webm", "wmv", "flv", "mpg", "mpeg", "3gp", "ts", NULL };
    return ext_in(name, L);
}

static const char *const PLAYABLE[] = { "wav", "wave", "mp3", "mp2", "mpa", "flac", "fla", "aif", "aiff", "aifc", "ogg", "oga", NULL };
static const char *const OTHER[]    = { "opus", "m4a", "aac", "wma",
                                        "ape", "wv", "mid", "midi", "mod", "xm", "s3m", "it", "amr", "ac3", NULL };

bool is_audio_playable_ext(const char *name) { return ext_in(name, PLAYABLE); }

bool is_audio_ext(const char *name)
{
    if (is_macos_meta(name)) return false;
    return ext_in(name, PLAYABLE) || ext_in(name, OTHER);
}

/* What the tools overlay extracts: .zip, .7z and the first part of a split 7z (.7z.001, or any
 * "x.001" whose name says 7z) */
bool is_archive_name(const char *name)
{
    if (is_macos_meta(name)) return false;
    static const char *const L[] = { "zip", "7z", NULL };
    if (ext_in(name, L)) return true;
    size_t n = strlen(name);
    if (n < 5 || strcmp(name + n - 4, ".001") != 0) return false;
    for (size_t i = 0; i + 2 <= n - 4; i++)
        if (strncasecmp(name + i, "7z", 2) == 0) return true;
    return false;
}
