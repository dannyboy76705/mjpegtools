/*
 * mjpegstore - store video from a y4m stream as JPEG pictures and serve it
 *              back as MJPEG in Matroska; lists the stored pictures in
 *              natural order for archiving
 *
 * MIT License
 *
 * Copyright (c) 2026 Daniel Lee Witzel
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject
 * to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The same set / clip layout as mjpegstore, with JPEG pictures in place of raw
 * frames. A set directory holds specs.txt and one subdirectory per clip; a
 * clip holds frame-0000001.jpg, frame-0000002.jpg, ... . specs.txt:
 *
 *   resolution=1920x1080        sampling=420 (or 422, 444)
 *   framerate=24000:1001        bitdepth=8 (JPEG baseline is 8 bits)
 *   chromasiting=jpeg|mpeg2|paldv (4:2:0 only)     aspect=1:1 (pixel aspect N:M)
 *   quality=90 (JPEG quality 1..100)
 *   range=limited|full (how the stored Y/U/V values are to be read)
 *
 * WHY THE PICTURES ARE MADE THIS WAY. JPEG's entropy coding is variable-length
 * and differential, so one changed block normally shifts every bit after it and
 * no two frames share bytes. Here every picture is encoded with the same
 * quantization tables (fixed quality) and the standard Huffman tables (never
 * per-picture optimised ones), and with a restart marker after every block of
 * 16x16 pixels (-R 1, the default; -R N puts one every N blocks, -R 0 one per
 * row of blocks). A restart byte-aligns the stream and resets the DC prediction,
 * so a piece's bytes depend only on that piece's pixels: where a piece is
 * unchanged from one frame to the next, the bytes are the same, at a byte
 * boundary, and a compressor that looks back (zstd, xz) can match
 * them. Noise then spoils only the pieces it touches, not whole rows: on an
 * anime episode -R 1 gave archives 17% smaller than one piece per row. The
 * cost is a larger raw file; optimised Huffman tables would save about 6% and
 * destroy the matching. The planes of the y4m frame go into the JPEG as they
 * are (no colour conversion), with the exact integer DCT, so the same input
 * gives the same bytes on any machine.
 *
 * PROGRESSIVE FOOTAGE ONLY. Every frame is one whole picture. Interlaced footage
 * (a y4m stream tagged It, Ib or Im) is refused: deinterlace it first (to 60p,
 * say) and feed progressive frames. Storing fields as separate pictures was
 * measured to cost about 16% in archive size on progressive footage, and a
 * JPEG of woven fields wastes its bits on the comb, so there is no mode for it.
 *
 * Serve mode (-s): "mjpegstore -s <clipdir>" writes a Matroska stream with one
 * V_MJPEG track on stdout, or to -f FILE. The pictures are copied in untouched;
 * the container is built from the file sizes, so it is exact and seekable.
 *
 * LISTING: the list goes to stdout (or -f), in natural filename order and
 * nothing else, so it can feed an archiver; it begins with the set's
 * specs.txt. Several clips (episodes) can be listed together, in the order
 * given, so a run of them can be cut into disc-size sets.
 *
 * Build:
 *   gcc -O2 -Wall -Wextra -pthread -o mjpegstore mjpegstore.c -ljpeg
 * (libjpeg-turbo recommended: libjpeg-turbo8-dev / libjpeg-turbo-devel)
 *
 * Usage: see -h.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <ctype.h>
#include <strings.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <setjmp.h>
#include <jpeglib.h>

/* ---------------------------------------------------------------- */
/* Progress bar (stderr only, throttled to ~10 updates/sec)         */
/* ---------------------------------------------------------------- */

static void progress_bar(const char *label, size_t done, size_t total) {
    static struct timespec last_print;
    static int have_last = 0;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (have_last && done != total) {
        double dt = (double)(now.tv_sec - last_print.tv_sec) +
                    (double)(now.tv_nsec - last_print.tv_nsec) / 1e9;
        if (dt < 0.1) return; /* throttle so huge n doesn't flood stderr */
    }
    last_print = now;
    have_last = 1;

    const int width = 30;
    double frac = total ? (double)done / (double)total : 1.0;
    if (frac > 1.0) frac = 1.0;
    int filled = (int)(frac * width);

    fprintf(stderr, "\r%-10s [", label);
    for (int i = 0; i < width; i++) fputc(i < filled ? '#' : '-', stderr);
    fprintf(stderr, "] %5.1f%% (%zu/%zu)", frac * 100.0, done, total);
    if (done >= total) fputc('\n', stderr);
    fflush(stderr);
}

/* ---------------------------------------------------------------- */
/* specs.txt: raw YUV properties shared by every clip in a set       */
/*                                                                   */
/* Directory layout:                                                 */
/*     <set>/specs.txt      short file, common to every clip         */
/*     <set>/<clip>/...     one subdirectory per clip                */
/*                                                                   */
/* specs.txt is key=value lines, in yuv4mpeg (y4m) terms:            */
/*     resolution=640x480      -> W640 H480                          */
/*     sampling=420            -> C420jpeg  (C422, C444)             */
/*     framerate=24000:1001    -> F24000:1001                        */
/*     bitdepth=8              -> no suffix (10 -> C420p10 etc)      */
/*     chromasiting=mpeg2      -> C420mpeg2 (4:2:0, 8-bit; jpeg, paldv)*/
/*     aspect=10:11            -> A10:11 (pixel aspect ratio N:M)    */
/* Blank lines and lines starting with '#' are ignored. If it is     */
/* missing, the user is asked for all six values and it is written.  */
/* aspect: 1:1 square pixels; presets 10:11 (NTSC 4:3), 40:33        */
/* (NTSC 16:9), 12:11 (PAL 4:3), 16:11 (PAL 16:9) -- any N:M works.   */
/* A specs.txt without an aspect line gets "aspect=1:1" appended,    */
/* with a message. An old fieldorder=progressive line is accepted    */
/* and ignored; fieldorder=tff or bff is refused (progressive only). */
/* ---------------------------------------------------------------- */

#define SPECS_FILENAME "specs.txt"

typedef struct {
    const char *name;    /* value used in specs.txt */
    const char *desc;    /* shown in the prompt */
    int hsub, vsub;      /* chroma subsampling divisors (horizontal, vertical) */
    const char *y4m8;    /* y4m C tag at 8 bits */
    const char *y4m10;   /* y4m C tag at 10 bits */
} sampling_info_t;

/* Planar Y, U, V -- the plane order y4m always uses. */
static const sampling_info_t SAMPLINGS[] = {
    {"420", "4:2:0 - chroma half width, half height", 2, 2, "420jpeg", "420p10"},
    {"422", "4:2:2 - chroma half width, full height", 2, 1, "422",     "422p10"},
    {"444", "4:4:4 - chroma full width, full height", 1, 1, "444",     "444p10"},
};
#define N_SAMPLINGS (sizeof(SAMPLINGS) / sizeof(SAMPLINGS[0]))

/* Bits per sample. 8 is one byte per sample; 10 is a 16-bit little-endian
 * word per sample, as in y4m's p10 colorspaces (yuv420p10le). */
static const int BITDEPTHS[] = {8};
#define N_BITDEPTHS (sizeof(BITDEPTHS) / sizeof(BITDEPTHS[0]))

/* Chroma siting (4:2:0 only): where the chroma samples sit relative to the luma.
 * y4m carries it in the C tag, at 8 bits only; the labels are the y4m names. */
enum { CS_JPEG = 0, CS_MPEG2 = 1, CS_PALDV = 2 };
static const struct { const char *name; const char *y4m; const char *desc; const char *menu_note; } SITINGS[] = {
    {"jpeg",  "420jpeg",  "centered: JPEG, MPEG-1, most web video", NULL},
    {"mpeg2", "420mpeg2", "left-aligned: MPEG-2",                   "(most common for DVD sources)"},
    {"paldv", "420paldv", "PAL DV",                                 NULL},
};
#define N_SITINGS (sizeof(SITINGS) / sizeof(SITINGS[0]))

/* Pixel (sample) aspect ratio presets, as y4m's A tag writes them. The NTSC and
 * PAL values are the ITU-R BT.601 ones for 704-wide active pictures; 720-wide
 * coding uses 8:9, 32:27, 16:15 and 64:45, which can be typed as any N:M. */
static const struct { long n, d; const char *desc; } ASPECT_PRESETS[] = {
    { 1,  1, "square pixels: HD, web video, anything already resampled"},
    {10, 11, "NTSC 4:3, 720x480 or 704x480"},
    {40, 33, "NTSC 16:9, anamorphic widescreen"},
    {12, 11, "PAL 4:3, 720x576 or 704x576"},
    {16, 11, "PAL 16:9, anamorphic widescreen"},
};
#define N_ASPECT_PRESETS (sizeof(ASPECT_PRESETS) / sizeof(ASPECT_PRESETS[0]))

typedef struct {
    int width, height;
    int sampling;            /* index into SAMPLINGS */
    long fps_num, fps_den;   /* frame rate as a reduced fraction */
    int bitdepth;            /* one of BITDEPTHS */
    long asp_num, asp_den;   /* pixel aspect ratio as a reduced fraction */
    int chromasiting;        /* CS_JPEG, CS_MPEG2 or CS_PALDV (meaningful for sampling 420 only) */
    int have_chromasiting;   /* specs.txt has a chromasiting line (or the stream named one) */
    int have_aspect;         /* specs.txt already had an aspect line */
    int quality;             /* JPEG quality 1..100 */
    int have_quality;        /* specs.txt has a quality line */
    int range_full;          /* 0 = limited (16-235), 1 = full (0-255) */
    int have_range;          /* specs.txt has a range line */
    uint64_t frame_bytes;    /* bytes in one raw frame at these specs */
} specs_t;

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

/* Lets an answer be typed either as the bare value ("640x480") or as the
 * whole example line ("resolution=640x480"). */
static char *strip_key_prefix(char *s, const char *key) {
    size_t kl = strlen(key);
    if (strncasecmp(s, key, kl) == 0) {
        char *p = s + kl;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '=') {
            p++;
            while (isspace((unsigned char)*p)) p++;
            return p;
        }
    }
    return s;
}

/* "640x480" (spaces around the x are tolerated). Returns 0 on success. */
static int parse_resolution(const char *s, int *w, int *h) {
    char *end;
    long a = strtol(s, &end, 10);
    if (end == s) return -1;
    while (isspace((unsigned char)*end)) end++;
    if (*end != 'x' && *end != 'X') return -1;
    const char *p = end + 1;
    long b = strtol(p, &end, 10);
    if (end == p) return -1;
    while (isspace((unsigned char)*end)) end++;
    if (*end != 0) return -1;
    if (a < 1 || a > 65535 || b < 1 || b > 65535) return -1;
    *w = (int)a;
    *h = (int)b;
    return 0;
}

/* Returns an index into SAMPLINGS, or -1. Exactly "420", "422" or "444". */
static int parse_sampling(const char *s) {
    for (size_t i = 0; i < N_SAMPLINGS; i++) {
        if (!strcmp(s, SAMPLINGS[i].name)) return (int)i;
    }
    return -1;
}

static long gcd_l(long a, long b) {
    while (b) { long t = a % b; a = b; b = t; }
    return a;
}

/* Fraction form only, in y4m's notation "24000:1001" (a "/" is accepted
 * too: "24000/1001"). Decimals ("29.97") and bare integers ("24") are
 * rejected on purpose -- an exact N:D is unambiguous. Stored reduced
 * (48000:2002 -> 24000:1001). Returns 0 on success. */
static int parse_fps(const char *s, long *num, long *den) {
    char *end;
    long n = strtol(s, &end, 10);
    if (end == s) return -1;
    while (isspace((unsigned char)*end)) end++;
    if (*end != ':' && *end != '/') return -1;
    const char *p = end + 1;
    long d = strtol(p, &end, 10);
    if (end == p) return -1;
    while (isspace((unsigned char)*end)) end++;
    if (*end != 0) return -1;
    if (n < 1 || d < 1) return -1;
    if ((double)n / (double)d > 1000.0) return -1;
    long g = gcd_l(n, d);
    *num = n / g;
    *den = d / g;
    return 0;
}

/* 8 or 10. Returns 0 on success. */
/* JPEG quality, a whole number 1..100. 0 = ok, -1 = not valid. */
static int parse_quality(const char *s, int *out) {
    char *e;
    long v = strtol(s, &e, 10);
    if (e == s || *e != 0 || v < 1 || v > 100) return -1;
    *out = (int)v;
    return 0;
}

/* range: limited (16-235 luma, the usual for video) or full (0-255). 0 = ok. */
static int parse_range(const char *s, int *full) {
    if (!strcasecmp(s, "limited") || !strcasecmp(s, "tv") || !strcasecmp(s, "broadcast") || !strcasecmp(s, "mpeg")) { *full = 0; return 0; }
    if (!strcasecmp(s, "full") || !strcasecmp(s, "pc") || !strcasecmp(s, "jpeg")) { *full = 1; return 0; }
    return -1;
}

static int parse_bitdepth(const char *s, int *out) {
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s) return -1;
    while (isspace((unsigned char)*end)) end++;
    if (*end != 0) return -1;
    for (size_t i = 0; i < N_BITDEPTHS; i++) {
        if (v == BITDEPTHS[i]) { *out = (int)v; return 0; }
    }
    return -1;
}

/* Pixel aspect ratio as a fraction N:M (or N/M), reduced: "10:11", "1:1".
 * Returns 0 on success. */
static int parse_aspect(const char *s, long *num, long *den) {
    char *end;
    long n = strtol(s, &end, 10);
    if (end == s) return -1;
    while (isspace((unsigned char)*end)) end++;
    if (*end != ':' && *end != '/') return -1;
    const char *p = end + 1;
    long d = strtol(p, &end, 10);
    if (end == p) return -1;
    while (isspace((unsigned char)*end)) end++;
    if (*end != 0) return -1;
    if (n < 1 || d < 1 || n > 100000 || d > 100000) return -1;
    long g = gcd_l(n, d);
    *num = n / g;
    *den = d / g;
    return 0;
}

/* The old fieldorder line of a specs.txt. Returns 0 for progressive (accepted, and ignored), 1 for an interlaced
 * value (refused: progressive footage only), -1 for anything else. */
static int parse_fieldorder(const char *s) {
    char t[32];
    size_t l = 0;
    while (isspace((unsigned char)*s)) s++;
    for (; *s && !isspace((unsigned char)*s) && l + 1 < sizeof(t); s++) t[l++] = (char)tolower((unsigned char)*s);
    t[l] = 0;
    while (isspace((unsigned char)*s)) s++;
    if (*s) return -1;
    if (!strcmp(t, "progressive") || !strcmp(t, "prog") || !strcmp(t, "p")) return 0;
    if (!strcmp(t, "tff") || !strcmp(t, "top") || !strcmp(t, "t") || !strcmp(t, "topfirst") ||
        !strcmp(t, "bff") || !strcmp(t, "bottom") || !strcmp(t, "b") || !strcmp(t, "bottomfirst")) return 1;
    return -1;
}

/* jpeg / mpeg2 / paldv (also center, mpeg1, left, topleft). Returns the CS_ value, or -1. */
static int parse_siting(const char *s) {
    char t[32];
    size_t l = 0;
    while (isspace((unsigned char)*s)) s++;
    for (; *s && !isspace((unsigned char)*s) && l + 1 < sizeof(t); s++) t[l++] = (char)tolower((unsigned char)*s);
    t[l] = 0;
    while (isspace((unsigned char)*s)) s++;
    if (*s) return -1;
    if (!strcmp(t, "jpeg") || !strcmp(t, "center") || !strcmp(t, "centre") || !strcmp(t, "mpeg1")) return CS_JPEG;
    if (!strcmp(t, "mpeg2") || !strcmp(t, "left")) return CS_MPEG2;
    if (!strcmp(t, "paldv") || !strcmp(t, "topleft")) return CS_PALDV;
    return -1;
}

static int is_420(const specs_t *sp) {
    return SAMPLINGS[sp->sampling].hsub == 2 && SAMPLINGS[sp->sampling].vsub == 2;
}

/* The y4m C tag (without the C) for these specs. 8-bit 4:2:0 carries the chroma siting
 * in it; 10-bit has no siting tag, so the siting is kept in specs.txt only. */
static const char *y4m_ctag(const specs_t *sp) {
    const sampling_info_t *si = &SAMPLINGS[sp->sampling];
    if (sp->bitdepth > 8) return si->y4m10;
    return is_420(sp) ? SITINGS[sp->chromasiting].y4m : si->y4m8;
}

/* The preset's description for a ratio, or NULL if it is not one of them. */
static const char *aspect_preset_desc(long n, long d) {
    for (size_t i = 0; i < N_ASPECT_PRESETS; i++)
        if (ASPECT_PRESETS[i].n == n && ASPECT_PRESETS[i].d == d) return ASPECT_PRESETS[i].desc;
    return NULL;
}

static uint64_t specs_frame_bytes(const specs_t *sp) {
    const sampling_info_t *si = &SAMPLINGS[sp->sampling];
    uint64_t y = (uint64_t)sp->width * (uint64_t)sp->height;
    uint64_t cw = ((uint64_t)sp->width + (uint64_t)si->hsub - 1) / (uint64_t)si->hsub;
    uint64_t ch = ((uint64_t)sp->height + (uint64_t)si->vsub - 1) / (uint64_t)si->vsub;
    return (y + 2 * cw * ch) * (sp->bitdepth > 8 ? 2u : 1u);
}

/* Returns 0 = loaded, 1 = file does not exist, -1 = unreadable/invalid
 * (reason in err). An existing but invalid file is never overwritten. */
static int specs_load(const char *path, specs_t *sp, char *err, size_t errlen) {
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT) return 1;
        snprintf(err, errlen, "cannot open: %s", strerror(errno));
        return -1;
    }
    int have_res = 0, have_smp = 0, have_fps = 0, have_bd = 0;
    sp->have_aspect = 0;
    sp->quality = 90;                       /* what a specs.txt without a quality line means */
    sp->have_quality = 0;
    sp->range_full = 0;
    sp->have_range = 0;
    sp->asp_num = sp->asp_den = 1;          /* what a specs.txt without these lines has always meant */
    sp->chromasiting = CS_JPEG;             /* what has always been served: C420jpeg */
    sp->have_chromasiting = 0;
    char line[256];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *t = trim(line);
        if (*t == 0 || *t == '#') continue;
        char *eq = strchr(t, '=');
        if (!eq) {
            snprintf(err, errlen, "line %d: expected key=value", lineno);
            fclose(f);
            return -1;
        }
        *eq = 0;
        char *key = trim(t);
        char *val = trim(eq + 1);
        for (char *c = key; *c; c++) *c = (char)tolower((unsigned char)*c);

        if (!strcmp(key, "resolution")) {
            if (have_res) { snprintf(err, errlen, "line %d: duplicate resolution", lineno); fclose(f); return -1; }
            if (parse_resolution(val, &sp->width, &sp->height) != 0) {
                snprintf(err, errlen, "line %d: bad resolution '%s' (expected WxH, e.g. resolution=640x480)", lineno, val);
                fclose(f);
                return -1;
            }
            have_res = 1;
        } else if (!strcmp(key, "sampling")) {
            if (have_smp) { snprintf(err, errlen, "line %d: duplicate sampling", lineno); fclose(f); return -1; }
            sp->sampling = parse_sampling(val);
            if (sp->sampling < 0) {
                snprintf(err, errlen, "line %d: unknown sampling '%s' (valid: 420 422 444)", lineno, val);
                fclose(f);
                return -1;
            }
            have_smp = 1;
        } else if (!strcmp(key, "framerate")) {
            if (have_fps) { snprintf(err, errlen, "line %d: duplicate framerate", lineno); fclose(f); return -1; }
            if (parse_fps(val, &sp->fps_num, &sp->fps_den) != 0) {
                snprintf(err, errlen, "line %d: bad framerate '%s' (must be a fraction N:D, e.g. framerate=24000:1001)", lineno, val);
                fclose(f);
                return -1;
            }
            have_fps = 1;
        } else if (!strcmp(key, "bitdepth")) {
            if (have_bd) { snprintf(err, errlen, "line %d: duplicate bitdepth", lineno); fclose(f); return -1; }
            if (parse_bitdepth(val, &sp->bitdepth) != 0) {
                snprintf(err, errlen, "line %d: bad bitdepth '%s' (valid: 8; JPEG stores 8 bits per sample)", lineno, val);
                fclose(f);
                return -1;
            }
            have_bd = 1;
        } else if (!strcmp(key, "aspect")) {
            if (sp->have_aspect) { snprintf(err, errlen, "line %d: duplicate aspect", lineno); fclose(f); return -1; }
            if (parse_aspect(val, &sp->asp_num, &sp->asp_den) != 0) {
                snprintf(err, errlen, "line %d: bad aspect '%s' (pixel aspect ratio as N:M, e.g. aspect=1:1, 10:11, 40:33, 12:11, 16:11)", lineno, val);
                fclose(f);
                return -1;
            }
            sp->have_aspect = 1;
        } else if (!strcmp(key, "fieldorder") || !strcmp(key, "field_order") ||
                   !strcmp(key, "interlace") || !strcmp(key, "interlacing")) {
            /* the synonyms are accepted so that a plausible spelling is never silently ignored */
            int fo = parse_fieldorder(val);
            if (fo < 0) {
                snprintf(err, errlen, "line %d: bad fieldorder '%s' (only progressive is stored, and the line is not needed)", lineno, val);
                fclose(f);
                return -1;
            }
            if (fo == 1) {
                snprintf(err, errlen, "line %d: fieldorder=%s -- mjpegstore stores progressive footage only; deinterlace the "
                                      "footage first (to 60p, say) and feed progressive frames", lineno, val);
                fclose(f);
                return -1;
            }
        } else if (!strcmp(key, "chromasiting") || !strcmp(key, "chroma_siting") ||
                   !strcmp(key, "chromalocation") || !strcmp(key, "chroma_location")) {
            if (sp->have_chromasiting) { snprintf(err, errlen, "line %d: duplicate chromasiting", lineno); fclose(f); return -1; }
            int cs = parse_siting(val);
            if (cs < 0) {
                snprintf(err, errlen, "line %d: bad chromasiting '%s' (valid: jpeg, mpeg2, paldv)", lineno, val);
                fclose(f);
                return -1;
            }
            sp->chromasiting = cs;
            sp->have_chromasiting = 1;
        } else if (!strcmp(key, "quality")) {
            if (sp->have_quality) { snprintf(err, errlen, "line %d: duplicate quality", lineno); fclose(f); return -1; }
            if (parse_quality(val, &sp->quality) != 0) {
                snprintf(err, errlen, "line %d: bad quality '%s' (a whole number from 1 to 100, e.g. quality=90)", lineno, val);
                fclose(f);
                return -1;
            }
            sp->have_quality = 1;
        } else if (!strcmp(key, "range")) {
            if (sp->have_range) { snprintf(err, errlen, "line %d: duplicate range", lineno); fclose(f); return -1; }
            if (parse_range(val, &sp->range_full) != 0) {
                snprintf(err, errlen, "line %d: bad range '%s' (valid: limited, full)", lineno, val);
                fclose(f);
                return -1;
            }
            sp->have_range = 1;
        } else {
            fprintf(stderr, "mjpegstore: %s: line %d: ignoring unknown key '%s'\n", path, lineno, key);
        }
    }
    fclose(f);
    if (!have_res || !have_smp || !have_fps || !have_bd) {
        snprintf(err, errlen, "missing %s",
                 !have_res ? "resolution" : !have_smp ? "sampling" :
                 !have_fps ? "framerate" : "bitdepth");
        return -1;
    }
    if (sp->have_chromasiting && !is_420(sp)) {
        snprintf(err, errlen, "chromasiting applies only to sampling=420 (this specs.txt says %s): remove the line",
                 SAMPLINGS[sp->sampling].name);
        return -1;
    }
    sp->frame_bytes = specs_frame_bytes(sp);
    return 0;
}

/* Prints "  <key>> ", reads one line, and returns the value (trimmed, with an
 * optional leading "key=" removed), or NULL on EOF. */
static char *prompt_value(const char *key, char *buf, size_t n) {
    fprintf(stderr, "  %s> ", key);
    fflush(stderr);
    if (!fgets(buf, (int)n, stdin)) return NULL;
    return strip_key_prefix(trim(buf), key);
}

/* A boxed block of text on stderr, for what must not be missed. */
static void warn_box(const char *const *lines, size_t n) {
    const int w = 72;
    fputs("\n  ", stderr);
    for (int i = 0; i < w + 6; i++) fputc('#', stderr);
    fputc('\n', stderr);
    for (size_t i = 0; i < n; i++) fprintf(stderr, "  ## %-*s ##\n", w, lines[i]);
    fputs("  ", stderr);
    for (int i = 0; i < w + 6; i++) fputc('#', stderr);
    fputc('\n', stderr);
}

/* Ask on the terminal until each answer parses; every question shows an
 * example line and the y4m header token it produces. Returns 0, or -1 if
 * input ends (EOF) before all six values are in. Prompts go to stderr so
 * they stay visible when stdout is redirected or piped. */
static int specs_prompt(specs_t *sp) {
    char buf[128];
    char *v;

    fprintf(stderr, "\n  Resolution: width x height in pixels (y4m W and H).\n"
                    "    example line:  resolution=640x480       -> W640 H480\n");
    for (;;) {
        if (!(v = prompt_value("resolution", buf, sizeof(buf)))) return -1;
        if (parse_resolution(v, &sp->width, &sp->height) == 0) break;
        fprintf(stderr, "    not valid -- expected e.g. resolution=640x480 (each side 1..65535)\n");
    }

    fprintf(stderr, "\n  Sampling: chroma subsampling (y4m C tag). Valid values:\n");
    for (size_t i = 0; i < N_SAMPLINGS; i++) {
        fprintf(stderr, "    %-4s %-40s -> C%s\n", SAMPLINGS[i].name, SAMPLINGS[i].desc, SAMPLINGS[i].y4m8);
    }
    fprintf(stderr, "    frame files must hold planes in y4m order: Y, then U, then V.\n"
                    "    example line:  sampling=420\n");
    for (;;) {
        if (!(v = prompt_value("sampling", buf, sizeof(buf)))) return -1;
        sp->sampling = parse_sampling(v);
        if (sp->sampling >= 0) break;
        fprintf(stderr, "    not valid -- pick one of 420, 422, 444, e.g. sampling=420\n");
    }

    fprintf(stderr, "\n  Frame rate: a fraction N:D, frames per second (y4m F).\n"
                    "    example line:  framerate=24000:1001     -> F24000:1001\n"
                    "    also valid:    24:1   25:1   30000:1001   60:1   60000:1001\n");
    for (;;) {
        if (!(v = prompt_value("framerate", buf, sizeof(buf)))) return -1;
        if (parse_fps(v, &sp->fps_num, &sp->fps_den) == 0) break;
        fprintf(stderr, "    not valid -- must be a fraction N:D, e.g. framerate=24000:1001 "
                        "(write 24 as 24:1)\n");
    }

    fprintf(stderr, "\n  Bit depth: JPEG stores 8 bits per sample, so 8 is the only value.\n"
                    "    example line:  bitdepth=8\n");
    for (;;) {
        if (!(v = prompt_value("bitdepth", buf, sizeof(buf)))) return -1;
        if (parse_bitdepth(v, &sp->bitdepth) == 0) break;
        fprintf(stderr, "    not valid -- the only value is 8, e.g. bitdepth=8\n");
    }

    sp->chromasiting = CS_JPEG;
    sp->have_chromasiting = 0;
    if (is_420(sp)) {
        fprintf(stderr, "\n  Chroma siting (4:2:0 only): where the chroma samples sit relative to the luma (y4m C tag).\n"
                        "  Valid values:\n");
        for (size_t i = 0; i < N_SITINGS; i++)
            fprintf(stderr, "    %-6s %-40s -> C%-9s %s\n", SITINGS[i].name, SITINGS[i].desc, SITINGS[i].y4m,
                    SITINGS[i].menu_note ? SITINGS[i].menu_note : "");
        fprintf(stderr, "    Press Enter for jpeg.\n"
                        "    example line:  chromasiting=mpeg2\n");
        for (;;) {
            if (!(v = prompt_value("chromasiting", buf, sizeof(buf)))) return -1;
            if (!*v) { sp->chromasiting = CS_JPEG; break; }
            int cs = parse_siting(v);
            if (cs >= 0) { sp->chromasiting = cs; break; }
            fprintf(stderr, "    not valid -- pick jpeg, mpeg2 or paldv, e.g. chromasiting=mpeg2\n");
        }
        sp->have_chromasiting = 1;
    }

    fprintf(stderr, "\n  Pixel aspect ratio: the shape of one pixel, N:M (y4m A tag). Presets:\n");
    for (size_t i = 0; i < N_ASPECT_PRESETS; i++) {
        char r[16];
        snprintf(r, sizeof(r), "%ld:%ld", ASPECT_PRESETS[i].n, ASPECT_PRESETS[i].d);
        fprintf(stderr, "    %-6s %-58s -> A%s\n", r, ASPECT_PRESETS[i].desc, r);
    }
    fprintf(stderr, "    any other N:M is accepted too. Press Enter for 1:1.\n"
                    "    example line:  aspect=10:11\n");
    for (;;) {
        if (!(v = prompt_value("aspect", buf, sizeof(buf)))) return -1;
        if (!*v) { sp->asp_num = sp->asp_den = 1; break; }
        if (parse_aspect(v, &sp->asp_num, &sp->asp_den) == 0) break;
        fprintf(stderr, "    not valid -- expected a ratio N:M, e.g. aspect=1:1 or aspect=40:33\n");
    }

    fprintf(stderr, "\n  JPEG quality: 1..100, higher is finer and bigger. 90 is the default; 100 is the\n"
                    "    finest a baseline JPEG can be (still lossy). Every frame in a set uses the same value, so\n"
                    "    that the same picture area gives the same bytes. Press Enter for 90.\n"
                    "    example line:  quality=90\n");
    for (;;) {
        if (!(v = prompt_value("quality", buf, sizeof(buf)))) return -1;
        if (!*v) { sp->quality = 90; break; }
        if (parse_quality(v, &sp->quality) == 0) break;
        fprintf(stderr, "    not valid -- a whole number from 1 to 100, e.g. quality=90\n");
    }

    fprintf(stderr, "\n  Range: how the stored Y/U/V values are to be read. Valid values:\n"
                    "    limited  16-235 luma, 16-240 chroma: normal video (DVD, broadcast, most web video)\n"
                    "    full     0-255: only if the source really is full range\n"
                    "    Press Enter for limited.\n"
                    "    example line:  range=limited\n");
    for (;;) {
        if (!(v = prompt_value("range", buf, sizeof(buf)))) return -1;
        if (!*v) { sp->range_full = 0; break; }
        if (parse_range(v, &sp->range_full) == 0) break;
        fprintf(stderr, "    not valid -- limited or full, e.g. range=limited\n");
    }
    sp->have_quality = 1;
    sp->have_range = 1;

    sp->have_aspect = 1;
    sp->frame_bytes = specs_frame_bytes(sp);
    return 0;
}

/* Exclusive create ("wx") so an existing file is never clobbered. */
static int specs_write(const char *path, const specs_t *sp) {
    FILE *f = fopen(path, "wx");
    if (!f) return -1;
    char siting[240] = "";
    if (is_420(sp) && sp->have_chromasiting)
        snprintf(siting, sizeof(siting), "# chromasiting (4:2:0 only): jpeg, mpeg2 or paldv.\n"
                                         "chromasiting=%s\n", SITINGS[sp->chromasiting].name);
    fprintf(f, "resolution=%dx%d\nsampling=%s\nframerate=%ld:%ld\nbitdepth=%d\n%s"
               "# aspect: pixel aspect ratio N:M. 1:1 square; 10:11 and 40:33 NTSC 4:3 and 16:9; 12:11 and 16:11 PAL 4:3 and 16:9\n"
               "aspect=%ld:%ld\n"
               "# quality: JPEG quality 1..100 for frames ingested into this set. Keep it the same for every clip: the same picture area then gives the same bytes.\n"
               "quality=%d\n"
               "# range: limited (16-235, normal video) or full (0-255): how the stored values are to be read.\n"
               "range=%s\n",
            sp->width, sp->height, SAMPLINGS[sp->sampling].name,
            sp->fps_num, sp->fps_den, sp->bitdepth, siting,
            sp->asp_num, sp->asp_den,
            sp->quality, sp->range_full ? "full" : "limited");
    if (fclose(f) != 0) return -1;
    return 0;
}

/* Appends a line such as "aspect=1:1" to an existing specs.txt (adding a
 * newline first if the file doesn't end with one). Returns 0 on success. */
static int specs_append_line(const char *path, const char *line) {
    FILE *f = fopen(path, "a+");
    if (!f) return -1;
    int need_nl = 0;
    if (fseek(f, -1, SEEK_END) == 0) need_nl = (fgetc(f) != '\n');
    fseek(f, 0, SEEK_END);   /* required between a read and a write */
    if (fprintf(f, "%s%s\n", need_nl ? "\n" : "", line) < 0) { fclose(f); return -1; }
    return fclose(f) == 0 ? 0 : -1;
}

static int specs_add_aspect(const char *path) { return specs_append_line(path, "aspect=1:1"); }

static void specs_print(const specs_t *sp, const char *path) {
    char cs[64] = "";
    if (is_420(sp))
        snprintf(cs, sizeof(cs), ", chroma siting %s", sp->have_chromasiting ? SITINGS[sp->chromasiting].name : "not set (jpeg assumed)");
    fprintf(stderr, "mjpegstore: specs (%s): %dx%d %s %d-bit%s, %ld:%ld fps, pixel aspect %ld:%ld, JPEG quality %d, %s range\n",
            path, sp->width, sp->height, SAMPLINGS[sp->sampling].name, sp->bitdepth, cs,
            sp->fps_num, sp->fps_den, sp->asp_num, sp->asp_den,
            sp->quality, sp->range_full ? "full" : "limited");
}

/* Find <dir>/specs.txt; if absent, ask the user and write it. Returns 0
 * with sp filled in, or -1 (message already printed). */
static int specs_for_dir(const char *dir, specs_t *sp, int quiet) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dir, SPECS_FILENAME);
    char err[256];
    int rc = specs_load(path, sp, err, sizeof(err));
    if (rc < 0) {
        fprintf(stderr, "mjpegstore: %s: %s\n", path, err);
        return -1;
    }
    if (rc == 0) {
        if (!sp->have_aspect) {
            /* Shown even with -q: this edits the user's file. */
            if (specs_add_aspect(path) == 0) {
                fprintf(stderr, "mjpegstore: added aspect=1:1 to %s (square pixels, which a specs.txt without an aspect line has always meant)\n", path);
            } else {
                fprintf(stderr, "mjpegstore: warning: could not add aspect=1:1 to %s: %s (using 1:1 for this run)\n",
                        path, strerror(errno));
            }
        }
        if (!quiet) specs_print(sp, path);
        return 0;
    }

    if (!isatty(STDIN_FILENO)) {
        fprintf(stderr,
            "mjpegstore: %s not found, and stdin is not a terminal so it cannot be asked for.\n"
            "         Create it with these lines (aspect=1:1 is assumed if omitted):\n"
            "           resolution=640x480\n"
            "           sampling=420\n"
            "           framerate=24000:1001\n"
            "           bitdepth=8              (JPEG stores 8 bits per sample)\n"
            "           chromasiting=jpeg       (4:2:0 only: jpeg, mpeg2 or paldv)\n"
            "           aspect=1:1              (or 10:11 40:33 12:11 16:11, or any N:M)\n"
            "           quality=90              (JPEG quality 1..100; 90 is assumed if omitted)\n"
            "           range=limited           (or full; limited is assumed if omitted)\n", path);
        return -1;
    }
    fprintf(stderr, "mjpegstore: %s not found -- describe the raw YUV in this set\n"
                    "         (applies to every clip subdirectory). Type either the value\n"
                    "         or the whole example line.\n", path);
    if (specs_prompt(sp) != 0) {
        fprintf(stderr, "\nmjpegstore: input ended before the specs were complete; nothing written\n");
        return -1;
    }
    if (specs_write(path, sp) != 0) {
        fprintf(stderr, "mjpegstore: warning: could not write %s: %s (using these values for this run only)\n",
                path, strerror(errno));
    } else if (!quiet) {
        fprintf(stderr, "mjpegstore: wrote %s\n", path);
    }
    if (!quiet) specs_print(sp, path);
    return 0;
}

/* Counts the regular files sitting directly in dir, other than specs.txt
 * and dotfiles (symlinks followed). A directory with any of these holds
 * frames, so it is a CLIP directory; a set directory holds only
 * specs.txt and clip subdirectories. */
/* Pictures are stored in bucket folders inside a clip: b00000, b00001, ... of 5000 frames each, so that
 * no folder gets huge. */
#define BUCKET_FRAMES 5000
static int is_bucket_name(const char *name) {
    if (name[0] != 'b' || !name[1]) return 0;
    for (const char *p = name + 1; *p; p++) if (*p < '0' || *p > '9') return 0;
    return 1;
}

static size_t count_stray_top_level_files(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    size_t n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (!strcmp(ent->d_name, SPECS_FILENAME)) continue;
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (stat(full, &st) == 0 && (S_ISREG(st.st_mode) || (S_ISDIR(st.st_mode) && is_bucket_name(ent->d_name)))) n++;
    }
    closedir(d);
    return n;
}

/* ---------------------------------------------------------------- */
/* Path collection                                                  */
/* ---------------------------------------------------------------- */

typedef struct {
    char **paths;
    size_t count;
    size_t cap;
} pathlist_t;

static void pathlist_init(pathlist_t *pl) {
    pl->paths = NULL;
    pl->count = 0;
    pl->cap = 0;
}

static void pathlist_push(pathlist_t *pl, const char *p) {
    if (pl->count == pl->cap) {
        pl->cap = pl->cap ? pl->cap * 2 : 1024;
        pl->paths = realloc(pl->paths, pl->cap * sizeof(char *));
    }
    pl->paths[pl->count++] = strdup(p);
}

static void scan_dir(const char *dir, pathlist_t *pl, int quiet, int top) {
    DIR *d = opendir(dir);
    if (!d) {
        if (!quiet) fprintf(stderr, "mjpegstore: warning: cannot open %s: %s\n",
                             dir, strerror(errno));
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (lstat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            scan_dir(full, pl, quiet, 0);
        } else if (S_ISREG(st.st_mode)) {
            /* specs.txt describes the set; it is not a frame. */
            if (top && !strcmp(ent->d_name, SPECS_FILENAME)) continue;
            pathlist_push(pl, full);
        }
    }
    closedir(d);
}

static void read_stdin_list(pathlist_t *pl) {
    char line[PATH_MAX];
    while (fgets(line, sizeof(line), stdin)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (l) pathlist_push(pl, line);
    }
}

/* ---------------------------------------------------------------- */
/* Serve mode (-s): stream a clip directory as yuv4mpeg (y4m)        */
/*                                                                   */
/* The named directory holds the raw frame files (planar Y,U,V at    */
/* the specs' resolution/sampling/bitdepth). specs.txt is read from   */
/* the directory's PARENT (the set directory). Each file becomes one  */
/* or more y4m frames, in natural filename order: header, then        */
/* "FRAME\n" + raw bytes per frame. Raw frames are already in y4m     */
/* plane order, so the bytes are copied through untouched.            */
/* ---------------------------------------------------------------- */

typedef struct {
    char *name;
    uint64_t size;
} frame_file_t;

/* Filename order where digit runs compare as numbers: f2 < f10. */
static int natural_cmp(const char *a, const char *b) {
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            const char *ra = a, *rb = b;              /* whole digit runs */
            while (isdigit((unsigned char)*ra)) ra++;
            while (isdigit((unsigned char)*rb)) rb++;
            const char *za = a, *zb = b;              /* skip leading zeros */
            while (za < ra - 1 && *za == '0') za++;
            while (zb < rb - 1 && *zb == '0') zb++;
            size_t la = (size_t)(ra - za), lb = (size_t)(rb - zb);
            if (la != lb) return la < lb ? -1 : 1;
            int c = memcmp(za, zb, la);
            if (c) return c < 0 ? -1 : 1;
            a = ra;
            b = rb;
        } else {
            if (*a != *b) return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
            a++;
            b++;
        }
    }
    if (*a) return 1;
    if (*b) return -1;
    return 0;
}

/* Textual parent of a directory path ("set/clip" -> "set", "clip" -> ".",
 * "." -> "./.."). */
static void parent_of(const char *dir, char *out, size_t n) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/') tmp[--len] = 0;
    char *slash = strrchr(tmp, '/');
    const char *base = slash ? slash + 1 : tmp;
    if (!strcmp(base, ".") || !strcmp(base, "..")) {
        snprintf(out, n, "%s/..", tmp);
    } else if (!slash) {
        snprintf(out, n, ".");
    } else if (slash == tmp) {
        snprintf(out, n, "/");
    } else {
        *slash = 0;
        snprintf(out, n, "%s", tmp);
    }
}

static int path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* If path doesn't exist, returns 1 (nothing to confirm) with no output.
 * If it exists and stdin is a terminal, asks the user and returns 1 for
 * yes, 0 (with a message) for no. If it exists and stdin is NOT a
 * terminal, refuses outright (with a message) rather than guessing or
 * hanging on a read that will never come -- same policy as the specs.txt
 * prompt elsewhere in this file. Never appends: the caller always opens
 * the file fresh ("w") after this returns 1, exactly once, and (for -d,
 * which can be written to across several plane sorts in one run) keeps
 * that single handle open rather than reopening it. */
static int confirm_overwrite(const char *path, int quiet) {
    if (!path_exists(path)) return 1;
    if (!isatty(STDIN_FILENO)) {
        fprintf(stderr, "mjpegstore: %s already exists, and stdin is not a terminal so it cannot be asked "
                        "whether to overwrite it. Remove it, choose another path, or run interactively.\n", path);
        return 0;
    }
    fprintf(stderr, "mjpegstore: %s already exists. Overwrite it? [y/N] ", path);
    fflush(stderr);
    char line[16];
    if (!fgets(line, sizeof(line), stdin) || (line[0] != 'y' && line[0] != 'Y')) {
        if (!quiet) fprintf(stderr, "mjpegstore: not overwriting %s; nothing written.\n", path);
        return 0;
    }
    return 1;
}




/* True if dir's immediate contents are ONLY subdirectories (plus,
 * optionally, specs.txt) -- no loose regular files directly inside it.
 * That shape means dir is a set directory holding more than one clip
 * (or exactly one, nested), not a clip itself: pointing a sort at it
 * would otherwise recurse blindly through every clip's files as one
 * undifferentiated pile, mixing separate clips (and, for component
 * clips, mixing y/u/v planes together) with no regard for the
 * structure. Callers should refuse rather than silently flattening it. */
static int dir_holds_only_subdirs(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int any_subdir = 0, any_loose_file = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (lstat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (is_bucket_name(ent->d_name)) any_loose_file = 1;       /* a clip's bucket folder, not a clip */
            else any_subdir = 1;
        } else if (S_ISREG(st.st_mode) && strcmp(ent->d_name, SPECS_FILENAME) != 0) any_loose_file = 1;
    }
    closedir(d);
    return any_subdir && !any_loose_file;
}


static int name_natural_cmp(const void *a, const void *b) {
    const char *x = *(char *const *)a, *y = *(char *const *)b;
    int c = natural_cmp(x, y);
    return c ? c : strcmp(x, y);
}

/* The regular files in dir whose names end in ext (or, with ext == NULL,
 * every regular file except dotfiles and specs.txt: a clip's frame files), as "dir/name" strings in
 * natural filename order (so the run does not depend on directory order).
 * Returns a malloc'd array (or NULL when empty); *count is its length. */
static int list_ext_collect(const char *dir, const char *ext, int top, char ***names_p, size_t *n_p, size_t *cap_p) {
    DIR *d = opendir(dir);
    if (!d) return top ? -1 : 0;
    size_t el = ext ? strlen(ext) : 0, cap = *cap_p, n = *n_p;
    char **names = *names_p;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t l = strlen(ent->d_name);
        if (ent->d_name[0] == '.') continue;
        if (top && is_bucket_name(ent->d_name)) {              /* a bucket folder: its pictures belong to this clip */
            char sub[PATH_MAX + 300];
            snprintf(sub, sizeof(sub), "%.*s/%s", PATH_MAX - 1, dir, ent->d_name);
            struct stat sst;
            if (stat(sub, &sst) == 0 && S_ISDIR(sst.st_mode)) {
                *names_p = names; *n_p = n; *cap_p = cap;
                if (list_ext_collect(sub, ext, 0, names_p, n_p, cap_p) != 0) { closedir(d); return -1; }
                names = *names_p; n = *n_p; cap = *cap_p;
                continue;
            }
        }
        if (ext) {
            if (l <= el || strcasecmp(ent->d_name + l - el, ext) != 0) continue;
        } else if (!strcmp(ent->d_name, SPECS_FILENAME)) {
            continue;
        }
        char full[PATH_MAX + 300];
        snprintf(full, sizeof(full), "%.*s/%s", PATH_MAX - 1, dir, ent->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 1024;
            char **nn = realloc(names, cap * sizeof(*names));
            if (!nn) { closedir(d); *names_p = names; *n_p = n; *cap_p = cap; return -1; }
            names = nn;
        }
        names[n] = strdup(full);
        if (!names[n]) { closedir(d); *names_p = names; *n_p = n; *cap_p = cap; return -1; }
        n++;
    }
    closedir(d);
    *names_p = names; *n_p = n; *cap_p = cap;
    return 0;
}

static char **list_ext_files(const char *dir, const char *ext, size_t *count) {
    char **names = NULL;
    size_t n = 0, cap = 0;
    *count = 0;
    if (list_ext_collect(dir, ext, 1, &names, &n, &cap) != 0) {
        for (size_t i = 0; i < n; i++) free(names[i]);
        free(names);
        return NULL;
    }
    if (n) qsort(names, n, sizeof(*names), name_natural_cmp);
    *count = n;
    return names;
}


/* Which directory's specs.txt applies to the directory the user named?
 *
 *  - A directory holding no frame files is a SET directory: its own
 *    specs.txt applies.
 *  - A directory holding frame files, or y/u/v component planes (made by
 *    -e), is a CLIP-like directory: the specs.txt belongs to its parent
 *    (the set) and ONLY there. This is a rule, not a preference: one
 *    specs.txt per set is easy to find, and a copy in every clip would be a
 *    lot of files to parse. A copy inside a clip is never used -- it is
 *    ignored with a warning, even when the parent has none (then the parent
 *    is where a new one is asked for and created).
 *
 * Decided by what the directory contains, never by whether a specs.txt
 * happens to sit inside it. Writes the directory to use into out. */
static void specs_locate(const char *dir, char *out, size_t n, int quiet) {
    if (count_stray_top_level_files(dir) == 0) {
        snprintf(out, n, "%s", dir);
        return;
    }
    char own[PATH_MAX + 32], parent[PATH_MAX + 8];
    parent_of(dir, parent, sizeof(parent));
    {   /* "." / ".." read badly in messages ("move it up to ."): spell those out */
        size_t pl = strlen(parent);
        if (!strcmp(parent, ".") || !strcmp(parent, "..") ||
            (pl >= 3 && !strcmp(parent + pl - 3, "/.."))) {
            char abs[PATH_MAX];
            if (realpath(parent, abs)) snprintf(parent, sizeof(parent), "%s", abs);
        }
    }
    snprintf(own, sizeof(own), "%s/%s", dir, SPECS_FILENAME);
    if (path_exists(own) && !quiet) {
        fprintf(stderr, "mjpegstore: warning: ignoring %s -- specs.txt belongs in the set directory (%s), "
                        "never inside a clip; this copy can be deleted\n", own, parent);
    }
    snprintf(out, n, "%s", parent);
}

/* The specs.txt that goes with the clip or component directory `dir`, spelled
 * the way the list's other paths are (relative when `dir` is), and only if the
 * file exists. Every list opens with it, so an archive built from the list
 * carries it: a read-only mount of that archive can't have a specs.txt
 * generated, and without one -s doesn't know the frame size.
 * Silent. Returns 1 and fills `out`, or 0. */
static int specs_list_path(const char *dir, char *out, size_t n) {
    char cand[PATH_MAX + 32];
    if (count_stray_top_level_files(dir) == 0) {
        snprintf(cand, sizeof(cand), "%s/%s", dir, SPECS_FILENAME);      /* a set directory itself */
        if (!path_exists(cand)) return 0;
        snprintf(out, n, "%s", cand);
        return 1;
    }
    char parent[PATH_MAX + 8];
    parent_of(dir, parent, sizeof(parent));
    if (!strcmp(parent, ".")) snprintf(cand, sizeof(cand), "%s", SPECS_FILENAME);
    else if (!strcmp(parent, "/")) snprintf(cand, sizeof(cand), "/%s", SPECS_FILENAME);
    else snprintf(cand, sizeof(cand), "%s/%s", parent, SPECS_FILENAME);
    if (!path_exists(cand)) return 0;       /* never a copy inside the clip: parent only */
    snprintf(out, n, "%s", cand);
    return 1;
}

static int same_file(const char *a, const char *b) {
    struct stat sa, sb;
    return stat(a, &sa) == 0 && stat(b, &sb) == 0 && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

static void warn_no_specs(const char *dir) {
    fprintf(stderr, "mjpegstore: warning: no specs.txt found for %s, so the list has none. An archive built from it "
                    "won't carry one, and a read-only mount of that archive can't have one generated.\n", dir);
}

/* The specs.txt lines that open a list: one per distinct set among dirs,
 * in the order the sets first appear. A directory with none is warned about
 * (unless quiet) and contributes nothing. Returns a malloc'd array (never
 * NULL) and its length; release it with free_specs(). */
static char **collect_specs(const char *const *dirs, int ndirs, size_t *count, int quiet) {
    char **v = calloc((size_t)ndirs + 1, sizeof(char *));
    size_t n = 0;
    *count = 0;
    if (!v) return NULL;
    for (int i = 0; i < ndirs; i++) {
        char p[PATH_MAX + 32];
        if (!specs_list_path(dirs[i], p, sizeof(p))) {
            if (!quiet) warn_no_specs(dirs[i]);
            continue;
        }
        int dup = 0;
        for (size_t k = 0; k < n && !dup; k++) dup = same_file(v[k], p);
        if (!dup) {
            v[n] = strdup(p);
            if (!v[n]) break;
            n++;
        }
    }
    *count = n;
    return v;
}

static void free_specs(char **v, size_t n) {
    if (!v) return;
    for (size_t i = 0; i < n; i++) free(v[i]);
    free(v);
}

#define SERVE_BUF (1u << 20)

/* Returns the process exit status. A closed pipe (player quit) is a
 * normal stop, not an error. */
static int serve_write_failed(void) {
    if (errno == EPIPE) return 0;
    fprintf(stderr, "\nmjpegstore: write error: %s\n", strerror(errno));
    return 1;
}

/* Text quoted from an input stream (or a filename) is untrusted: a damaged or
 * hostile source must not be able to put control characters or terminal
 * escape sequences in a message. Copies at most n-1 bytes, showing anything that
 * isn't printable ASCII as '?'. */
static const char *safe_text(const char *in, char *out, size_t n) {
    size_t i = 0;
    for (; in[i] && i + 1 < n; i++) {
        unsigned char c = (unsigned char)in[i];
        out[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
    }
    out[i] = 0;
    return out;
}

/* ---------------------------------------------------------------- */
/* Ingest (-i): a y4m stream becomes a clip directory                */
/*                                                                   */
/*   mjpegstore -i FILE|- <clipdir>                                     */
/*                                                                   */
/* The stream header is turned into specs (resolution, sampling,     */
/* framerate, bitdepth). specs.txt in the clip's parent (the set) is */
/* created from it if absent; if present it must be identical, and   */
/* otherwise nothing at all is written. Each frame then becomes one  */
/* raw file, yuv-000001.yuv onward, in the new clip directory. The   */
/* frame bytes are copied through untouched -- y4m frames already    */
/* are raw planar Y,U,V in exactly the layout the other modes read.  */
/* ---------------------------------------------------------------- */

#define INGEST_LINE_MAX 4096
#define INGEST_BUF (1u << 20)

/* y4m colorspace tags this tool can store, and the specs they map to. */
static const struct {
    const char *tag, *sampling;
    int depth;
    int siting;                /* the chroma siting the tag names, or -1 if it names none */
    const char *note;
} Y4M_COLORSPACES[] = {
    {"420jpeg",  "420",  8, CS_JPEG,  NULL},
    {"420",      "420",  8, CS_JPEG,  NULL},      /* the y4m default: centered */
    {"420mpeg2", "420",  8, CS_MPEG2, NULL},
    {"420paldv", "420",  8, CS_PALDV, NULL},
    {"422",      "422",  8, -1, NULL},
    {"444",      "444",  8, -1, NULL},
    {"420p10",   "420", 10, -1, NULL},              /* y4m has no siting tag at 10 bits */
    {"422p10",   "422", 10, -1, NULL},
    {"444p10",   "444", 10, -1, NULL},
};

static void notes_add(char *notes, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void notes_add(char *notes, size_t n, const char *fmt, ...) {
    size_t l = strlen(notes);
    if (l >= n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(notes + l, n - l, fmt, ap);
    va_end(ap);
}

/* Reads one line (without its newline). Returns 0 = got a line,
 * 1 = clean EOF before any byte, -1 = EOF in the middle of a line,
 * -2 = line longer than the buffer. */
static int ingest_read_line(FILE *in, char *line, size_t n) {
    size_t l = 0;
    int c = getc(in);
    if (c == EOF) return 1;
    for (;;) {
        if (c == '\n') { line[l] = 0; return 0; }
        if (c == EOF) return -1;
        if (l + 1 >= n) return -2;
        line[l++] = (char)c;
        c = getc(in);
    }
}

static int parse_int_strict(const char *v, long lo, long hi, long *out) {
    char *e;
    long x = strtol(v, &e, 10);
    if (e == v || *e != 0 || x < lo || x > hi) return -1;
    *out = x;
    return 0;
}

/* Turn a y4m stream header line into specs. Returns 0, or -1 with the
 * reason in err. Anything the tool can't store faithfully is refused
 * rather than guessed at. Informational remarks go into notes. */
static int y4m_parse_header(const char *line, specs_t *sp, char *err, size_t errlen,
                            char *notes, size_t noteslen) {
    char buf[INGEST_LINE_MAX];
    char shown[80];   /* sanitized copy of a stream token, for messages */
    snprintf(buf, sizeof(buf), "%s", line);
    int have_w = 0, have_h = 0, have_f = 0;
    sp->asp_num = sp->asp_den = 1;
    const char *cs = "420jpeg";   /* the y4m default when there is no C tag */
    char interlace = '?';
    int range_full = 0;
    char *save = NULL;
    char *tok = strtok_r(buf, " ", &save);
    if (!tok || strcmp(tok, "YUV4MPEG2") != 0) {
        snprintf(err, errlen, "not a yuv4mpeg stream (it does not start with YUV4MPEG2)");
        return -1;
    }
    while ((tok = strtok_r(NULL, " ", &save)) != NULL) {
        const char *v = tok + 1;
        long x;
        switch (tok[0]) {
        case 'W':
            if (parse_int_strict(v, 1, 65535, &x) != 0) { snprintf(err, errlen, "bad width tag '%s'", safe_text(tok, shown, sizeof(shown))); return -1; }
            sp->width = (int)x; have_w = 1;
            break;
        case 'H':
            if (parse_int_strict(v, 1, 65535, &x) != 0) { snprintf(err, errlen, "bad height tag '%s'", safe_text(tok, shown, sizeof(shown))); return -1; }
            sp->height = (int)x; have_h = 1;
            break;
        case 'F':
            if (parse_fps(v, &sp->fps_num, &sp->fps_den) != 0) {
                snprintf(err, errlen, "the stream has no usable frame rate ('%s'); specs.txt needs a real one",
                         safe_text(tok, shown, sizeof(shown)));
                return -1;
            }
            have_f = 1;
            break;
        case 'I':
            interlace = v[0] ? v[0] : '?';
            break;
        case 'A': {
            char *e;
            long n = strtol(v, &e, 10);
            long d = 0;
            if (e == v || *e != ':' || (d = strtol(e + 1, &e, 10), *e != 0) || n < 0 || d < 0 || n > 100000 || d > 100000) {
                snprintf(err, errlen, "bad pixel aspect tag '%s'", safe_text(tok, shown, sizeof(shown)));
                return -1;
            }
            if (n == 0 && d == 0) {
                notes_add(notes, noteslen, "mjpegstore: note: the stream's pixel aspect is unknown (A0:0); treated as square (1:1)\n");
            } else if (n == 0 || d == 0) {
                snprintf(err, errlen, "bad pixel aspect tag '%s' (a zero side)", safe_text(tok, shown, sizeof(shown)));
                return -1;
            } else {
                long g = gcd_l(n, d);
                sp->asp_num = n / g;
                sp->asp_den = d / g;
                if (!aspect_preset_desc(sp->asp_num, sp->asp_den))
                    notes_add(notes, noteslen, "mjpegstore: note: pixel aspect %ld:%ld is not one of the presets; it is recorded as given\n",
                              sp->asp_num, sp->asp_den);
            }
            break;
        }
        case 'C':
            cs = v;
            break;
        default:
            if (!strncasecmp(tok, "XCOLORRANGE=", 12)) range_full = !strcasecmp(tok + 12, "FULL");
            break;   /* other X comments and tags this tool doesn't use */
        }
    }
    if (!have_w || !have_h) { snprintf(err, errlen, "the header has no %s", !have_w ? "width (W)" : "height (H)"); return -1; }
    if (!have_f) { snprintf(err, errlen, "the header has no frame rate (F tag); specs.txt needs one"); return -1; }

    int found = -1;
    for (size_t i = 0; i < sizeof(Y4M_COLORSPACES) / sizeof(Y4M_COLORSPACES[0]); i++) {
        if (!strcmp(cs, Y4M_COLORSPACES[i].tag)) { found = (int)i; break; }
    }
    if (found < 0) {
        snprintf(err, errlen, "unsupported colorspace 'C%s'. Supported: 420 (420jpeg/420mpeg2/420paldv), 422, 444, "
                              "and their 10-bit forms 420p10, 422p10, 444p10", safe_text(cs, shown, sizeof(shown)));
        return -1;
    }
    sp->sampling = parse_sampling(Y4M_COLORSPACES[found].sampling);
    sp->bitdepth = Y4M_COLORSPACES[found].depth;
    sp->chromasiting = Y4M_COLORSPACES[found].siting >= 0 ? Y4M_COLORSPACES[found].siting : CS_JPEG;
    sp->have_chromasiting = Y4M_COLORSPACES[found].siting >= 0;
    sp->have_aspect = 1;
    sp->quality = 90;
    sp->have_quality = 1;
    sp->range_full = range_full;
    sp->have_range = 1;
    sp->frame_bytes = specs_frame_bytes(sp);
    if (Y4M_COLORSPACES[found].note) notes_add(notes, noteslen, "mjpegstore: note: %s\n", Y4M_COLORSPACES[found].note);
    if (interlace == 't' || interlace == 'b' || interlace == 'm') {
        snprintf(err, errlen, "the stream is interlaced (I%c). mjpegstore stores progressive footage only: deinterlace it "
                              "first (to 60p, say) and feed progressive frames", interlace);
        return -1;
    }
    if (interlace != 'p')
        notes_add(notes, noteslen, "mjpegstore: note: the stream does not say whether it is interlaced (I%c); it is taken as progressive.\n"
                                   "         Interlaced footage must be deinterlaced before it is fed in.\n", interlace);
    return 0;
}

/* Creates <dir>/specs.txt with its complete content in a single atomic
 * step, and fails with EEXIST if it already exists. (Plain exclusive
 * create makes the file empty first and fills it a moment later, so a
 * second process running at the same time could read a half-written
 * file.) The temp file is a dotfile so it can never look like a frame or
 * a stray file if the process is killed mid-way. Filesystems without hard
 * links fall back to the plain exclusive create. */
static int specs_write_atomic(const char *dir, const specs_t *sp) {
    char path[PATH_MAX + 32], tmp[PATH_MAX + 64];
    snprintf(path, sizeof(path), "%s/%s", dir, SPECS_FILENAME);
    snprintf(tmp, sizeof(tmp), "%s/.%s.tmp%ld", dir, SPECS_FILENAME, (long)getpid());
    unlink(tmp);   /* a stale one from a killed run with the same pid */
    if (specs_write(tmp, sp) != 0) return -1;
    int r = link(tmp, path);
    int e = errno;
    unlink(tmp);
    if (r == 0) return 0;
    if (e == EEXIST) { errno = EEXIST; return -1; }
    return specs_write(path, sp);
}

/* Print each spec that differs between specs.txt (a) and the stream (b). */
static size_t specs_diff(const specs_t *a, const specs_t *b) {
    size_t n = 0;
    char x[64], y[64];
    if (a->width != b->width || a->height != b->height) {
        snprintf(x, sizeof(x), "%dx%d", a->width, a->height);
        snprintf(y, sizeof(y), "%dx%d", b->width, b->height);
        fprintf(stderr, "  %-11s specs.txt %-14s stream %s\n", "resolution", x, y);
        n++;
    }
    if (a->sampling != b->sampling) {
        fprintf(stderr, "  %-11s specs.txt %-14s stream %s\n", "sampling", SAMPLINGS[a->sampling].name, SAMPLINGS[b->sampling].name);
        n++;
    }
    if (a->bitdepth != b->bitdepth) {
        snprintf(x, sizeof(x), "%d", a->bitdepth);
        snprintf(y, sizeof(y), "%d", b->bitdepth);
        fprintf(stderr, "  %-11s specs.txt %-14s stream %s\n", "bitdepth", x, y);
        n++;
    }
    if (a->fps_num != b->fps_num || a->fps_den != b->fps_den) {
        snprintf(x, sizeof(x), "%ld:%ld", a->fps_num, a->fps_den);
        snprintf(y, sizeof(y), "%ld:%ld", b->fps_num, b->fps_den);
        fprintf(stderr, "  %-11s specs.txt %-14s stream %s\n", "framerate", x, y);
        n++;
    }
    if (a->asp_num != b->asp_num || a->asp_den != b->asp_den) {
        snprintf(x, sizeof(x), "%ld:%ld", a->asp_num, a->asp_den);
        snprintf(y, sizeof(y), "%ld:%ld", b->asp_num, b->asp_den);
        fprintf(stderr, "  %-11s specs.txt %-14s stream %s\n", "aspect", x, y);
        n++;
    }
    if (is_420(a) && a->bitdepth == 8 && a->chromasiting != b->chromasiting) {
        snprintf(x, sizeof(x), "%s%s", SITINGS[a->chromasiting].name, a->have_chromasiting ? "" : " (no line)");
        fprintf(stderr, "  %-11s specs.txt %-14s stream %s\n", "chromasiting", x, SITINGS[b->chromasiting].name);
        n++;
    }
    return n;
}

/* ---- live frame counter (-p) --------------------------------------------- */
/* A piped stream has no known size, so there is no progress bar; without   */
/* this a long "ffmpeg ... | mjpegstore -i -" prints nothing until it is done.  */
/* -p shows a running count of frames read. On a terminal it is one line    */
/* redrawn about 4 times a second; if stderr is redirected (a log file) it  */
/* is a plain line every couple of seconds instead, so a log doesn't fill   */
/* with carriage-return redraws. It is opt-in because upstream tools such   */
/* as ffmpeg draw their own \r status line on the same terminal, and two    */
/* programs redrawing one line garble each other. Only the reading thread   */
/* ever touches it.                                                         */

static int g_progress_count = 0;      /* -p was given */

static struct {
    int on;          /* counting for this run */
    int tty;         /* stderr is a terminal */
    int drawn;       /* a \r line is on screen: end it before printing anything else */
    double t0, last;
} g_ctr;

static double mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void counter_start(int enable) {
    memset(&g_ctr, 0, sizeof(g_ctr));
    g_ctr.on = enable;
    g_ctr.tty = isatty(STDERR_FILENO);
    g_ctr.t0 = g_ctr.last = mono_now();
}

static void counter_draw(const char *label, uint64_t frames, uint64_t bytes) {
    double now = mono_now(), el = now - g_ctr.t0;
    double fps = el > 0.05 ? (double)frames / el : 0.0;
    int secs = (int)el;
    fprintf(stderr, g_ctr.tty ? "\rmjpegstore: %s: %llu frames, %.1f MB, %.1f frames/s, %d:%02d elapsed   "
                              : "mjpegstore: %s: %llu frames, %.1f MB, %.1f frames/s, %d:%02d elapsed\n",
            label, (unsigned long long)frames, (double)bytes / 1e6, fps, secs / 60, secs % 60);
    g_ctr.drawn = g_ctr.tty;
    g_ctr.last = now;
}

/* Called after every frame; draws only when enough time has passed. */
static void counter_update(const char *label, uint64_t frames, uint64_t bytes) {
    if (!g_ctr.on) return;
    if (mono_now() - g_ctr.last < (g_ctr.tty ? 0.25 : 2.0)) return;
    counter_draw(label, frames, bytes);
}

/* Ends a redrawn line, so the next message starts on a fresh one. */
static void counter_break(void) {
    if (g_ctr.drawn) {
        fputc('\n', stderr);
        g_ctr.drawn = 0;
    }
}

/* The last state, then a clean line end. (In a log the summary line that
 * follows already says the total, so only a terminal gets a final redraw.) */
static void counter_finish(const char *label, uint64_t frames, uint64_t bytes) {
    if (!g_ctr.on) return;
    if (g_ctr.tty) counter_draw(label, frames, bytes);
    counter_break();
}

/* Opens the y4m source for -i / -e: "-" is stdin (refused if it is a
 * terminal), anything else is a file (a directory is refused with the
 * way to feed one in). flag is "-i" or "-e", for the messages. */
static int y4m_open_source(const char *src, const char *flag, const char *dest, FILE **in_out, off_t *fsize_out) {
    FILE *in;
    if (!strcmp(src, "-")) {
        if (isatty(STDIN_FILENO)) {
            fprintf(stderr, "mjpegstore: refusing to read a video stream from a terminal; pipe it in "
                            "(ffmpeg ... -f yuv4mpegpipe - | mjpegstore %s - %s)\n", flag, dest);
            return 1;
        }
        in = stdin;
    } else {
        struct stat dst;
        if (stat(src, &dst) == 0 && S_ISDIR(dst.st_mode)) {
            fprintf(stderr, "mjpegstore: %s is a directory; %s reads a y4m file or stream.\n"
                            "         To start from a clip directory, pipe another instance in: "
                            "mjpegstore -s %s | mjpegstore %s - %s\n", src, flag, src, flag, dest);
            return 1;
        }
        in = fopen(src, "rb");
        if (!in) {
            fprintf(stderr, "mjpegstore: cannot open %s: %s\n", src, strerror(errno));
            return 1;
        }
    }
    off_t fsize = 0;
    {
        struct stat st;
        if (in != stdin && fstat(fileno(in), &st) == 0 && S_ISREG(st.st_mode)) fsize = st.st_size;
    }
    setvbuf(in, NULL, _IOFBF, INGEST_BUF);
    *in_out = in;
    *fsize_out = fsize;
    return 0;
}

/* Reads and parses the stream header into hs, and says what the stream
 * is. hdr_bytes gets the header's length including its newline. */
static int y4m_read_header(FILE *in, specs_t *hs, int quiet, uint64_t *hdr_bytes) {
    char line[INGEST_LINE_MAX], err[512], notes[1024] = "";
    int lr = ingest_read_line(in, line, sizeof(line));
    if (lr == 1) { fprintf(stderr, "mjpegstore: the input is empty; expected a yuv4mpeg stream\n"); return 1; }
    if (lr != 0) {
        fprintf(stderr, "mjpegstore: could not read a yuv4mpeg header line (%s)\n",
                lr == -2 ? "line too long" : "input ended early");
        return 1;
    }
    memset(hs, 0, sizeof(*hs));
    if (y4m_parse_header(line, hs, err, sizeof(err), notes, sizeof(notes)) != 0) {
        fprintf(stderr, "mjpegstore: %s\n", err);
        return 1;
    }
    if (!quiet) {
        char scs[48] = "";
        if (hs->have_chromasiting) snprintf(scs, sizeof(scs), ", chroma siting %s", SITINGS[hs->chromasiting].name);
        fprintf(stderr, "mjpegstore: stream: %dx%d %s %d-bit%s, %ld:%ld fps, pixel aspect %ld:%ld, progressive, %llu bytes/frame\n",
                hs->width, hs->height, SAMPLINGS[hs->sampling].name, hs->bitdepth, scs, hs->fps_num, hs->fps_den,
                hs->asp_num, hs->asp_den,
                (unsigned long long)hs->frame_bytes);
        fputs(notes, stderr);
    }
    *hdr_bytes = (uint64_t)strlen(line) + 1;
    return 0;
}

/* specs.txt in the set directory (parent): create it from the stream
 * header, or require that the existing one is identical. Returns 0 to
 * carry on, 1 to stop (message printed, nothing written). */
static int specs_settle(const char *parent, const specs_t *hs, int quiet) {
    char spath[PATH_MAX + 32], err[512];
    snprintf(spath, sizeof(spath), "%s/%s", parent, SPECS_FILENAME);
    for (int attempt = 0; attempt < 2; attempt++) {
        specs_t fs;
        int sr = specs_load(spath, &fs, err, sizeof(err));
        if (sr < 0) {
            fprintf(stderr, "mjpegstore: %s: %s\n         The existing specs.txt is left as it is; nothing was written.\n", spath, err);
            return 1;
        }
        if (sr == 0) {
            if (fs.width != hs->width || fs.height != hs->height || fs.sampling != hs->sampling ||
                fs.bitdepth != hs->bitdepth || fs.fps_num != hs->fps_num || fs.fps_den != hs->fps_den ||
                fs.asp_num != hs->asp_num || fs.asp_den != hs->asp_den ||
                (is_420(&fs) && fs.bitdepth == 8 && fs.chromasiting != hs->chromasiting)) {
                fprintf(stderr, "mjpegstore: the stream does not match %s -- stopping, nothing was written:\n", spath);
                specs_diff(&fs, hs);
                fprintf(stderr, "         Use a set whose specs match the stream, or a new set directory.\n");
                return 1;
            }
            if (!fs.have_aspect) {
                if (specs_add_aspect(spath) == 0)
                    fprintf(stderr, "mjpegstore: added aspect=1:1 to %s (it matches the stream)\n", spath);
                else
                    fprintf(stderr, "mjpegstore: warning: could not add aspect=1:1 to %s: %s\n", spath, strerror(errno));
            }
            if (is_420(&fs) && fs.bitdepth == 8 && !fs.have_chromasiting) {
                char cl[48];
                snprintf(cl, sizeof(cl), "chromasiting=%s", SITINGS[hs->chromasiting].name);
                if (specs_append_line(spath, cl) == 0)
                    fprintf(stderr, "mjpegstore: added %s to %s (it matches the stream)\n", cl, spath);
                else
                    fprintf(stderr, "mjpegstore: warning: could not add %s to %s: %s\n", cl, spath, strerror(errno));
            }
            if (!fs.have_quality) {
                char ql[32];
                snprintf(ql, sizeof(ql), "quality=%d", hs->quality);
                if (specs_append_line(spath, ql) == 0) fprintf(stderr, "mjpegstore: added %s to %s\n", ql, spath);
                else fprintf(stderr, "mjpegstore: warning: could not add %s to %s: %s\n", ql, spath, strerror(errno));
            }
            if (!fs.have_range) {
                char rl[32];
                snprintf(rl, sizeof(rl), "range=%s", hs->range_full ? "full" : "limited");
                if (specs_append_line(spath, rl) == 0) fprintf(stderr, "mjpegstore: added %s to %s\n", rl, spath);
                else fprintf(stderr, "mjpegstore: warning: could not add %s to %s: %s\n", rl, spath, strerror(errno));
            }
            if (!quiet) fprintf(stderr, "mjpegstore: %s matches the stream\n", spath);
            return 0;
        }
        if (!path_exists(parent) && mkdir(parent, 0777) != 0 && errno != EEXIST) {
            fprintf(stderr, "mjpegstore: cannot create %s: %s\n", parent, strerror(errno));
            return 1;
        }
        if (specs_write_atomic(parent, hs) == 0) {
            if (!quiet) fprintf(stderr, "mjpegstore: wrote %s from the stream header\n", spath);
            return 0;
        }
        if (errno != EEXIST) {
            fprintf(stderr, "mjpegstore: cannot write %s: %s\n", spath, strerror(errno));
            return 1;
        }
        /* EEXIST: another run created it a moment ago -- look again and compare */
    }
    fprintf(stderr, "mjpegstore: could not settle %s\n", spath);
    return 1;
}

/* dest must be a new or empty directory. *to_create is set to 1 if it
 * does not exist yet. Returns 0 to carry on, 1 to stop. */
static int dest_new_or_empty(const char *dest, int *to_create, const char *hint) {
    struct stat st;
    *to_create = 0;
    if (stat(dest, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) {
            fprintf(stderr, "mjpegstore: %s exists and is not a directory\n", dest);
            return 1;
        }
        DIR *d = opendir(dest);
        if (!d) { fprintf(stderr, "mjpegstore: cannot open %s: %s\n", dest, strerror(errno)); return 1; }
        struct dirent *ent;
        size_t entries = 0;
        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") && strcmp(ent->d_name, "..")) entries++;
        }
        closedir(d);
        if (entries) {
            fprintf(stderr, "mjpegstore: %s already holds %zu entr%s. %s; nothing was written.\n",
                    dest, entries, entries == 1 ? "y" : "ies", hint);
            return 1;
        }
        return 0;
    }
    if (errno != ENOENT) {
        fprintf(stderr, "mjpegstore: cannot stat %s: %s\n", dest, strerror(errno));
        return 1;
    }
    *to_create = 1;
    return 0;
}


/* ---------------------------------------------------------------- */
/* Check mode (-c): create specs.txt if missing, else list it        */
/* ---------------------------------------------------------------- */

/* "%.3f" with trailing zeros dropped: 24000/1001 -> "23.976", 24/1 -> "24". */
static void fps_text(const specs_t *sp, char *buf, size_t n) {
    snprintf(buf, n, "%.3f", (double)sp->fps_num / (double)sp->fps_den);
    size_t l = strlen(buf);
    while (l > 0 && buf[l - 1] == '0') buf[--l] = 0;
    if (l > 0 && buf[l - 1] == '.') buf[--l] = 0;
}

/* The report -c exists for, so it goes to stdout and is shown even
 * with -q. Each spec is shown with the y4m header token it produces. */
static void specs_list(const specs_t *sp, const char *path) {
    const sampling_info_t *si = &SAMPLINGS[sp->sampling];
    char fps[32];
    fps_text(sp, fps, sizeof(fps));
    char val[64], tok[64], note[96];

    printf("%s\n", path);

    snprintf(val, sizeof(val), "%dx%d", sp->width, sp->height);
    snprintf(tok, sizeof(tok), "W%d H%d", sp->width, sp->height);
    printf("  %-11s %-14s -> %s\n", "resolution", val, tok);

    snprintf(tok, sizeof(tok), "C%s", y4m_ctag(sp));
    printf("  %-11s %-14s -> %-12s (%s)\n", "sampling", si->name, tok, si->desc);

    if (is_420(sp)) {
        snprintf(tok, sizeof(tok), "%s", sp->bitdepth > 8 ? "(no y4m tag at 10-bit)" : y4m_ctag(sp));
        printf("  %-11s %-14s -> %s%-12s (%s)%s%s\n", "chromasiting", SITINGS[sp->chromasiting].name,
               sp->bitdepth > 8 ? "" : "C", tok, SITINGS[sp->chromasiting].desc,
               sp->bitdepth > 8 ? "  [kept in specs.txt only]" : "",
               sp->have_chromasiting ? "" : "  [not set in specs.txt: jpeg assumed]");
    }

    snprintf(val, sizeof(val), "%ld:%ld", sp->fps_num, sp->fps_den);
    snprintf(tok, sizeof(tok), "F%ld:%ld", sp->fps_num, sp->fps_den);
    printf("  %-11s %-14s -> %-12s (%s fps)\n", "framerate", val, tok, fps);

    snprintf(val, sizeof(val), "%d", sp->bitdepth);
    if (sp->bitdepth > 8) snprintf(note, sizeof(note), "2 bytes per sample, little-endian (p10 suffix)");
    else                  snprintf(note, sizeof(note), "1 byte per sample (no suffix)");
    printf("  %-11s %-14s -> %s\n", "bitdepth", val, note);

    {
        const char *pd = aspect_preset_desc(sp->asp_num, sp->asp_den);
        char at[32];
        snprintf(val, sizeof(val), "%ld:%ld", sp->asp_num, sp->asp_den);
        snprintf(at, sizeof(at), "A%ld:%ld", sp->asp_num, sp->asp_den);
        printf("  %-11s %-14s -> %-12s (%s)\n", "aspect", val, at, pd ? pd : "custom pixel aspect ratio");
    }
    snprintf(val, sizeof(val), "%d", sp->quality);
    printf("  %-11s %-14s -> JPEG quality, baseline, standard Huffman tables, restart pieces set by -R at ingest%s\n", "quality", val,
           sp->have_quality ? "" : "  [not set in specs.txt: 90 assumed]");
    printf("  %-11s %-14s -> %s%s\n", "range", sp->range_full ? "full" : "limited",
           sp->range_full ? "0-255, flagged full range in the Matroska file" : "16-235 luma, 16-240 chroma, flagged limited range in the Matroska file",
           sp->have_range ? "" : "  [not set in specs.txt: limited assumed]");
    printf("  %-11s %s\n", "stored as", "one JPEG per frame (b00000/frame-0000001.jpg), progressive footage only");
    fflush(stdout);
}

/* -c: find the specs.txt that applies to the named directory (see
 * specs_locate: a set directory's own, or a clip directory's parent's).
 * Missing -> ask for the values and create it; present -> list it.
 * Nothing is scanned, listed or served. Exit status 0 = specs.txt exists
 * and is valid. */
static int check_specs(const char *dir, int quiet) {
    char sdir[PATH_MAX + 8];
    specs_locate(dir, sdir, sizeof(sdir), quiet);
    if (strcmp(sdir, dir) != 0 && !quiet) {
        fprintf(stderr, "mjpegstore: %s holds %s, so it is a clip directory; "
                        "checking its set directory %s\n", dir,
                "frame files", sdir);
    }

    char path[PATH_MAX + 32];
    snprintf(path, sizeof(path), "%s/%s", sdir, SPECS_FILENAME);
    int existed = path_exists(path);

    specs_t sp;
    if (specs_for_dir(sdir, &sp, 1) != 0) return 1;
    if (!existed) {
        if (!path_exists(path)) {
            fprintf(stderr, "mjpegstore: %s could not be created\n", path);
            return 1;
        }
        if (!quiet) fprintf(stderr, "mjpegstore: created %s\n", path);
    }
    specs_list(&sp, path);
    return 0;
}

/* ---------------------------------------------------------------- */
/* Edit mode (-c -E): change specs.txt values from menus             */
/*                                                                   */
/* Each value is shown with its valid choices; Enter keeps it, a     */
/* number picks a choice, and a typed value (or "key=value") is      */
/* checked the same way specs.txt is. Nothing is written until the   */
/* changes have been listed and confirmed. Only the lines that       */
/* change are touched: other lines and comments stay as they are.    */
/* The new file is built beside the old, read back and checked, and  */
/* only then put in place.                                           */
/* ---------------------------------------------------------------- */

typedef struct { const char *value, *desc; } menu_item_t;

static int norm_resolution(const char *s, char *o, size_t n) { int w, h; if (parse_resolution(s, &w, &h)) return -1; snprintf(o, n, "%dx%d", w, h); return 0; }
static int norm_sampling(const char *s, char *o, size_t n) { int i = parse_sampling(s); if (i < 0) return -1; snprintf(o, n, "%s", SAMPLINGS[i].name); return 0; }
static int norm_fps(const char *s, char *o, size_t n) { long a, b; if (parse_fps(s, &a, &b)) return -1; snprintf(o, n, "%ld:%ld", a, b); return 0; }
static int norm_bitdepth(const char *s, char *o, size_t n) { int v; if (parse_bitdepth(s, &v)) return -1; snprintf(o, n, "%d", v); return 0; }
static int norm_quality(const char *s, char *o, size_t n) { int v; if (parse_quality(s, &v)) return -1; snprintf(o, n, "%d", v); return 0; }
static int norm_range(const char *s, char *o, size_t n) { int v; if (parse_range(s, &v)) return -1; snprintf(o, n, "%s", v ? "full" : "limited"); return 0; }
static int norm_aspect(const char *s, char *o, size_t n) { long a, b; if (parse_aspect(s, &a, &b)) return -1; snprintf(o, n, "%ld:%ld", a, b); return 0; }
static int norm_siting(const char *s, char *o, size_t n) { int c = parse_siting(s); if (c < 0) return -1; snprintf(o, n, "%s", SITINGS[c].name); return 0; }

/* Shows the choices for one value and reads the answer. Returns 1 with the
 * new value (in its written form) in out, 0 to keep the current one, -1 if
 * input ended. is_set says whether specs.txt has the line at all: choosing
 * the value that is only assumed still counts as a change, since it writes
 * the line. */
static int edit_field(const char *key, const char *intro, const menu_item_t *items, size_t n, int numbered,
                      const char *cur, int is_set, int (*norm)(const char *, char *, size_t),
                      const char *example, char *out, size_t outn) {
    fprintf(stderr, "\n  %s\n", intro);
    for (size_t i = 0; i < n; i++) {
        int is_cur = !strcmp(items[i].value, cur);
        if (numbered) fprintf(stderr, "    %zu) %-11s %s%s\n", i + 1, items[i].value, items[i].desc, is_cur ? "   <- current" : "");
        else fprintf(stderr, "    %-11s %s%s\n", items[i].value, items[i].desc, is_cur ? "   <- current" : "");
    }
    fprintf(stderr, "    Enter keeps %s%s.%s\n", cur, is_set ? "" : " (not set in specs.txt: assumed)",
            numbered ? " Pick a number, or type a value." : " Type a value.");
    char buf[128];
    for (;;) {
        fprintf(stderr, "  %s [%s]> ", key, cur);
        fflush(stderr);
        if (!fgets(buf, sizeof(buf), stdin)) return -1;
        char *v = strip_key_prefix(trim(buf), key);
        if (!*v) return 0;
        char cand[96];
        snprintf(cand, sizeof(cand), "%s", v);
        if (numbered) {
            char *e;
            long k = strtol(v, &e, 10);
            if (e != v && !*e && k >= 1 && (size_t)k <= n) snprintf(cand, sizeof(cand), "%s", items[k - 1].value);
        }
        if (norm(cand, out, outn) != 0) { fprintf(stderr, "    not valid -- %s\n", example); continue; }
        if (!strcmp(out, cur) && is_set) return 0;
        return 1;
    }
}

static const char *canon_key(const char *k) {
    if (!strcmp(k, "resolution") || !strcmp(k, "sampling") || !strcmp(k, "framerate") ||
        !strcmp(k, "bitdepth") || !strcmp(k, "aspect") || !strcmp(k, "quality") || !strcmp(k, "range")) return k;
    if (!strcmp(k, "chromasiting") || !strcmp(k, "chroma_siting") || !strcmp(k, "chromalocation") || !strcmp(k, "chroma_location")) return "chromasiting";
    return NULL;
}

/* Rewrites <sdir>/specs.txt with the given key changes (an empty value
 * removes the line). Returns 0, or -1 with the reason in err; the old file
 * is left alone unless the new one reads back as valid. */
static int specs_apply_changes(const char *sdir, const char *const *keys, const char *const *vals, size_t nc,
                               char *err, size_t errlen) {
    char path[PATH_MAX + 32], tmp[PATH_MAX + 64];
    snprintf(path, sizeof(path), "%s/%s", sdir, SPECS_FILENAME);
    snprintf(tmp, sizeof(tmp), "%s/.%s.edit%ld", sdir, SPECS_FILENAME, (long)getpid());
    FILE *in = fopen(path, "r");
    if (!in) { snprintf(err, errlen, "cannot open %s: %s", path, strerror(errno)); return -1; }
    struct stat st;
    int have_st = fstat(fileno(in), &st) == 0;
    FILE *o = fopen(tmp, "w");
    if (!o) { snprintf(err, errlen, "cannot create %.250s: %s", tmp, strerror(errno)); fclose(in); return -1; }
    int done[16] = {0}, need_nl = 0;
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, in)) > 0) {
        int handled = 0;
        char *cp = strdup(line);
        if (cp) {
            char *t = trim(cp);
            char *eq = (*t && *t != '#') ? strchr(t, '=') : NULL;
            if (eq) {
                *eq = 0;
                char *key = trim(t);
                for (char *c = key; *c; c++) *c = (char)tolower((unsigned char)*c);
                const char *ck = canon_key(key);
                for (size_t k = 0; ck && k < nc && !handled; k++) {
                    if (strcmp(keys[k], ck) != 0) continue;
                    if (vals[k][0]) fprintf(o, "%s=%s\n", ck, vals[k]);
                    done[k] = 1;
                    handled = 1;
                    need_nl = 0;
                }
            }
            free(cp);
        }
        if (!handled) { fwrite(line, 1, (size_t)len, o); need_nl = line[len - 1] != '\n'; }
    }
    free(line);
    fclose(in);
    if (need_nl) fputc('\n', o);
    for (size_t k = 0; k < nc; k++)
        if (!done[k] && vals[k][0]) fprintf(o, "%s=%s\n", keys[k], vals[k]);
    if (fclose(o) != 0) { snprintf(err, errlen, "write error on %s: %s", tmp, strerror(errno)); unlink(tmp); return -1; }
    specs_t chk;
    char verr[256];
    if (specs_load(tmp, &chk, verr, sizeof(verr)) != 0) {
        snprintf(err, errlen, "the edited file would not be valid (%s); %s is unchanged", verr, path);
        unlink(tmp);
        return -1;
    }
    if (have_st) chmod(tmp, st.st_mode & 07777);
    if (rename(tmp, path) != 0) { snprintf(err, errlen, "cannot replace %s: %s", path, strerror(errno)); unlink(tmp); return -1; }
    return 0;
}

static int edit_specs(const char *dir, int quiet) {
    if (!isatty(STDIN_FILENO)) {
        fprintf(stderr, "mjpegstore: -E needs a terminal to ask its questions; nothing was changed. "
                        "To change specs.txt without one, edit the file.\n");
        return 1;
    }
    char sdir[PATH_MAX + 8], path[PATH_MAX + 32];
    specs_locate(dir, sdir, sizeof(sdir), quiet);
    if (strcmp(sdir, dir) != 0 && !quiet)
        fprintf(stderr, "mjpegstore: %s is a clip directory; editing its set directory %s\n", dir, sdir);
    snprintf(path, sizeof(path), "%s/%s", sdir, SPECS_FILENAME);
    if (!path_exists(path)) return check_specs(dir, quiet);      /* none yet: create it, as -c does */

    specs_t sp;
    char err[PATH_MAX + 700];
    if (specs_load(path, &sp, err, sizeof(err)) != 0) {
        fprintf(stderr, "mjpegstore: %s: %s\n         The file is left as it is; fix it by hand.\n", path, err);
        return 1;
    }
    specs_list(&sp, path);
    fprintf(stderr, "\n  Editing %s. For each value: Enter keeps it, or pick a number / type a value.\n"
                    "  Nothing is written until you have seen the list of changes and said yes.\n", path);

    const char *keys[12], *vals[12];
    char vbuf[12][64], obuf[12][64];
    size_t nc = 0;
    int risky = 0, r;
    char nv[64], cur[64];
    char rbuf[16][32], dbuf[16][64];   /* aspect menu text */

#define CHANGE(key, old, isrisky) do { snprintf(vbuf[nc], sizeof(vbuf[nc]), "%s", nv); snprintf(obuf[nc], sizeof(obuf[nc]), "%s", old); \
        keys[nc] = key; vals[nc] = vbuf[nc]; nc++; if (isrisky) risky = 1; } while (0)
#define ABORT_IF_EOF(x) do { if ((x) < 0) { fprintf(stderr, "\n  input ended; nothing was changed\n"); return 1; } } while (0)

    {   /* resolution */
        static const menu_item_t it[] = {
            {"640x480", "VGA / 4:3 SD"}, {"720x480", "NTSC DVD"}, {"720x576", "PAL DVD"},
            {"1280x720", "HD 720p"}, {"1920x1080", "HD 1080"}, {"3840x2160", "UHD"},
        };
        snprintf(cur, sizeof(cur), "%dx%d", sp.width, sp.height);
        r = edit_field("resolution", "Resolution: width x height in pixels (y4m W and H). Type any WxH.",
                       it, sizeof(it) / sizeof(it[0]), 1, cur, 1, norm_resolution, "expected e.g. 720x480", nv, sizeof(nv));
        ABORT_IF_EOF(r);
        if (r == 1) CHANGE("resolution", cur, 1);
    }
    int samp_after = sp.sampling;
    {   /* sampling */
        static menu_item_t it[3];
        for (size_t i = 0; i < N_SAMPLINGS; i++) { it[i].value = SAMPLINGS[i].name; it[i].desc = SAMPLINGS[i].desc; }
        snprintf(cur, sizeof(cur), "%s", SAMPLINGS[sp.sampling].name);
        r = edit_field("sampling", "Sampling: chroma subsampling (y4m C tag).", it, N_SAMPLINGS, 1, cur, 1, norm_sampling,
                       "pick 420, 422 or 444", nv, sizeof(nv));
        ABORT_IF_EOF(r);
        if (r == 1) { samp_after = parse_sampling(nv); CHANGE("sampling", cur, 1); }
    }
    {   /* framerate */
        static const menu_item_t it[] = {
            {"24000:1001", "23.976 fps: film, most anime"}, {"24:1", "24 fps"}, {"25:1", "25 fps: PAL"},
            {"30000:1001", "29.97 fps: NTSC"}, {"30:1", "30 fps"}, {"50:1", "50 fps"},
            {"60000:1001", "59.94 fps"}, {"60:1", "60 fps"},
        };
        snprintf(cur, sizeof(cur), "%ld:%ld", sp.fps_num, sp.fps_den);
        r = edit_field("framerate", "Frame rate: a fraction N:D, frames per second (y4m F). Write 24 as 24:1.",
                       it, sizeof(it) / sizeof(it[0]), 1, cur, 1, norm_fps, "must be a fraction N:D, e.g. 24000:1001", nv, sizeof(nv));
        ABORT_IF_EOF(r);
        if (r == 1) CHANGE("framerate", cur, 0);
    }
    {   /* bitdepth */
        static const menu_item_t it[] = { {"8", "one byte per sample: the only depth JPEG baseline has"} };
        snprintf(cur, sizeof(cur), "%d", sp.bitdepth);
        r = edit_field("bitdepth", "Bit depth: bits per sample.", it, 1, 0, cur, 1, norm_bitdepth, "the only value is 8", nv, sizeof(nv));
        ABORT_IF_EOF(r);
        if (r == 1) CHANGE("bitdepth", cur, 1);
    }
    const int is420_after = SAMPLINGS[samp_after].hsub == 2 && SAMPLINGS[samp_after].vsub == 2;
    if (!is420_after) {
        if (sp.have_chromasiting) {          /* siting only exists for 4:2:0: drop the line */
            snprintf(nv, sizeof(nv), "%s", "");
            CHANGE("chromasiting", SITINGS[sp.chromasiting].name, 0);
        }
    } else {   /* chroma siting */
        static menu_item_t it[3];
        for (size_t i = 0; i < N_SITINGS; i++) { it[i].value = SITINGS[i].name; it[i].desc = SITINGS[i].desc; }
        snprintf(cur, sizeof(cur), "%s", SITINGS[sp.chromasiting].name);
        r = edit_field("chromasiting", "Chroma siting (4:2:0 only): where chroma sits relative to luma.",
                       it, N_SITINGS, 1, cur, sp.have_chromasiting, norm_siting, "pick jpeg, mpeg2 or paldv", nv, sizeof(nv));
        ABORT_IF_EOF(r);
        if (r == 1) CHANGE("chromasiting", sp.have_chromasiting ? cur : "(not set)", 0);
    }
    {   /* aspect */
        static menu_item_t it[8];
        size_t n = N_ASPECT_PRESETS < 8 ? N_ASPECT_PRESETS : 8;
        for (size_t i = 0; i < n; i++) {
            snprintf(rbuf[i], sizeof(rbuf[i]), "%ld:%ld", ASPECT_PRESETS[i].n, ASPECT_PRESETS[i].d);
            snprintf(dbuf[i], sizeof(dbuf[i]), "%s", ASPECT_PRESETS[i].desc);
            it[i].value = rbuf[i];
            it[i].desc = dbuf[i];
        }
        snprintf(cur, sizeof(cur), "%ld:%ld", sp.asp_num, sp.asp_den);
        r = edit_field("aspect", "Pixel aspect ratio: the shape of one pixel, N:M (y4m A tag). Any N:M is accepted.",
                       it, n, 1, cur, sp.have_aspect, norm_aspect, "expected a ratio N:M, e.g. 10:11", nv, sizeof(nv));
        ABORT_IF_EOF(r);
        if (r == 1) CHANGE("aspect", sp.have_aspect ? cur : "(not set)", 0);
    }
    {   /* quality */
        static const menu_item_t it[] = {
            {"90", "the default: the size/quality balance this tool was tuned at"}, {"95", "finer, bigger"},
            {"100", "the finest a baseline JPEG can be (still lossy)"}, {"85", "smaller still"},
        };
        snprintf(cur, sizeof(cur), "%d", sp.quality);
        fprintf(stderr, "\n  Changing the quality affects only clips ingested afterwards; the stored files stay as they are.\n"
                        "  Clips of different quality in one set do not share bytes with each other.");
        r = edit_field("quality", "\n  JPEG quality: 1..100, higher is finer and bigger.", it, sizeof(it) / sizeof(it[0]), 1, cur,
                       sp.have_quality, norm_quality, "a whole number from 1 to 100", nv, sizeof(nv));
        ABORT_IF_EOF(r);
        if (r == 1) CHANGE("quality", sp.have_quality ? cur : "(not set)", 0);
    }
    {   /* range */
        static const menu_item_t it[] = {
            {"limited", "16-235 luma, 16-240 chroma: normal video"}, {"full", "0-255: only if the source really is full range"},
        };
        snprintf(cur, sizeof(cur), "%s", sp.range_full ? "full" : "limited");
        r = edit_field("range", "Range: how the stored Y/U/V values are to be read (flagged in the Matroska file).",
                       it, 2, 1, cur, sp.have_range, norm_range, "pick limited or full", nv, sizeof(nv));
        ABORT_IF_EOF(r);
        if (r == 1) CHANGE("range", sp.have_range ? cur : "(not set)", 0);
    }

    if (nc == 0) { fprintf(stderr, "\n  No changes; %s is as it was.\n", path); return 0; }
    fprintf(stderr, "\n  Changes to %s:\n", path);
    for (size_t k = 0; k < nc; k++) {
        if (vals[k][0]) fprintf(stderr, "    %-12s %s  ->  %s\n", keys[k], obuf[k], vals[k]);
        else fprintf(stderr, "    %-12s %s  ->  (line removed: 4:2:0 only)\n", keys[k], obuf[k]);
    }
    char buf[64];
    if (risky) {
        static const char *const RISK[] = {
            "",
            "RESOLUTION AND SAMPLING DESCRIBE THE STORED PICTURES",
            "",
            "They fix the size and layout of every picture. Changing them does NOT",
            "convert any stored files: if they no longer match the files, the clips",
            "cannot be served. Use this only to correct a specs.txt that was wrong",
            "from the start, when the files were stored with the right values.",
            "",
        };
        warn_box(RISK, sizeof(RISK) / sizeof(RISK[0]));
        fprintf(stderr, "\n  Type yes to write these changes: ");
        fflush(stderr);
        if (!fgets(buf, sizeof(buf), stdin) || strcmp(trim(buf), "yes") != 0) { fprintf(stderr, "  nothing was changed.\n"); return 1; }
    } else {
        fprintf(stderr, "\n  Write these changes? [y/N] ");
        fflush(stderr);
        if (!fgets(buf, sizeof(buf), stdin) || (buf[0] != 'y' && buf[0] != 'Y')) { fprintf(stderr, "  nothing was changed.\n"); return 1; }
    }
    if (specs_apply_changes(sdir, keys, vals, nc, err, sizeof(err)) != 0) {
        fprintf(stderr, "mjpegstore: %s\n", err);
        return 1;
    }
    if (!quiet) fprintf(stderr, "mjpegstore: updated %s\n", path);
    if (specs_load(path, &sp, err, sizeof(err)) != 0) { fprintf(stderr, "mjpegstore: %s\n", err); return 1; }
    specs_list(&sp, path);
    return 0;
#undef CHANGE
#undef ABORT_IF_EOF
}


typedef struct {
    int nthreads, quiet;
    const char *out_path;    /* the one combined list file; NULL = stdout */
    char **specs;             /* specs.txt paths listed first, before any listed entry */
    size_t nspecs;
} elem_opts_t;

/* Writes an already-gathered list (mem, memlen) to stdout or to out_path.
 * Only called once every listing has succeeded, so a failed run never
 * touches an existing -f file. Returns 0 or 1. */
static int emit_list(const char *mem, size_t memlen, const char *out_path, size_t lines, int quiet) {
    FILE *out = stdout;
    if (out_path) {
        out = fopen(out_path, "w");
        if (!out) {
            fprintf(stderr, "mjpegstore: cannot open %s for writing: %s\n", out_path, strerror(errno));
            return 1;
        }
    }
    int werr = (fwrite(mem, 1, memlen, out) != memlen) || (fflush(out) != 0);
    if (out_path && fclose(out) != 0) werr = 1;
    if (werr) { fprintf(stderr, "mjpegstore: write error on the list: %s\n", strerror(errno)); return 1; }
    if (out_path && !quiet) fprintf(stderr, "mjpegstore: wrote %zu file path(s) to %s\n", lines, out_path);
    return 0;
}

/* Lists the files in natural filename order (numbers compare as numbers):
 * no hashing and no per-file reads beyond the directory scan itself.
 * Always succeeds. */
static int scan_only(char **paths, size_t count, FILE *out) {
    if (count > 0) {
        qsort(paths, count, sizeof(char *), name_natural_cmp);
        for (size_t i = 0; i < count; i++) fprintf(out, "%s\n", paths[i]);
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* main                                                              */
/* ---------------------------------------------------------------- */

/* ---- new set: show what the stream header says, offer to override ---- */

static char *tty_line(FILE *tt, const char *label, char *buf, size_t n) {
    fprintf(stderr, "  %s> ", label);
    fflush(stderr);
    if (!fgets(buf, (int)n, tt)) return NULL;
    return trim(buf);
}

/* Called when specs.txt does not exist yet. Lists what was detected from the
 * stream header and asks whether to override anything. Resolution, sampling
 * and bit depth are not offered: they describe the bytes in the stream.
 * Questions go to the terminal (/dev/tty), so it also works when the y4m
 * arrives on stdin. No terminal: the detected values are used, as before.
 * Returns 0, or 1 to stop (input ended). */
static int specs_override_offer(specs_t *hs) {
    FILE *tt = fopen("/dev/tty", "r");
    if (!tt) return 0;
    char buf[128], fps[48], *v;
    int rc = 0;
    fps_text(hs, fps, sizeof(fps));
    fprintf(stderr, "\n  No specs.txt yet. Detected from the stream header:\n"
                    "    resolution   %dx%d\n    sampling     %s\n    framerate    %ld:%ld (%s fps)\n"
                    "    aspect       %ld:%ld\n",
            hs->width, hs->height, SAMPLINGS[hs->sampling].name, hs->fps_num, hs->fps_den, fps,
            hs->asp_num, hs->asp_den);
    if (is_420(hs)) fprintf(stderr, "    chromasiting %s\n", SITINGS[hs->chromasiting].name);
    fprintf(stderr, "    range        %s\n    quality      %d\n", hs->range_full ? "full" : "limited", hs->quality);
    fprintf(stderr, "  A stream header can be wrong (a missing aspect, say). Resolution and sampling cannot be changed:\n"
                    "  they are the real shape of the data. Only progressive footage is stored.\n");
    for (;;) {
        v = tty_line(tt, "override any of these? [y/N]", buf, sizeof(buf));
        if (!v) { rc = 1; goto out; }
        if (!*v || !strcasecmp(v, "n") || !strcasecmp(v, "no")) goto out;
        if (!strcasecmp(v, "y") || !strcasecmp(v, "yes")) break;
        fprintf(stderr, "    answer y or n\n");
    }
    fprintf(stderr, "  Enter keeps the detected value.\n");
#define ASK(label, cur, okexpr, hint)                                                   \
    for (;;) {                                                                         \
        char lb[96];                                                                   \
        snprintf(lb, sizeof(lb), "%s [%s]", label, cur);                               \
        if (!(v = tty_line(tt, lb, buf, sizeof(buf)))) { rc = 1; goto out; }           \
        if (!*v) break;                                                                \
        if (okexpr) break;                                                             \
        fprintf(stderr, "    not valid -- %s\n", hint);                                \
    }
    {   long a, b; char cur[32]; snprintf(cur, sizeof(cur), "%ld:%ld", hs->fps_num, hs->fps_den);
        ASK("framerate", cur, parse_fps(v, &a, &b) == 0 && (hs->fps_num = a, hs->fps_den = b, 1), "a fraction N:D, e.g. 24000:1001")
    }
    {   long a, b; char cur[32]; snprintf(cur, sizeof(cur), "%ld:%ld", hs->asp_num, hs->asp_den);
        ASK("aspect", cur, parse_aspect(v, &a, &b) == 0 && (hs->asp_num = a, hs->asp_den = b, 1), "a ratio N:M, e.g. 1:1 or 40:33")
    }
    if (is_420(hs)) {
        int c;
        ASK("chromasiting", SITINGS[hs->chromasiting].name, (c = parse_siting(v)) >= 0 && (hs->chromasiting = c, hs->have_chromasiting = 1, 1),
            "jpeg, mpeg2 or paldv")
    }
    {   int r;
        ASK("range", hs->range_full ? "full" : "limited", parse_range(v, &r) == 0 && (hs->range_full = r, 1), "limited or full")
    }
    {   int q; char cur[16]; snprintf(cur, sizeof(cur), "%d", hs->quality);
        ASK("quality", cur, parse_quality(v, &q) == 0 && (hs->quality = q, 1), "a whole number 1..100")
    }
#undef ASK
    fprintf(stderr, "  using framerate %ld:%ld, aspect %ld:%ld, range %s, quality %d\n",
            hs->fps_num, hs->fps_den, hs->asp_num, hs->asp_den,
            hs->range_full ? "full" : "limited", hs->quality);
out:
    fclose(tt);
    return rc;
}

/* specs.txt exists, and the stream's labels (aspect, frame rate, chroma
 * siting) differ from it. Those are labels, not the shape of the data, so offer to go
 * with specs.txt's values instead of stopping. Resolution, sampling and bit depth
 * still must match: those are left for specs_settle to refuse. No terminal, an
 * answer of no, or input ending: nothing changes and specs_settle reports the
 * difference as before. */
static void specs_adopt_offer(const char *parent, specs_t *hs) {
    char spath[PATH_MAX + 32], err[512], buf[64];
    snprintf(spath, sizeof(spath), "%s/%s", parent, SPECS_FILENAME);
    specs_t fs;
    if (specs_load(spath, &fs, err, sizeof(err)) != 0) return;
    if (fs.width != hs->width || fs.height != hs->height || fs.sampling != hs->sampling || fs.bitdepth != hs->bitdepth) return;
    const int cs_differs = is_420(&fs) && fs.bitdepth == 8 && fs.chromasiting != hs->chromasiting;
    if (fs.fps_num == hs->fps_num && fs.fps_den == hs->fps_den && fs.asp_num == hs->asp_num && fs.asp_den == hs->asp_den &&
        !cs_differs) return;
    FILE *tt = fopen("/dev/tty", "r");
    if (!tt) return;
    fprintf(stderr, "mjpegstore: the stream's labels differ from %s:\n", spath);
    specs_diff(&fs, hs);
    for (;;) {
        fprintf(stderr, "  use the specs.txt values and continue? [y/N]> ");
        fflush(stderr);
        if (!fgets(buf, (int)sizeof(buf), tt)) break;
        char *v = trim(buf);
        if (!*v || !strcasecmp(v, "n") || !strcasecmp(v, "no")) break;
        if (!strcasecmp(v, "y") || !strcasecmp(v, "yes")) {
            hs->fps_num = fs.fps_num; hs->fps_den = fs.fps_den;
            hs->asp_num = fs.asp_num; hs->asp_den = fs.asp_den;
            if (cs_differs) { hs->chromasiting = fs.chromasiting; hs->have_chromasiting = fs.have_chromasiting; }
            fprintf(stderr, "  using the specs.txt values\n");
            break;
        }
        fprintf(stderr, "    answer y or n\n");
    }
    fclose(tt);
}

/* ---------------------------------------------------------------- */
/* Ingest (-i): a y4m stream becomes a clip directory of JPEG files   */
/*                                                                   */
/*   mjpegstore -i FILE|- <clipdir>                                   */
/*                                                                   */
/* Every frame is encoded as a baseline JPEG straight from the       */
/* stream's Y, U and V planes (no colour conversion), with the       */
/* quality from specs.txt, the standard Huffman tables and a restart */
/* marker after every row of blocks. The same tables in every frame, */
/* and restarts that realign the stream and reset the DC prediction  */
/* each row, are what let identical picture rows come out as         */
/* identical bytes in different frames, so a compressor that looks   */
/* back over the whole set can match them.                           */
/*                                                                   */
/* Progressive footage only: one whole picture per frame.            */
/* ---------------------------------------------------------------- */

typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf jb;
    char msg[JMSG_LENGTH_MAX];
} jpeg_err_t;

static void jpeg_err_exit(j_common_ptr c) {
    jpeg_err_t *e = (jpeg_err_t *)c->err;
    (*c->err->format_message)(c, e->msg);
    longjmp(e->jb, 1);
}
static void jpeg_err_emit(j_common_ptr c, int level) { (void)c; (void)level; }
static void jpeg_err_out(j_common_ptr c) { (void)c; }

/* Per-thread encoder state: padded copies of the planes and the output buffer. */
typedef struct {
    unsigned char *pad[3];
    size_t padcap[3];
    unsigned char *buf;       /* the JPEG is built here ... */
    unsigned long cap;
    unsigned char *ob;        /* ... libjpeg's view of it (it may swap in a bigger one) */
    unsigned long osz;
} enc_t;

static void enc_free(enc_t *e) {
    for (int i = 0; i < 3; i++) free(e->pad[i]);
    free(e->buf);
}

__attribute__((noinline)) static unsigned char *plane_pad(enc_t *e, int i, size_t need) {
    if (e->padcap[i] < need) {
        unsigned char *p = realloc(e->pad[i], need);
        if (!p) return NULL;
        e->pad[i] = p;
        e->padcap[i] = need;
    }
    return e->pad[i];
}

/* Copies a w x h plane (rows `stride` apart) into the top-left of a pw x ph
 * buffer, repeating the last column and the last row into the padding. */
static void pad_plane(unsigned char *dst, size_t pw, size_t ph, const unsigned char *src, size_t stride,
                      size_t w, size_t h) {
    for (size_t r = 0; r < ph; r++) {
        unsigned char *d = dst + r * pw;
        if (r < h) {
            memcpy(d, src + r * stride, w);
            if (pw > w) memset(d + w, d[w - 1], pw - w);
        } else {
            memcpy(d, dst + (h - 1) * pw, pw);
        }
    }
}

/* Encodes one picture from three planes (luma w x h, chroma cw x ch, each
 * `stride` bytes apart). On success *out and *outlen describe the JPEG, which
 * stays valid until the next call with this enc_t; *heap is set when the
 * library had to move it to a buffer of its own that the caller must free. */
/* -R N: a restart marker after every N MCUs (0 = once per row of blocks). The default, one per MCU (16x16
 * pixels in 4:2:0), gave the smallest archives: a change in one place spoils the fewest bytes. */
static unsigned g_restart_mcus = 1;

static int g_assume_yes = 0;          /* -y: a new set takes the settings detected from the stream header, without asking */

static int jpeg_encode(const specs_t *sp, enc_t *e, const unsigned char *const src[3], const size_t stride[3],
                       int w, int h, int cw, int ch, unsigned char **out, unsigned long *outlen, int *heap,
                       char *err, size_t errn) {
    const int hs = SAMPLINGS[sp->sampling].hsub, vs = SAMPLINGS[sp->sampling].vsub;
    const size_t pw = ((size_t)w + (size_t)(8 * hs) - 1) / (size_t)(8 * hs) * (size_t)(8 * hs);
    const size_t ph = ((size_t)h + (size_t)(8 * vs) - 1) / (size_t)(8 * vs) * (size_t)(8 * vs);
    const size_t pcw = pw / (size_t)hs, pch = ph / (size_t)vs;
    unsigned char *volatile py = plane_pad(e, 0, pw * ph), *volatile pu = plane_pad(e, 1, pcw * pch), *volatile pv = plane_pad(e, 2, pcw * pch);
    size_t need = 4 * (size_t)w * (size_t)h + (1u << 20);
    if (e->cap < need) {
        unsigned char *nb = realloc(e->buf, need);
        if (nb) { e->buf = nb; e->cap = (unsigned long)need; }
    }
    if (!py || !pu || !pv || e->cap < need) { snprintf(err, errn, "out of memory"); return -1; }
    pad_plane(py, pw, ph, src[0], stride[0], (size_t)w, (size_t)h);
    pad_plane(pu, pcw, pch, src[1], stride[1], (size_t)cw, (size_t)ch);
    pad_plane(pv, pcw, pch, src[2], stride[2], (size_t)cw, (size_t)ch);

    struct jpeg_compress_struct ci;
    jpeg_err_t je;
    memset(&ci, 0, sizeof(ci));
    ci.err = jpeg_std_error(&je.pub);
    je.pub.error_exit = jpeg_err_exit;
    je.pub.emit_message = jpeg_err_emit;
    je.pub.output_message = jpeg_err_out;
    je.msg[0] = 0;
    e->ob = e->buf;
    e->osz = e->cap;
    if (setjmp(je.jb)) {
        jpeg_destroy_compress(&ci);
        snprintf(err, errn, "JPEG encoder: %s", je.msg);
        return -1;
    }
    jpeg_create_compress(&ci);
    jpeg_mem_dest(&ci, &e->ob, &e->osz);
    ci.image_width = (JDIMENSION)w;
    ci.image_height = (JDIMENSION)h;
    ci.input_components = 3;
    ci.in_color_space = JCS_YCbCr;
    jpeg_set_defaults(&ci);
    jpeg_set_colorspace(&ci, JCS_YCbCr);
    ci.raw_data_in = TRUE;                   /* the planes go in as they are: no colour conversion, no resampling */
    jpeg_set_quality(&ci, sp->quality, TRUE);
    ci.optimize_coding = FALSE;              /* the standard Huffman tables, the same in every frame */
    ci.dct_method = JDCT_ISLOW;              /* the exact integer transform: the same bytes on any machine */
    ci.comp_info[0].h_samp_factor = hs;
    ci.comp_info[0].v_samp_factor = vs;
    for (int i = 1; i < 3; i++) { ci.comp_info[i].h_samp_factor = 1; ci.comp_info[i].v_samp_factor = 1; }
    {   /* a restart marker every g_restart_mcus MCUs, or once per row of blocks if that is 0. Set as an MCU count,
         * not restart_in_rows, so that no library has to work it out for us. */
        unsigned long mcus = ((unsigned long)w + (unsigned long)(8 * hs) - 1) / (unsigned long)(8 * hs);
        ci.restart_in_rows = 0;
        ci.restart_interval = (unsigned)(mcus > 65535 ? 65535 : mcus);
        if (g_restart_mcus > 0 && g_restart_mcus < ci.restart_interval) ci.restart_interval = g_restart_mcus;
    }
    {   /* pixel aspect in the JFIF header (units 0: only the ratio counts) */
        long n = sp->asp_num, d = sp->asp_den;
        while (n > 65535 || d > 65535) { n = (n + 1) / 2; d = (d + 1) / 2; }
        ci.density_unit = 0;
        ci.X_density = (UINT16)(n < 1 ? 1 : n);
        ci.Y_density = (UINT16)(d < 1 ? 1 : d);
    }
    jpeg_start_compress(&ci, TRUE);
    JSAMPROW yr[16], ur[16], vr[16];
    JSAMPARRAY planes[3] = { yr, ur, vr };
    for (size_t row = 0; row < ph; row += (size_t)(8 * vs)) {
        for (int i = 0; i < 8 * vs; i++) yr[i] = py + (row + (size_t)i) * pw;
        for (int i = 0; i < 8; i++) {
            size_t cr = row / (size_t)vs + (size_t)i;
            ur[i] = pu + cr * pcw;
            vr[i] = pv + cr * pcw;
        }
        jpeg_write_raw_data(&ci, planes, (JDIMENSION)(8 * vs));
    }
    jpeg_finish_compress(&ci);
    jpeg_destroy_compress(&ci);
    *out = e->ob;
    *outlen = e->osz;
    *heap = (e->ob != e->buf);
    return 0;
}

/* The file name of a stored picture: <dir>/b00000/frame-0000001.jpg (5000 frames to a bucket folder). */
static void jpeg_frame_name(char *buf, size_t n, const char *dir, uint64_t idx) {
    snprintf(buf, n, "%s/b%05llu/frame-%07llu.jpg", dir, (unsigned long long)((idx - 1) / BUCKET_FRAMES),
             (unsigned long long)idx);
}

/* Makes the folder a picture is to be written into (its bucket), if it is not there yet. */
static void ensure_parent_dir(const char *path) {
    char d[PATH_MAX + 80];
    const char *slash = strrchr(path, '/');
    if (!slash) return;
    snprintf(d, sizeof(d), "%.*s", (int)(slash - path), path);
    if (mkdir(d, 0777) != 0 && errno != EEXIST) { /* the write that follows reports the failure */ }
}

/* Writes path through a dotfile beside it and renames it into place, so a
 * listing made while ingest runs never sees half a file. */
static int write_file_atomic(const char *path, const unsigned char *data, size_t len, char *err, size_t errn) {
    char tmp[PATH_MAX + 80];
    const char *slash = strrchr(path, '/');
    if (slash) snprintf(tmp, sizeof(tmp), "%.*s/.%s.part", (int)(slash - path), path, slash + 1);
    else snprintf(tmp, sizeof(tmp), ".%s.part", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) { snprintf(err, errn, "cannot create %.250s: %s", tmp, strerror(errno)); return -1; }
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            snprintf(err, errn, "write error on %.250s: %s", path, strerror(errno));
            close(fd);
            unlink(tmp);
            return -1;
        }
        off += (size_t)w;
    }
    if (close(fd) != 0) { snprintf(err, errn, "write error on %.250s: %s", path, strerror(errno)); unlink(tmp); return -1; }
    if (rename(tmp, path) != 0) { snprintf(err, errn, "cannot rename %.250s: %s", tmp, strerror(errno)); unlink(tmp); return -1; }
    return 0;
}

/* The reader fills slots with raw frames; worker threads encode them. */
typedef struct {
    unsigned char *buf;
    uint64_t idx;        /* 1-based frame number */
    int state;           /* 0 free, 1 waiting to be encoded, 2 being encoded */
} ing_slot_t;

typedef struct {
    const specs_t *sp;
    const char *dest;
    ing_slot_t *slot;
    int nslots;
    pthread_mutex_t m;
    pthread_cond_t ready, freed;
    int closed;          /* no more frames will come */
    int failed;
    uint64_t fail_idx;   /* the lowest frame that failed */
    char err[700];
    uint64_t bytes_out;
} ing_t;

static void *ing_worker(void *arg) {
    ing_t *g = arg;
    enc_t e;
    memset(&e, 0, sizeof(e));
    const specs_t *sp = g->sp;
    const sampling_info_t *si = &SAMPLINGS[sp->sampling];
    const int w = sp->width, h = sp->height;
    const int cw = (w + si->hsub - 1) / si->hsub, ch = (h + si->vsub - 1) / si->vsub;
    const size_t ysz = (size_t)w * (size_t)h, csz = (size_t)cw * (size_t)ch;
    for (;;) {
        pthread_mutex_lock(&g->m);
        int pick = -1;
        for (;;) {
            for (int i = 0; i < g->nslots; i++)
                if (g->slot[i].state == 1 && (pick < 0 || g->slot[i].idx < g->slot[pick].idx)) pick = i;
            if (pick >= 0 || g->closed) break;
            pthread_cond_wait(&g->ready, &g->m);
        }
        if (pick < 0) { pthread_mutex_unlock(&g->m); break; }
        g->slot[pick].state = 2;
        const uint64_t idx = g->slot[pick].idx;
        const unsigned char *buf = g->slot[pick].buf;
        const int skip = g->failed && idx > g->fail_idx;      /* after a failure the later frames are pointless */
        pthread_mutex_unlock(&g->m);

        char err[600] = "";
        uint64_t wrote = 0;
        int rc = 0;
        if (!skip) {
            const unsigned char *Y = buf, *U = buf + ysz, *V = buf + ysz + csz;
            const unsigned char *src[3] = { Y, U, V };
            const size_t stride[3] = { (size_t)w, (size_t)cw, (size_t)cw };
            unsigned char *jb = NULL;
            unsigned long jl = 0;
            int heap = 0;
            rc = jpeg_encode(sp, &e, src, stride, w, h, cw, ch, &jb, &jl, &heap, err, sizeof(err));
            if (rc == 0) {
                char path[PATH_MAX + 64];
                jpeg_frame_name(path, sizeof(path), g->dest, idx);
                ensure_parent_dir(path);
                rc = write_file_atomic(path, jb, jl, err, sizeof(err));
                wrote += jl;
            }
            if (heap) free(jb);
        }
        pthread_mutex_lock(&g->m);
        if (rc != 0 && (!g->failed || idx < g->fail_idx)) {
            g->failed = 1;
            g->fail_idx = idx;
            snprintf(g->err, sizeof(g->err), "frame %llu: %s", (unsigned long long)idx, err);
        }
        g->bytes_out += wrote;
        g->slot[pick].state = 0;
        pthread_cond_signal(&g->freed);
        pthread_mutex_unlock(&g->m);
    }
    enc_free(&e);
    return NULL;
}

static int ingest_jpeg(const char *src, const char *dest, int quiet, int nthreads, int quality) {
    int rc = 1;
    FILE *in = NULL;
    int created_dest = 0;
    uint64_t frames = 0;
    char line[INGEST_LINE_MAX];
    ing_t g;
    memset(&g, 0, sizeof(g));
    pthread_t *tids = NULL;
    int nstarted = 0, sync_ready = 0;

    if (!strcmp(dest, "-")) {
        fprintf(stderr, "mjpegstore: -i needs a clip directory to write into\n");
        return 1;
    }
    off_t fsize = 0;
    if (y4m_open_source(src, "-i", dest, &in, &fsize) != 0) return 1;

    /* 1. the header, before anything is touched */
    specs_t hs;
    uint64_t consumed = 0;
    if (y4m_read_header(in, &hs, quiet, &consumed) != 0) goto done;
    if (hs.bitdepth > 8) {
        fprintf(stderr, "mjpegstore: the stream is %d-bit; JPEG stores 8 bits per sample, so it cannot be stored here.\n"
                        "         Convert it first, e.g. ffmpeg ... -pix_fmt yuv420p -f yuv4mpegpipe -\n", hs.bitdepth);
        goto done;
    }
    hs.quality = quality;
    hs.have_quality = 1;
    {   /* a new set: show what the header said and offer to override it */
        char pdir[PATH_MAX + 8], sp0[PATH_MAX + 32];
        parent_of(dest, pdir, sizeof(pdir));
        snprintf(sp0, sizeof(sp0), "%s/%s", pdir, SPECS_FILENAME);
        if (!path_exists(sp0) && !g_assume_yes && specs_override_offer(&hs) != 0) {
            fprintf(stderr, "mjpegstore: input ended; nothing was written.\n");
            goto done;
        }
        if (path_exists(sp0)) specs_adopt_offer(pdir, &hs);
    }
    if (!quiet) fprintf(stderr, "mjpegstore: writing JPEG frames to %s\n", dest);

    /* 2. the destination must be new or empty */
    if (dest_new_or_empty(dest, &created_dest, "-i writes only into a new or empty clip directory") != 0) goto done;

    /* 3. specs.txt in the set directory (the clip's parent): made from the
     *    stream if absent, or it must agree with the stream; then read back,
     *    because the quality and range to encode with are the file's */
    char parent[PATH_MAX + 8];
    parent_of(dest, parent, sizeof(parent));
    if (specs_settle(parent, &hs, quiet) != 0) goto done;
    specs_t sp;
    {
        char spath[PATH_MAX + 32], err[512];
        snprintf(spath, sizeof(spath), "%s/%s", parent, SPECS_FILENAME);
        if (specs_load(spath, &sp, err, sizeof(err)) != 0) {
            fprintf(stderr, "mjpegstore: %s: %s\n", spath, err);
            goto done;
        }
    }
    if (!quiet) {
        if (g_restart_mcus)
            fprintf(stderr, "mjpegstore: JPEG quality %d, restart marker every %u MCU%s, standard Huffman tables\n",
                    sp.quality, g_restart_mcus, g_restart_mcus == 1 ? "" : "s");
        else
            fprintf(stderr, "mjpegstore: JPEG quality %d, restart marker after every row of blocks, standard Huffman tables\n",
                    sp.quality);
    }

    /* 4. the frames */
    if (created_dest && mkdir(dest, 0777) != 0 && errno != EEXIST) {
        fprintf(stderr, "mjpegstore: cannot create %s: %s\n", dest, strerror(errno));
        created_dest = 0;
        goto done;
    }
    if (nthreads < 1) nthreads = 1;
    g.sp = &sp;
    g.dest = dest;
    g.nslots = 2 * nthreads + 2;
    g.slot = calloc((size_t)g.nslots, sizeof(*g.slot));
    tids = calloc((size_t)nthreads, sizeof(*tids));
    if (!g.slot || !tids) { fprintf(stderr, "mjpegstore: out of memory\n"); goto done; }
    for (int i = 0; i < g.nslots; i++) {
        g.slot[i].buf = malloc((size_t)hs.frame_bytes);
        if (!g.slot[i].buf) { fprintf(stderr, "mjpegstore: out of memory\n"); goto done; }
    }
    pthread_mutex_init(&g.m, NULL);
    pthread_cond_init(&g.ready, NULL);
    pthread_cond_init(&g.freed, NULL);
    sync_ready = 1;
    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&tids[i], NULL, ing_worker, &g) != 0) break;
        nstarted++;
    }
    if (nstarted == 0) { fprintf(stderr, "mjpegstore: cannot start an encoder thread\n"); goto done; }

    counter_start(g_progress_count && !quiet && fsize == 0);   /* a file source has the progress bar */
    if (g_ctr.on) counter_draw("ingesting", 0, 0);
    int read_failed = 0;
    for (;;) {
        int fr = ingest_read_line(in, line, sizeof(line));
        if (fr == 1) break;   /* clean end of stream, on a frame boundary */
        if (fr != 0) {
            counter_break();
            fprintf(stderr, "mjpegstore: stream damaged after %llu frame%s: %s\n", (unsigned long long)frames,
                    frames == 1 ? "" : "s", fr == -2 ? "frame header too long" : "input ended inside a frame header");
            read_failed = 1;
            break;
        }
        if (strncmp(line, "FRAME", 5) != 0 || (line[5] != 0 && line[5] != ' ')) {
            char shown[24];
            counter_break();
            fprintf(stderr, "mjpegstore: stream damaged after %llu frame%s: expected FRAME, found '%s'\n",
                    (unsigned long long)frames, frames == 1 ? "" : "s", safe_text(line, shown, sizeof(shown)));
            read_failed = 1;
            break;
        }
        consumed += (uint64_t)strlen(line) + 1;
        /* a free slot to read into; stop at once if an encoder has failed */
        int si = -1;
        pthread_mutex_lock(&g.m);
        for (;;) {
            if (g.failed) break;
            for (int i = 0; i < g.nslots && si < 0; i++) if (g.slot[i].state == 0) si = i;
            if (si >= 0) break;
            pthread_cond_wait(&g.freed, &g.m);
        }
        pthread_mutex_unlock(&g.m);
        if (si < 0) break;
        size_t got = fread(g.slot[si].buf, 1, (size_t)hs.frame_bytes, in);
        if (got != (size_t)hs.frame_bytes) {
            counter_break();
            fprintf(stderr, "mjpegstore: stream ended inside frame %llu (%llu of %llu bytes); "
                            "%llu complete frame%s kept in %s\n", (unsigned long long)(frames + 1),
                    (unsigned long long)got, (unsigned long long)hs.frame_bytes,
                    (unsigned long long)frames, frames == 1 ? "" : "s", dest);
            read_failed = 1;
            break;
        }
        pthread_mutex_lock(&g.m);
        g.slot[si].idx = frames + 1;
        g.slot[si].state = 1;
        pthread_cond_signal(&g.ready);
        pthread_mutex_unlock(&g.m);
        consumed += hs.frame_bytes;
        frames++;
        if (fsize > 0 && !quiet) progress_bar("ingesting", (size_t)consumed, (size_t)fsize);
        counter_update("ingesting", frames, consumed);
    }
    /* let the encoders finish what has been queued */
    pthread_mutex_lock(&g.m);
    g.closed = 1;
    pthread_cond_broadcast(&g.ready);
    pthread_mutex_unlock(&g.m);
    for (int i = 0; i < nstarted; i++) pthread_join(tids[i], NULL);
    nstarted = 0;
    counter_finish("ingesting", frames, consumed);
    if (fsize > 0 && !quiet) progress_bar("ingesting", (size_t)fsize, (size_t)fsize);

    if (g.failed) {
        /* frames from the failed one on may be missing or incomplete: remove them, so what is left is a clean prefix */
        counter_break();
        fprintf(stderr, "mjpegstore: %s\n", g.err);
        for (uint64_t i = g.fail_idx; i <= frames; i++) {
            char p[PATH_MAX + 64];
            jpeg_frame_name(p, sizeof(p), dest, i);
            unlink(p);
        }
        frames = g.fail_idx - 1;
        fprintf(stderr, "mjpegstore: %llu complete frame%s kept in %s\n", (unsigned long long)frames,
                frames == 1 ? "" : "s", dest);
        goto done;
    }
    if (read_failed) goto done;
    if (frames == 0) {
        fprintf(stderr, "mjpegstore: the stream has a header but no frames\n");
        goto done;
    }
    if (!quiet) {
        fprintf(stderr, "mjpegstore: ingested %llu frame%s into %s: %.1f MB of JPEG from %.1f MB raw (%.1f%%), "
                        "%.1f KB per frame, %.1f s at %ld:%ld fps\n",
                (unsigned long long)frames, frames == 1 ? "" : "s", dest,
                (double)g.bytes_out / 1e6, (double)frames * (double)hs.frame_bytes / 1e6,
                100.0 * (double)g.bytes_out / ((double)frames * (double)hs.frame_bytes),
                (double)g.bytes_out / (double)frames / 1e3,
                (double)frames * (double)hs.fps_den / (double)hs.fps_num, hs.fps_num, hs.fps_den);
    }
    rc = 0;

done:
    counter_break();
    if (nstarted > 0) {   /* an early exit with workers running: let them end */
        pthread_mutex_lock(&g.m);
        g.closed = 1;
        pthread_cond_broadcast(&g.ready);
        pthread_mutex_unlock(&g.m);
        for (int i = 0; i < nstarted; i++) pthread_join(tids[i], NULL);
    }
    /* a directory this run created but never filled is not left behind */
    if (rc != 0 && created_dest && frames == 0) rmdir(dest);
    if (g.slot) { for (int i = 0; i < g.nslots; i++) free(g.slot[i].buf); }
    free(g.slot);
    free(tids);
    if (sync_ready) {
        pthread_mutex_destroy(&g.m);
        pthread_cond_destroy(&g.ready);
        pthread_cond_destroy(&g.freed);
    }
    if (in && in != stdin) fclose(in);
    return rc;
}

/* ---------------------------------------------------------------- */
/* Serve (-s): a clip directory as a Matroska stream with one       */
/* V_MJPEG video track                                              */
/*                                                                  */
/* The JPEG files are copied into SimpleBlocks untouched. Every     */
/* size in the file is known before the first byte goes out (from   */
/* the sizes of the files), so the Segment, the Clusters and the    */
/* Cues are all written with their real sizes and positions, and    */
/* the output can be piped or saved and then seeked in.            */
/* ---------------------------------------------------------------- */

typedef struct { unsigned char *p; size_t n, cap; int bad; } mb_t;

static void mb_put(mb_t *b, const void *d, size_t n) {
    if (b->bad) return;
    if (b->n + n > b->cap) {
        size_t nc = b->cap ? b->cap : 256;
        while (nc < b->n + n) nc *= 2;
        unsigned char *t = realloc(b->p, nc);
        if (!t) { b->bad = 1; return; }
        b->p = t;
        b->cap = nc;
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void mb_be(mb_t *b, uint64_t v, int len) {
    unsigned char t[8];
    for (int i = len - 1; i >= 0; i--) { t[i] = (unsigned char)v; v >>= 8; }
    mb_put(b, t, (size_t)len);
}
static void mb_id(mb_t *b, uint32_t id) {
    mb_be(b, id, id > 0xFFFFFFu ? 4 : id > 0xFFFFu ? 3 : id > 0xFFu ? 2 : 1);
}
static int vint_len(uint64_t v) { int l = 1; while (l < 8 && v >= (1ull << (7 * l)) - 1) l++; return l; }
static int uint_len(uint64_t v) { int l = 1; while (l < 8 && (v >> (8 * l)) != 0) l++; return l; }
static void mb_size(mb_t *b, uint64_t v, int len) { mb_be(b, v | (1ull << (7 * len)), len); }
static void el_uint(mb_t *b, uint32_t id, uint64_t v, int len) {
    if (!len) len = uint_len(v);
    mb_id(b, id);
    mb_size(b, (uint64_t)len, 1);
    mb_be(b, v, len);
}
static void el_f64(mb_t *b, uint32_t id, double d) {
    uint64_t u;
    memcpy(&u, &d, 8);
    mb_id(b, id);
    mb_size(b, 8, 1);
    mb_be(b, u, 8);
}
static void el_str(mb_t *b, uint32_t id, const char *s) {
    size_t n = strlen(s);
    mb_id(b, id);
    mb_size(b, n, vint_len(n));
    mb_put(b, s, n);
}
static void el_master(mb_t *b, uint32_t id, const mb_t *c, int sizelen) {
    mb_id(b, id);
    mb_size(b, c->n, sizelen ? sizelen : vint_len(c->n));
    mb_put(b, c->p, c->n);
    if (c->bad) b->bad = 1;
}
static void mb_free(mb_t *b) { free(b->p); memset(b, 0, sizeof(*b)); }

static void seek_entry(mb_t *sh, uint32_t id, uint64_t pos) {
    mb_t s = {0}, idb = {0};
    mb_id(&idb, id);
    mb_id(&s, 0x53AB);
    mb_size(&s, idb.n, 1);
    mb_put(&s, idb.p, idb.n);
    el_uint(&s, 0x53AC, pos, 8);
    el_master(sh, 0x4DBB, &s, 0);
    mb_free(&s);
    mb_free(&idb);
}

/* One frame of a clip: its picture file. */
typedef struct {
    char *path;
    uint64_t size;
} mj_frame_t;

static int has_jpg_ext(const char *name) {
    size_t l = strlen(name);
    return (l > 4 && !strcasecmp(name + l - 4, ".jpg")) || (l > 5 && !strcasecmp(name + l - 5, ".jpeg"));
}

/* frame-0000007-2.jpg -> 2 (a picture of a clip stored as fields by an earlier version); 0 if not such a name */
static int field_of(const char *path) {
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    size_t l = strlen(b);
    if (l < 7 || strcasecmp(b + l - 4, ".jpg") != 0) return 0;
    char f = b[l - 5], dash = b[l - 6], dig = b[l - 7];
    if ((f != '1' && f != '2') || dash != '-' || !isdigit((unsigned char)dig)) return 0;
    return f - '0';
}

/* The width and height in the first start-of-frame marker of a JPEG file. */
static int jpeg_file_dims(const char *path, int *w, int *h) {
    unsigned char b[1 << 16];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, b, sizeof(b));
    close(fd);
    if (n < 4 || b[0] != 0xFF || b[1] != 0xD8) return -1;
    size_t p = 2;
    while (p + 9 < (size_t)n) {
        if (b[p] != 0xFF) return -1;
        unsigned m = b[p + 1];
        if (m == 0xFF) { p++; continue; }
        size_t len = ((size_t)b[p + 2] << 8) | b[p + 3];
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            *h = (b[p + 5] << 8) | b[p + 6];
            *w = (b[p + 7] << 8) | b[p + 8];
            return 0;
        }
        p += 2 + len;
    }
    return -1;
}

/* ---- read-ahead: worker threads read the next frames while the writer writes ---- */

typedef struct { unsigned char *data; size_t len; int ready; } ra_slot_t;
typedef struct {
    mj_frame_t *fr;
    size_t n;
    ra_slot_t *slot;
    int nslots;
    pthread_mutex_t m;
    pthread_cond_t cv;
    size_t next, consumed;
    int failed;
    char err[700];
} ra_t;

static int read_whole(const char *path, unsigned char *dst, uint64_t want, char *err, size_t errn) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { snprintf(err, errn, "cannot open %s: %s", path, strerror(errno)); return -1; }
    struct stat st;
    if (fstat(fd, &st) != 0 || (uint64_t)st.st_size != want) {
        snprintf(err, errn, "%s changed size while serving (was %llu bytes, now %llu)", path,
                 (unsigned long long)want, (unsigned long long)(fstat(fd, &st) == 0 ? st.st_size : 0));
        close(fd);
        return -1;
    }
    uint64_t off = 0;
    while (off < want) {
        ssize_t r = read(fd, dst + off, (size_t)(want - off));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { snprintf(err, errn, "read error on %s: %s", path, r < 0 ? strerror(errno) : "short file"); close(fd); return -1; }
        off += (uint64_t)r;
    }
    close(fd);
    if (want < 4 || dst[0] != 0xFF || dst[1] != 0xD8) {
        snprintf(err, errn, "%s is not a JPEG file (it does not start with the JPEG start marker)", path);
        return -1;
    }
    return 0;
}

static void *ra_worker(void *arg) {
    ra_t *ra = arg;
    for (;;) {
        pthread_mutex_lock(&ra->m);
        while (!ra->failed && ra->next < ra->n && ra->next >= ra->consumed + (size_t)ra->nslots)
            pthread_cond_wait(&ra->cv, &ra->m);
        if (ra->failed || ra->next >= ra->n) { pthread_mutex_unlock(&ra->m); break; }
        size_t idx = ra->next++;
        pthread_mutex_unlock(&ra->m);

        const mj_frame_t *f = &ra->fr[idx];
        char err[600] = "";
        unsigned char *buf = malloc((size_t)f->size);
        int rc = buf ? 0 : -1;
        if (!buf) snprintf(err, sizeof(err), "out of memory");
        if (rc == 0) rc = read_whole(f->path, buf, f->size, err, sizeof(err));
        pthread_mutex_lock(&ra->m);
        if (rc != 0) {
            if (!ra->failed) { ra->failed = 1; snprintf(ra->err, sizeof(ra->err), "%s", err); }
            free(buf);
        } else {
            ra_slot_t *s = &ra->slot[idx % (size_t)ra->nslots];
            s->data = buf;
            s->len = (size_t)f->size;
            s->ready = 1;
        }
        pthread_cond_broadcast(&ra->cv);
        pthread_mutex_unlock(&ra->m);
    }
    return NULL;
}

/* Takes frame idx's bytes (malloc'd); NULL if the read-ahead failed. */
static unsigned char *ra_take(ra_t *ra, size_t idx) {
    pthread_mutex_lock(&ra->m);
    ra_slot_t *s = &ra->slot[idx % (size_t)ra->nslots];
    while (!s->ready && !ra->failed) pthread_cond_wait(&ra->cv, &ra->m);
    unsigned char *d = NULL;
    if (s->ready) { d = s->data; s->data = NULL; s->ready = 0; }
    pthread_mutex_unlock(&ra->m);
    return d;
}

static void ra_done_with(ra_t *ra) {
    pthread_mutex_lock(&ra->m);
    ra->consumed++;
    pthread_cond_broadcast(&ra->cv);
    pthread_mutex_unlock(&ra->m);
}

static int serve_mkv(const char *dir, const char *out_path, int quiet, int nthreads) {
    int rc = 1;
    mj_frame_t *fr = NULL;
    size_t nfr = 0;
    char **names = NULL;
    size_t nnames = 0;
    FILE *out = NULL;
    uint64_t *csz = NULL;
    ra_t ra;
    memset(&ra, 0, sizeof(ra));
    pthread_t *tids = NULL;
    int nstarted = 0;
    mb_t ebml = {0}, seg = {0}, seekh = {0}, info = {0}, tracks = {0}, cues = {0}, ch = {0};

    char sdir[PATH_MAX + 8];
    specs_locate(dir, sdir, sizeof(sdir), quiet);
    specs_t sp;
    if (specs_for_dir(sdir, &sp, quiet) != 0) return 1;

    /* 1. the pictures, in natural order, and their sizes */
    names = list_ext_files(dir, NULL, &nnames);
    if (!names || nnames == 0) { fprintf(stderr, "mjpegstore: no frame files found in %s\n", dir); goto done; }
    size_t njpg = 0, skipped = 0;
    for (size_t i = 0; i < nnames; i++) {
        if (has_jpg_ext(names[i])) names[njpg++] = names[i];
        else { skipped++; free(names[i]); }
    }
    if (skipped && !quiet) fprintf(stderr, "mjpegstore: warning: %zu file%s in %s without a .jpg extension %s ignored\n",
                                   skipped, skipped == 1 ? "" : "s", dir, skipped == 1 ? "is" : "are");
    nnames = njpg;
    if (nnames == 0) { fprintf(stderr, "mjpegstore: no .jpg files found in %s\n", dir); goto done; }
    fr = calloc(nnames, sizeof(*fr));
    if (!fr) { fprintf(stderr, "mjpegstore: out of memory\n"); goto done; }
    for (size_t i = 0; i < nnames; i++) {
        if (field_of(names[i])) {
            fprintf(stderr, "mjpegstore: %s is one field picture of a frame stored as fields (an earlier version did that).\n"
                            "         This version stores and serves whole progressive frames only. Nothing was served.\n", names[i]);
            goto done;
        }
        mj_frame_t *f = &fr[nfr];
        struct stat st;
        f->path = names[i];
        if (stat(f->path, &st) != 0) { fprintf(stderr, "mjpegstore: cannot stat %s: %s\n", f->path, strerror(errno)); goto done; }
        if (st.st_size < 4 || st.st_size > 200 * 1000 * 1000) {
            fprintf(stderr, "mjpegstore: %s is %lld bytes; that is not a usable JPEG picture\n", f->path, (long long)st.st_size);
            goto done;
        }
        f->size = (uint64_t)st.st_size;
        nfr++;
    }
    {   /* the pictures must be the size specs.txt says */
        int w = 0, h = 0;
        const int want_h = sp.height;
        if (jpeg_file_dims(fr[0].path, &w, &h) != 0) {
            fprintf(stderr, "mjpegstore: %s is not a readable JPEG\n", fr[0].path);
            goto done;
        }
        if (w != sp.width || h != want_h) {
            fprintf(stderr, "mjpegstore: %s is %dx%d, but specs.txt (%dx%d) needs %dx%d. Nothing was served.\n",
                    fr[0].path, w, h, sp.width, sp.height, sp.width, want_h);
            goto done;
        }
    }

    /* 2. the layout: every size is known now */
    const uint64_t num = (uint64_t)sp.fps_num, den = (uint64_t)sp.fps_den;
    size_t K = (size_t)((num + den / 2) / den);          /* frames per cluster: about a second */
    if (K < 1) K = 1;
    const size_t ncl = (nfr + K - 1) / K;
    csz = calloc(ncl, sizeof(*csz));
    if (!csz) { fprintf(stderr, "mjpegstore: out of memory\n"); goto done; }
    uint64_t clusters_total = 0;
    for (size_t c = 0; c < ncl; c++) {
        uint64_t s = 4 + 8 + 10;      /* id, size, Timestamp */
        for (size_t i = c * K; i < nfr && i < (c + 1) * K; i++) s += 9 + fr[i].size;
        csz[c] = s;
        clusters_total += s;
    }
#define TS_MS(i) (((uint64_t)(i) * 1000u * den + num / 2) / num)

    /* Info */
    el_uint(&ch, 0x2AD7B1, 1000000, 0);
    el_f64(&ch, 0x4489, (double)nfr * 1000.0 * (double)den / (double)num);
    el_str(&ch, 0x4D80, "mjpegstore");
    el_str(&ch, 0x5741, "mjpegstore");
    el_master(&info, 0x1549A966, &ch, 0);
    mb_free(&ch);

    /* Tracks */
    {
        mb_t te = {0}, vid = {0}, col = {0};
        el_uint(&te, 0xD7, 1, 0);
        el_uint(&te, 0x73C5, 1, 0);
        el_uint(&te, 0x83, 1, 0);
        el_uint(&te, 0x9C, 0, 0);
        el_str(&te, 0x86, "V_MJPEG");
        el_uint(&te, 0x23E383, (1000000000ull * den + num / 2) / num, 0);
        el_uint(&vid, 0x9A, 2, 0);                       /* FlagInterlaced: progressive */
        el_uint(&vid, 0xB0, (uint64_t)sp.width, 0);
        el_uint(&vid, 0xBA, (uint64_t)sp.height, 0);
        el_uint(&vid, 0x54B0, ((uint64_t)sp.width * (uint64_t)sp.asp_num + (uint64_t)sp.asp_den / 2) / (uint64_t)sp.asp_den, 0);
        el_uint(&vid, 0x54BA, (uint64_t)sp.height, 0);
        el_uint(&col, 0x55B9, sp.range_full ? 2 : 1, 0);
        if (is_420(&sp)) {
            el_uint(&col, 0x55B7, sp.chromasiting == CS_JPEG ? 2 : 1, 0);
            if (sp.chromasiting != CS_PALDV) el_uint(&col, 0x55B8, 2, 0);
        }
        el_master(&vid, 0x55B0, &col, 0);
        el_master(&te, 0xE0, &vid, 0);
        mb_t tr = {0};
        el_master(&tr, 0xAE, &te, 0);
        el_master(&tracks, 0x1654AE6B, &tr, 0);
        mb_free(&te); mb_free(&vid); mb_free(&col); mb_free(&tr);
    }

    /* SeekHead: built once with zero positions to learn its size */
    seek_entry(&seekh, 0x1549A966, 0);
    seek_entry(&seekh, 0x1654AE6B, 0);
    seek_entry(&seekh, 0x1C53BB6B, 0);
    mb_t sh = {0};
    el_master(&sh, 0x114D9B74, &seekh, 0);
    const uint64_t S = sh.n, I = info.n, T = tracks.n;
    mb_free(&sh);
    mb_free(&seekh);

    /* Cues: one per cluster */
    {
        uint64_t pos = S + I + T;
        for (size_t c = 0; c < ncl; c++) {
            mb_t pt = {0}, ctp = {0}, cp = {0};
            el_uint(&pt, 0xB3, TS_MS(c * K), 8);
            el_uint(&ctp, 0xF7, 1, 1);
            el_uint(&ctp, 0xF1, pos, 8);
            el_master(&pt, 0xB7, &ctp, 0);
            el_master(&cp, 0xBB, &pt, 0);
            mb_put(&ch, cp.p, cp.n);
            mb_free(&pt); mb_free(&ctp); mb_free(&cp);
            pos += csz[c];
        }
        el_master(&cues, 0x1C53BB6B, &ch, 0);
        mb_free(&ch);
    }
    {   /* the real SeekHead */
        seek_entry(&ch, 0x1549A966, S);
        seek_entry(&ch, 0x1654AE6B, S + I);
        seek_entry(&ch, 0x1C53BB6B, S + I + T + clusters_total);
        el_master(&seekh, 0x114D9B74, &ch, 0);
        mb_free(&ch);
    }
    if (seekh.n != S || ebml.bad || info.bad || tracks.bad || cues.bad || seekh.bad) {
        fprintf(stderr, "mjpegstore: internal error building the Matroska header\n");
        goto done;
    }
    {   /* EBML header, Segment header */
        mb_t e = {0};
        el_uint(&e, 0x4286, 1, 0);
        el_uint(&e, 0x42F7, 1, 0);
        el_uint(&e, 0x42F2, 4, 0);
        el_uint(&e, 0x42F3, 8, 0);
        el_str(&e, 0x4282, "matroska");
        el_uint(&e, 0x4287, 4, 0);
        el_uint(&e, 0x4285, 2, 0);
        el_master(&ebml, 0x1A45DFA3, &e, 0);
        mb_free(&e);
        mb_id(&seg, 0x18538067);
        mb_size(&seg, S + I + T + clusters_total + cues.n, 8);
    }

    /* 3. the output */
    out = stdout;
    if (out_path) {
        out = fopen(out_path, "wb");
        if (!out) { fprintf(stderr, "mjpegstore: cannot open %s for writing: %s\n", out_path, strerror(errno)); goto done; }
    }
    setvbuf(out, NULL, _IOFBF, SERVE_BUF);

    ra.fr = fr;
    ra.n = nfr;
    if (nthreads < 1) nthreads = 1;
    if (nthreads > 8) nthreads = 8;
    ra.nslots = 2 * nthreads + 2;
    ra.slot = calloc((size_t)ra.nslots, sizeof(*ra.slot));
    tids = calloc((size_t)nthreads, sizeof(*tids));
    if (!ra.slot || !tids) { fprintf(stderr, "mjpegstore: out of memory\n"); goto done; }
    pthread_mutex_init(&ra.m, NULL);
    pthread_cond_init(&ra.cv, NULL);
    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&tids[i], NULL, ra_worker, &ra) != 0) break;
        nstarted++;
    }
    if (nstarted == 0) { fprintf(stderr, "mjpegstore: cannot start a reader thread\n"); goto done; }

    if (fwrite(ebml.p, 1, ebml.n, out) != ebml.n || fwrite(seg.p, 1, seg.n, out) != seg.n ||
        fwrite(seekh.p, 1, seekh.n, out) != seekh.n || fwrite(info.p, 1, info.n, out) != info.n ||
        fwrite(tracks.p, 1, tracks.n, out) != tracks.n) { rc = serve_write_failed(); goto done; }

    for (size_t c = 0; c < ncl; c++) {
        mb_t hd = {0};
        mb_id(&hd, 0x1F43B675);
        mb_size(&hd, csz[c] - 12, 8);
        el_uint(&hd, 0xE7, TS_MS(c * K), 8);
        if (fwrite(hd.p, 1, hd.n, out) != hd.n) { mb_free(&hd); rc = serve_write_failed(); goto done; }
        mb_free(&hd);
        for (size_t i = c * K; i < nfr && i < (c + 1) * K; i++) {
            unsigned char *data = ra_take(&ra, i);
            if (!data) { fprintf(stderr, "mjpegstore: %s\n", ra.err); goto done; }
            unsigned char bh[9];
            bh[0] = 0xA3;
            uint32_t bs = (uint32_t)(4 + fr[i].size) | 0x10000000u;
            bh[1] = (unsigned char)(bs >> 24); bh[2] = (unsigned char)(bs >> 16);
            bh[3] = (unsigned char)(bs >> 8);  bh[4] = (unsigned char)bs;
            bh[5] = 0x81;
            int16_t rel = (int16_t)(TS_MS(i) - TS_MS(c * K));
            bh[6] = (unsigned char)((uint16_t)rel >> 8); bh[7] = (unsigned char)rel;
            bh[8] = 0x80;                                  /* every JPEG is a key frame */
            int bad = fwrite(bh, 1, 9, out) != 9 || fwrite(data, 1, (size_t)fr[i].size, out) != (size_t)fr[i].size;
            free(data);
            ra_done_with(&ra);
            if (bad) { rc = serve_write_failed(); goto done; }
        }
    }
    if (fwrite(cues.p, 1, cues.n, out) != cues.n || fflush(out) != 0) { rc = serve_write_failed(); goto done; }
    if (!quiet) fprintf(stderr, "mjpegstore: served %zu frame%s as Matroska (%.1f s at %ld:%ld fps)\n", nfr, nfr == 1 ? "" : "s",
                        (double)nfr * (double)den / (double)num, sp.fps_num, sp.fps_den);
    rc = 0;

done:
    if (nstarted > 0) {
        pthread_mutex_lock(&ra.m);
        ra.failed = 1;                 /* wake and stop any reader still waiting */
        pthread_cond_broadcast(&ra.cv);
        pthread_mutex_unlock(&ra.m);
        for (int i = 0; i < nstarted; i++) pthread_join(tids[i], NULL);
    }
    if (ra.slot) { for (int i = 0; i < ra.nslots; i++) free(ra.slot[i].data); }
    free(ra.slot);
    free(tids);
    if (nstarted > 0 || ra.slot) { pthread_mutex_destroy(&ra.m); pthread_cond_destroy(&ra.cv); }
    if (out && out != stdout) fclose(out);
    mb_free(&ebml); mb_free(&seg); mb_free(&seekh); mb_free(&info); mb_free(&tracks); mb_free(&cues); mb_free(&ch);
    free(csz);
    free(fr);
    if (names) { for (size_t i = 0; i < nnames; i++) free(names[i]); free(names); }
    return rc;
}

/* ---------------------------------------------------------------- */
/* Several directories, one list (e.g. multiple episodes)            */
/*                                                                   */
/*   mjpegstore DIR DIR ...                                          */
/*                                                                   */
/* For listing only. Every directory must be a clip (it holds frame   */
/* files directly). All the files are pooled and listed once, in      */
/* natural order, into one list that opens with each set's specs.txt. */
/* Directories are taken in the order given, so a glob (ep*) gives    */
/* episode order, and any run of them can be cut into disc-size sets. */
/* ---------------------------------------------------------------- */

typedef struct {
    const char *dir;
    dev_t dev;
    ino_t ino;
    char sdir[PATH_MAX + 8];   /* the directory whose specs.txt applies */
    specs_t specs;
    int have_specs;
    int specs_done;
    size_t nfiles;
} multi_dir_t;

/* Only what would make pooling clips of different formats worth a warning. */
static int geom_equal(const specs_t *a, const specs_t *b) {
    return a->width == b->width && a->height == b->height && a->sampling == b->sampling &&
           a->quality == b->quality;
}

static void geom_text(const specs_t *sp, char *buf, size_t n) {
    snprintf(buf, n, "%dx%d %s q%d", sp->width, sp->height, SAMPLINGS[sp->sampling].name, sp->quality);
}

/* Appends the k strings of src to *dst (taking ownership). 0 or -1. */
static int append_paths(char ***dst, size_t *n, size_t *cap, char **src, size_t k) {
    if (*n + k > *cap) {
        size_t nc = *cap ? *cap : 1024;
        while (nc < *n + k) nc *= 2;
        char **t = realloc(*dst, nc * sizeof(char *));
        if (!t) return -1;
        *dst = t;
        *cap = nc;
    }
    memcpy(*dst + *n, src, k * sizeof(char *));
    *n += k;
    return 0;
}

static int multi_sort(int ndirs, char **dirs, const elem_opts_t *op) {
    int rc = 1;
    const int quiet = op->quiet;
    char **all = NULL;
    size_t nall = 0, capall = 0;
    multi_dir_t *md = calloc((size_t)ndirs, sizeof(*md));
    if (!md) { fprintf(stderr, "mjpegstore: out of memory\n"); return 1; }

    /* 1. each must be an existing directory, and none may be given twice */
    for (int i = 0; i < ndirs; i++) {
        struct stat st;
        md[i].dir = dirs[i];
        if (stat(dirs[i], &st) != 0) { fprintf(stderr, "mjpegstore: cannot stat %s: %s\n", dirs[i], strerror(errno)); goto done; }
        if (!S_ISDIR(st.st_mode)) { fprintf(stderr, "mjpegstore: %s is not a directory\n", dirs[i]); goto done; }
        md[i].dev = st.st_dev;
        md[i].ino = st.st_ino;
        for (int j = 0; j < i; j++) {
            if (md[j].dev == md[i].dev && md[j].ino == md[i].ino) {
                fprintf(stderr, "mjpegstore: %s and %s are the same directory; give each directory once "
                                "(its files would be listed twice)\n", dirs[j], dirs[i]);
                goto done;
            }
        }
    }

    /* 2. each must be a clip */
    int bad = 0;
    for (int i = 0; i < ndirs; i++) if (count_stray_top_level_files(dirs[i]) == 0) bad = 1;
    if (bad) {
        fprintf(stderr, "mjpegstore: several directories can be listed together only if every one is a clip "
                        "(holds frame files directly):\n");
        for (int i = 0; i < ndirs; i++)
            fprintf(stderr, "  %-40s %s\n", dirs[i], count_stray_top_level_files(dirs[i]) ? "clip" : "not a clip (no frame files)");
        fprintf(stderr, "         Nothing was written.\n");
        goto done;
    }

    /* 3. specs, found the way a single directory's are (the parent's specs.txt).
     *    Directories in one set share theirs, so each file is read once. */
    for (int i = 0; i < ndirs; i++) {
        specs_locate(md[i].dir, md[i].sdir, sizeof(md[i].sdir), quiet);
        for (int j = 0; j < i; j++) {
            if (md[j].specs_done && !strcmp(md[j].sdir, md[i].sdir)) {
                md[i].specs = md[j].specs;
                md[i].have_specs = md[j].have_specs;
                md[i].specs_done = 1;
                break;
            }
        }
        if (md[i].specs_done) continue;
        if (specs_for_dir(md[i].sdir, &md[i].specs, quiet) != 0) goto done;
        md[i].have_specs = 1;
        md[i].specs_done = 1;
    }
    int first = -1;
    for (int i = 0; i < ndirs; i++) {
        if (!md[i].have_specs) continue;
        if (first < 0) { first = i; continue; }
        if (!geom_equal(&md[first].specs, &md[i].specs) && !quiet) {
            char a[64], b[64];
            geom_text(&md[first].specs, a, sizeof(a));
            geom_text(&md[i].specs, b, sizeof(b));
            fprintf(stderr, "mjpegstore: warning: %s is %s but %s is %s; they are listed together anyway\n",
                    md[first].dir, a, md[i].dir, b);
        }
    }

    /* 4. walk each directory, in the order given, pooling its files */
    for (int i = 0; i < ndirs; i++) {
        size_t c = 0;
        char **l = list_ext_files(md[i].dir, NULL, &c);
        if (!l || c == 0) { fprintf(stderr, "mjpegstore: no frame files found in %s\n", md[i].dir); free(l); goto done; }
        if (append_paths(&all, &nall, &capall, l, c) != 0) {
            for (size_t k = 0; k < c; k++) free(l[k]);
            free(l);
            fprintf(stderr, "mjpegstore: out of memory\n");
            goto done;
        }
        free(l);
        md[i].nfiles = c;
    }
    if (!quiet) {
        fprintf(stderr, "mjpegstore: %d directories -- JPEG frame storage; natural order list being generated; %zu files pooled into one list\n",
                ndirs, nall);
        for (int i = 0; i < ndirs; i++) fprintf(stderr, "  %-40s %zu files\n", md[i].dir, md[i].nfiles);
    }

    /* 5. the listing */
    {
        const char **mdirs = malloc((size_t)ndirs * sizeof(char *));
        size_t nsp = 0;
        char **sp = NULL;
        if (mdirs) {
            for (int i = 0; i < ndirs; i++) mdirs[i] = md[i].dir;
            sp = collect_specs(mdirs, ndirs, &nsp, quiet);
            free(mdirs);
        }
        char *mem = NULL;
        size_t memlen = 0;
        FILE *ms = open_memstream(&mem, &memlen);
        if (!ms) { fprintf(stderr, "mjpegstore: out of memory\n"); free_specs(sp, nsp); goto done; }
        for (size_t i = 0; i < nsp; i++) fprintf(ms, "%s\n", sp[i]);       /* specs.txt leads the list */
        int sr = scan_only(all, nall, ms);
        if (fclose(ms) != 0) sr = 1;
        rc = sr == 0 ? emit_list(mem, memlen, op->out_path, nall + nsp, quiet) : 1;
        free(mem);
        free_specs(sp, nsp);
    }

done:
    for (size_t i = 0; i < nall; i++) free(all[i]);
    free(all);
    free(md);
    return rc;
}

/* ---------------------------------------------------------------- */
/* main                                                              */
/* ---------------------------------------------------------------- */

static void usage(FILE *fp, const char *prog) {
    fprintf(fp,
        "Usage: %s -i FILE|- [-j QUALITY] <clipdir>   (encode a y4m stream into JPEG files)\n"
        "       %s -s [-f FILE] <clipdir>             (serve a clip as a Matroska MJPEG stream)\n"
        "       %s -x FILE.mkv <clipdir>              (export a clip to a Matroska file)\n"
        "       %s [-f FILE] <dir> [<dir> ...]        (list the files, in natural order)\n"
        "       find /some/dir -type f | %s [options] -\n"
        "       %s -c [-E] <setdir>                   (check / create / edit specs.txt)\n\n"
        "Stores video as JPEG pictures and serves it back as MJPEG in Matroska. Every\n"
        "frame is a baseline JPEG made from the y4m planes as they are (no colour\n"
        "conversion), with the same quantization and standard Huffman tables in every\n"
        "frame and a restart marker after every block of 16x16 pixels (-R). A piece of the\n"
        "picture that does not change from one frame to the next then comes out as the same bytes,\n"
        "aligned the same way, so a compressor that looks back over the set (zstd, xz)\n"
        "can match it. JPEG is lossy: the quality is in specs.txt.\n\n"
        "<dir> holds specs.txt (resolution, sampling, chromasiting, framerate, bitdepth, aspect,\n"
        "quality, range) and one subdirectory per clip; if specs.txt is missing you are asked\n"
        "for it. A clip holds its pictures in bucket folders of 5000 frames:\n"
        "b00000/frame-0000001.jpg, ... Only progressive footage is stored: an interlaced y4m\n"
        "stream is refused, so deinterlace it first (to 60p, say).\n\n"
        "Several directories (say, several episodes) are listed together into one list.\n"
        "A <dir> that holds only subdirectories (a set with more than one clip in it) is\n"
        "refused rather than blindly recursed into every clip as one undifferentiated\n"
        "pile: name or glob the clips explicitly instead.\n\n"
        "The list is always in natural filename order (numbers compare as numbers), with\n"
        "no hashing and no reading of the files beyond the directory scan. It begins with\n"
        "the set's specs.txt, so an archive built from the list carries it: -s needs one.\n"
        "A warning is printed if there is none.\n\n"
        "-f never appends: if the file already exists, you're asked whether to\n"
        "overwrite it (refused outright if stdin isn't a terminal, rather than guessing).\n\n"
        "Options:\n"
        "  -i SRC ingest: read a yuv4mpeg (y4m) stream from SRC (a file, or - for\n"
        "         stdin) and write each frame as a JPEG file into the new or empty clip\n"
        "         directory <clipdir>. specs.txt in its parent is created from the stream\n"
        "         header if absent; if present it must match, otherwise nothing is\n"
        "         written. 8-bit streams only. The frames are encoded on -t threads.\n"
        "  -R N   with -i: a restart marker after every N MCUs (an MCU is 16x16 pixels for\n"
        "         4:2:0, 8x8 for 4:4:4). Default 1, the smallest pieces, which gave the smallest\n"
        "         archives; 0 means one marker per row of blocks (a 720-pixel row is 45 MCUs).\n"
        "         Use the same -R for every clip of a set; it is not kept in specs.txt.\n"
        "  -y     with -i making a new set: use the settings detected from the stream header\n"
        "         without showing them or asking (the question is read from the terminal, not\n"
        "         from stdin, so redirecting stdin does not skip it)\n"
        "  -j N   JPEG quality, 1..100, for a specs.txt that -i creates (default 90; 100 is\n"
        "         the finest a baseline JPEG can be). Ignored when specs.txt exists: its\n"
        "         quality line is used, so every frame in a set matches.\n"
        "  -s     serve: write <clipdir> as a Matroska (.mkv) stream with one V_MJPEG\n"
        "         track on stdout, or to -f FILE, using specs.txt from its parent\n"
        "         directory. The pictures are copied in untouched, in filename order.\n"
        "         Sizes and seek positions are exact, so a saved file seeks normally.\n"
        "  -x FILE export: write <clipdir> to FILE as a seekable Matroska (.mkv) file, the same\n"
        "         as -s -f FILE. Asks before overwriting an existing file.\n"
        "  -c     check: look for specs.txt in <setdir>. If it is missing, ask for the\n"
        "         values and create it; if present, list the specs.\n"
        "         Nothing is scanned, listed or served.\n"
        "  -E     with -c: edit specs.txt. Each value is shown with its valid choices\n"
        "         (pick a number or type a value; Enter keeps it), the changes are\n"
        "         listed, and nothing is written until you confirm. Only the changed\n"
        "         lines are touched. Resolution and sampling describe the stored\n"
        "         files and need an extra yes. Needs a\n"
        "         terminal.\n"
        "  -t N   worker threads for -i and -s. Default = number of CPUs\n"
        "  -f PATH write the file list (or, with -s, the Matroska stream) to PATH\n"
        "         instead of stdout\n"
        "  -p     with -i reading stdin (-): show a running count of frames read, so a\n"
        "         slow upstream doesn't look like a hang (a file source already has a\n"
        "         progress bar). -q wins.\n"
        "  -q     quiet (suppress progress messages on stderr)\n"
        "  -h     show this help\n",
        prog, prog, prog, prog, prog, prog);
}

int main(int argc, char **argv) {
    int nthreads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (nthreads < 1) nthreads = 1;
    int quiet = 0;
    int serve = 0;
    int check = 0;
    int edit = 0;
    int quality = 90;
    int have_j = 0;
    int have_R = 0;
    const char *ingest_src = NULL;
    const char *out_path = NULL;
    const char *export_path = NULL;
    int opt;

    while ((opt = getopt(argc, argv, "t:f:i:j:R:x:cEspqhy")) != -1) {
        switch (opt) {
            case 'E': edit = 1; check = 1; break;
            case 't': nthreads = atoi(optarg); break;
            case 'f': out_path = optarg; break;
            case 's': serve = 1; break;
            case 'x': export_path = optarg; break;
            case 'c': check = 1; break;
            case 'i': ingest_src = optarg; break;
            case 'j':
                if (parse_quality(optarg, &quality) != 0) {
                    fprintf(stderr, "mjpegstore: -j takes a JPEG quality from 1 to 100, not '%s'\n", optarg);
                    return 1;
                }
                have_j = 1;
                break;
            case 'R': {
                char *end;
                long rv = strtol(optarg, &end, 10);
                if (*optarg == 0 || *end || rv < 0 || rv > 65535) {
                    fprintf(stderr, "mjpegstore: -R takes a number of MCUs from 1 to 65535 (0 = one per row), not '%s'\n", optarg);
                    return 1;
                }
                g_restart_mcus = (unsigned)rv;
                have_R = 1;
                break;
            }
            case 'y': g_assume_yes = 1; break;
            case 'p': g_progress_count = 1; break;
            case 'q': quiet = 1; break;
            case 'h': usage(stdout, argv[0]); return 0;   /* asked for: stdout, so it can be piped */
            default: usage(stderr, argv[0]); return 1;
        }
    }
    if (optind >= argc) {
        usage(stderr, argv[0]);
        return 1;
    }
    if (export_path) {
        if (out_path || serve) {
            fprintf(stderr, "mjpegstore: -x FILE already means serve to FILE; do not combine it with -s or -f\n");
            return 1;
        }
        if (check || ingest_src) {
            fprintf(stderr, "mjpegstore: -x cannot be combined with -i, -c or -E\n");
            return 1;
        }
        serve = 1;
        out_path = export_path;
        size_t xl = strlen(export_path);
        if (!quiet && (xl < 4 || strcasecmp(export_path + xl - 4, ".mkv") != 0))
            fprintf(stderr, "mjpegstore: warning: %s does not end in .mkv\n", export_path);
    }
    if (have_j && !ingest_src) {
        fprintf(stderr, "mjpegstore: -j applies only to -i (the quality of a set lives in its specs.txt; "
                        "change it with -c -E)\n");
        return 1;
    }

    if (have_R && !ingest_src) {
        fprintf(stderr, "mjpegstore: -R applies only to -i\n");
        return 1;
    }

    /* -f is never appended to: if it already exists, ask before truncating
     * it (or refuse outright if not on a terminal). */
    if (out_path && !confirm_overwrite(out_path, quiet)) return 1;

    /* Several directories are for listing only. The other modes take exactly
     * one, and must say so rather than quietly ignore the rest. */
    const int ndirs = argc - optind;
    if (ndirs > 1) {
        if (ingest_src || serve || check) {
            fprintf(stderr, "mjpegstore: %s takes exactly one directory; several directories are for listing only "
                            "(mjpegstore DIR DIR ...)\n", ingest_src ? "-i" : serve ? "-s" : "-c");
            return 1;
        }
        for (int i = optind; i < argc; i++) {
            if (!strcmp(argv[i], "-")) {
                fprintf(stderr, "mjpegstore: a file list on stdin (-) can't be combined with directories\n");
                return 1;
            }
        }
    }

    if (ingest_src && (serve || check)) {
        fprintf(stderr, "mjpegstore: -i cannot be combined with -s, -c or -E\n");
        return 1;
    }
    if (ingest_src) {
        fprintf(stderr, "mjpegstore: ingest: y4m from %s into %s\n",
                !strcmp(ingest_src, "-") ? "stdin" : ingest_src, argv[optind]);
        return ingest_jpeg(ingest_src, argv[optind], quiet, nthreads, quality);
    }
    if (check) {
        if (serve) {
            fprintf(stderr, "mjpegstore: -c and -s cannot be combined\n");
            return 1;
        }
        if (!strcmp(argv[optind], "-")) {
            fprintf(stderr, "mjpegstore: -c needs a set directory, not a file list on stdin\n");
            return 1;
        }
        struct stat cst;
        if (stat(argv[optind], &cst) != 0) {
            fprintf(stderr, "mjpegstore: cannot stat %s: %s\n", argv[optind], strerror(errno));
            return 1;
        }
        if (!S_ISDIR(cst.st_mode)) {
            fprintf(stderr, "mjpegstore: -c needs a set directory; %s is not a directory\n", argv[optind]);
            return 1;
        }
        return edit ? edit_specs(argv[optind], quiet) : check_specs(argv[optind], quiet);
    }

    if (serve) {
        if (!strcmp(argv[optind], "-")) {
            fprintf(stderr, "mjpegstore: -s needs a clip directory, not a file list on stdin\n");
            return 1;
        }
        struct stat sst;
        if (stat(argv[optind], &sst) != 0) {
            fprintf(stderr, "mjpegstore: cannot stat %s: %s\n", argv[optind], strerror(errno));
            return 1;
        }
        if (!S_ISDIR(sst.st_mode)) {
            fprintf(stderr, "mjpegstore: -s needs a clip directory; %s is not a directory\n", argv[optind]);
            return 1;
        }
        if (!out_path && isatty(STDOUT_FILENO)) {
            fprintf(stderr, "mjpegstore: refusing to write a video stream to a terminal; pipe it "
                            "(mjpegstore -s %s | mpv -) or use -f FILE.mkv\n", argv[optind]);
            return 1;
        }
        fprintf(stderr, "mjpegstore: serve: Matroska MJPEG stream from %s | outputting to %s\n",
                argv[optind], out_path ? out_path : "stdout");
        return serve_mkv(argv[optind], out_path, quiet, nthreads);
    }

    /* Several directories (say, several episodes): pooled into one list */
    if (ndirs > 1) {
        elem_opts_t mo = { nthreads, quiet, out_path, NULL, 0 };
        return multi_sort(ndirs, &argv[optind], &mo);
    }

    /* Not a clip itself: if it holds only subdirectories (no loose frame
     * files directly inside), it's a set directory with more than one clip
     * in it. Pointing a sort at it directly would otherwise recurse blindly
     * through every clip's files as one undifferentiated pile. Refuse rather
     * than silently doing that. */
    if (strcmp(argv[optind], "-") != 0 && dir_holds_only_subdirs(argv[optind])) {
        fprintf(stderr, "mjpegstore: multiple clips detected. Please list the clips to be ingested by name or glob\n");
        return 1;
    }

    fprintf(stderr, "mjpegstore: %s -- JPEG frame storage; natural order list being generated | outputting to %s\n",
            argv[optind], out_path ? out_path : "stdout");

    FILE *out = stdout;
    if (out_path) {
        out = fopen(out_path, "w");
        if (!out) {
            fprintf(stderr, "mjpegstore: cannot open %s for writing: %s\n",
                    out_path, strerror(errno));
            return 1;
        }
    }

    const char *src = argv[optind];
    pathlist_t pl;
    pathlist_init(&pl);
    char lead_specs[PATH_MAX + 32];      /* specs.txt, written first when there is one */
    int have_lead = 0;

    if (!strcmp(src, "-")) {
        read_stdin_list(&pl);
    } else {
        struct stat st;
        if (stat(src, &st) != 0) {
            fprintf(stderr, "mjpegstore: cannot stat %s: %s\n", src, strerror(errno));
            return 1;
        }
        if (S_ISDIR(st.st_mode)) {
            /* The listing needs no specs, so it only reads one that is
             * already there and never prompts. */
            char sdir[PATH_MAX + 8];
            specs_locate(src, sdir, sizeof(sdir), quiet);
            char spath[PATH_MAX + 32];
            snprintf(spath, sizeof(spath), "%s/%s", sdir, SPECS_FILENAME);
            if (path_exists(spath)) {
                specs_t specs;
                if (specs_for_dir(sdir, &specs, quiet) != 0) return 1;
            }
            have_lead = specs_list_path(src, lead_specs, sizeof(lead_specs));
            if (!have_lead && !quiet) warn_no_specs(src);
            scan_dir(src, &pl, quiet, 1);
        } else {
            pathlist_push(&pl, src);
        }
    }

    if (pl.count == 0) {
        if (!quiet) fprintf(stderr, "mjpegstore: no files found\n");
        return 0;
    }

    if (have_lead) fprintf(out, "%s\n", lead_specs);      /* specs.txt leads the list */
    int nrc = scan_only(pl.paths, pl.count, out);
    if (nrc == 0 && out_path) {
        fclose(out);
        if (!quiet) fprintf(stderr, "mjpegstore: wrote %zu file path(s) to %s\n", pl.count + (size_t)have_lead, out_path);
    }
    for (size_t i = 0; i < pl.count; i++) free(pl.paths[i]);
    free(pl.paths);
    return nrc;
}
