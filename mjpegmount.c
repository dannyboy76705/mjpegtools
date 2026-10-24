/*
 * mjpegmount - present clips stored by mjpegstore as seekable .mkv files.
 *
 * Copyright (c) 2026 Daniel Lee Witzel
 * MIT License - see LICENSE.
 *
 * Mounts a set directory (specs.txt plus one subdirectory per clip, each clip a
 * directory of JPEG pictures as written by mjpegstore -i) read-only. Every clip
 * appears as <clip>.mkv: the exact byte stream `mjpegstore -s` would write, one
 * V_MJPEG track in Matroska, but as a real file, so a player, encoder or editor
 * can open it and seek without anything being exported or copied first.
 *
 * Why this works: the JPEG files go into the Matroska blocks untouched, and every
 * size in the file (the Segment, the Clusters, the Cues, each block) follows from
 * the sizes of the picture files. When a clip is first touched its files are listed
 * and their sizes added up, which fixes every offset in the virtual file. A
 * read(offset, length) then maps onto the few header bytes in front of a block and
 * the picture file(s) behind it; nothing is decoded, re-encoded or scanned.
 *
 * Progressive footage only: one picture file per frame, one picture per Matroska block.
 * Interlaced sets (fieldorder tff/bff, or NAME-1.jpg / NAME-2.jpg field pictures) are
 * refused; deinterlace first and store the progressive frames.
 *
 * A valid specs.txt is required (frame rate, size, range for the
 * Matroska header). It is read from the set directory only; a mount cannot generate
 * one. It never edits specs.txt. The picture files can live anywhere: plain disk, or
 * another mount (zpaqfuse, DwarFS). Only the pictures actually read get touched.
 *
 * Build (Debian: apt install libfuse3-dev pkg-config):
 *   gcc -O2 -Wall -Wextra -pthread -o mjpegmount mjpegmount.c $(pkg-config fuse3 --cflags --libs)
 *
 * Usage:
 *   mjpegmount [fuse options] SETDIR MOUNTPOINT
 *   mjpegmount -f SETDIR MOUNTPOINT        # foreground, errors on the terminal
 *   fusermount3 -u MOUNTPOINT              # unmount
 */

#define _GNU_SOURCE
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef MJPEGMOUNT_AS_LIBRARY
#define FUSE_USE_VERSION 31
#include <fuse.h>
#endif

#define SPECS_FILENAME "specs.txt"
#define NAME "mjpegmount"
#ifndef APPNAME
#define APPNAME "mjpegmount"      /* the MuxingApp / WritingApp strings in the Matroska file */
#endif

/* ---------------------------------------------------------------- */
/* specs.txt: the same rules mjpegstore applies                       */
/* ---------------------------------------------------------------- */

/* ---------------------------------------------------------------- */
/* Where the files are: a source (plain disk by default)             */
/* ---------------------------------------------------------------- */

/* Everything this program asks of the place the set lives in. The default is the disk; a program that
 * includes this file as a library (MJPEGMOUNT_AS_LIBRARY, see mjpegmount.h) supplies its own, which is how
 * zfrag mount serves a set from inside an archive. Paths are "dir/name", as the source understands them. */
#ifndef MJ_SOURCE_DEFINED
#define MJ_SOURCE_DEFINED
typedef struct mj_source {
    /* calls cb(arg, name, is_dir, size) for each regular file and directory in dir (size: files only);
     * a nonzero return from cb stops it. Returns 0, or -errno. */
    int (*list)(void *ctx, const char *dir, int (*cb)(void *arg, const char *name, int is_dir, uint64_t size), void *arg);
    /* kind of path: 1 = file, 2 = directory; size and mtime filled in. 0, or -errno. */
    int (*stat)(void *ctx, const char *path, int *kind, uint64_t *size, int64_t *mtime);
    /* exactly n bytes at off, or -errno (-EIO if the file is shorter) */
    int (*pread)(void *ctx, const char *path, void *buf, size_t n, uint64_t off);
    void *ctx;
} mj_source;
#endif

static int posix_list(void *ctx, const char *dir, int (*cb)(void *, const char *, int, uint64_t), void *arg) {
    (void)ctx;
    DIR *d = opendir(dir);
    if (!d) return -errno;
    struct dirent *ent;
    int rc = 0;
    while (!rc && (ent = readdir(d)) != NULL) {
        char full[PATH_MAX + 300];
        struct stat st;
        snprintf(full, sizeof(full), "%.*s/%s", PATH_MAX - 1, dir, ent->d_name);
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) rc = cb(arg, ent->d_name, 1, 0);
        else if (S_ISREG(st.st_mode)) rc = cb(arg, ent->d_name, 0, (uint64_t)st.st_size);
    }
    closedir(d);
    return 0;
}

static int posix_stat(void *ctx, const char *path, int *kind, uint64_t *size, int64_t *mtime) {
    (void)ctx;
    struct stat st;
    if (stat(path, &st) != 0) return -errno;
    *kind = S_ISDIR(st.st_mode) ? 2 : S_ISREG(st.st_mode) ? 1 : 0;
    *size = (uint64_t)st.st_size;
    *mtime = (int64_t)st.st_mtime;
    return 0;
}

static int posix_pread(void *ctx, const char *path, void *buf, size_t n, uint64_t off) {
    (void)ctx;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -errno;
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(fd, (char *)buf + got, n - got, (off_t)(off + got));
        if (r < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            close(fd);
            return -e;
        }
        if (r == 0) { close(fd); return -EIO; }   /* shorter than when indexed */
        got += (size_t)r;
    }
    close(fd);
    return 0;
}

static const mj_source g_posix = { posix_list, posix_stat, posix_pread, NULL };
static const mj_source *g_src = &g_posix;

typedef struct {
    const char *name;    /* value used in specs.txt */
    int hsub, vsub;      /* chroma subsampling divisors */
    const char *y4m8;    /* y4m C tag at 8 bits */
    const char *y4m10;   /* y4m C tag at 10 bits */
} sampling_t;

static const sampling_t SAMPLINGS[] = {
    {"420", 2, 2, "420jpeg", "420p10"},
    {"422", 2, 1, "422",     "422p10"},
    {"444", 1, 1, "444",     "444p10"},
};
#define N_SAMPLINGS (sizeof(SAMPLINGS) / sizeof(SAMPLINGS[0]))

/* Chroma siting (4:2:0 only), as mjpegstore: the 8-bit C tag names it; 10-bit has no siting tag. */
enum { CS_JPEG = 0, CS_MPEG2 = 1, CS_PALDV = 2 };
static const struct { const char *name; const char *y4m; } SITINGS[] = {
    {"jpeg", "420jpeg"}, {"mpeg2", "420mpeg2"}, {"paldv", "420paldv"},
};

typedef struct {
    int width, height;
    int sampling;            /* index into SAMPLINGS */
    long fps_num, fps_den;
    int bitdepth;            /* 8, or 10 (16-bit little-endian words) */
    int chromasiting;        /* CS_JPEG, CS_MPEG2 or CS_PALDV (sampling 420 only) */
    int have_chromasiting;   /* specs.txt had a chromasiting line */
    long asp_num, asp_den;   /* pixel aspect ratio, reduced (A tag) */
    int quality, have_quality;   /* JPEG quality the set was stored at (informational here) */
    int range_full, have_range;  /* 0 = limited (16-235), 1 = full: the Matroska Colour Range */
    uint64_t frame_bytes;
} specs_t;

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

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

static int parse_sampling(const char *s) {
    for (size_t i = 0; i < N_SAMPLINGS; i++)
        if (!strcmp(s, SAMPLINGS[i].name)) return (int)i;
    return -1;
}

static long gcd_l(long a, long b) {
    while (b) { long t = a % b; a = b; b = t; }
    return a;
}

/* "24000:1001" ("/" accepted too); stored reduced. */
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

static int parse_bitdepth(const char *s, int *out) {
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s) return -1;
    while (isspace((unsigned char)*end)) end++;
    if (*end != 0) return -1;
    if (v == 8 || v == 10) { *out = (int)v; return 0; }
    return -1;
}

/* Pixel aspect ratio N:M (or N/M), reduced: the same rules mjpegstore applies. */
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

/* progressive (also prog, p) -> 0; tff / bff (also top, t, bottom, b) -> 1 (interlaced, refused); else -1. */
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

static void plane_sizes(const specs_t *sp, uint64_t psz[3]) {
    const sampling_t *si = &SAMPLINGS[sp->sampling];
    const uint64_t bps = sp->bitdepth > 8 ? 2 : 1;
    psz[0] = (uint64_t)sp->width * (uint64_t)sp->height * bps;
    psz[1] = psz[2] = (((uint64_t)sp->width + (uint64_t)si->hsub - 1) / (uint64_t)si->hsub) *
                      (((uint64_t)sp->height + (uint64_t)si->vsub - 1) / (uint64_t)si->vsub) * bps;
}

static uint64_t specs_frame_bytes(const specs_t *sp) {
    uint64_t p[3];
    plane_sizes(sp, p);
    return p[0] + p[1] + p[2];
}

static int is_420(const specs_t *sp) {
    return SAMPLINGS[sp->sampling].hsub == 2 && SAMPLINGS[sp->sampling].vsub == 2;
}


static int parse_quality(const char *s, int *out) {
    char *e;
    long v = strtol(s, &e, 10);
    if (e == s || *e != 0 || v < 1 || v > 100) return -1;
    *out = (int)v;
    return 0;
}

static int parse_range(const char *s, int *full) {
    if (!strcasecmp(s, "limited") || !strcasecmp(s, "tv") || !strcasecmp(s, "broadcast") || !strcasecmp(s, "mpeg")) { *full = 0; return 0; }
    if (!strcasecmp(s, "full") || !strcasecmp(s, "pc") || !strcasecmp(s, "jpeg")) { *full = 1; return 0; }
    return -1;
}


/* 0 = loaded, -1 = missing or invalid (reason in err). Never prompts and
 * never writes: this is a read-only mount. */
static int specs_load(const char *path, specs_t *sp, char *err, size_t errlen) {
    int kind = 0, e;
    uint64_t sz = 0;
    int64_t mt = 0;
    if ((e = g_src->stat(g_src->ctx, path, &kind, &sz, &mt)) != 0 || kind != 1 || sz > (1u << 20)) {
        snprintf(err, errlen, "cannot open %s: %s", path, strerror(e ? -e : EINVAL));
        return -1;
    }
    char *text = malloc((size_t)sz + 1);
    if (!text) { snprintf(err, errlen, "out of memory"); return -1; }
    if (sz && (e = g_src->pread(g_src->ctx, path, text, (size_t)sz, 0)) != 0) {
        snprintf(err, errlen, "cannot read %s: %s", path, strerror(-e));
        free(text);
        return -1;
    }
    FILE *f = fmemopen(text, sz ? (size_t)sz : 1, "r");        /* the text is parsed line by line as a file would be */
    if (!f) { snprintf(err, errlen, "cannot read %s", path); free(text); return -1; }
    int have_res = 0, have_smp = 0, have_fps = 0, have_bd = 0, have_asp = 0, have_fo = 0;
    sp->asp_num = sp->asp_den = 1;              /* what a specs.txt without these lines has always meant */
    sp->chromasiting = CS_JPEG;             /* what has always been served: C420jpeg */
    sp->have_chromasiting = 0;
    sp->quality = 95; sp->have_quality = 0;       /* a specs.txt without these lines: 95, limited range */
    sp->range_full = 0; sp->have_range = 0;
    char line[256];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *t = trim(line);
        if (*t == 0 || *t == '#') continue;
        char *eq = strchr(t, '=');
        if (!eq) { snprintf(err, errlen, "%s line %d: expected key=value", path, lineno); fclose(f); free(text); return -1; }
        *eq = 0;
        char *key = trim(t);
        char *val = trim(eq + 1);
        for (char *c = key; *c; c++) *c = (char)tolower((unsigned char)*c);

        if (!strcmp(key, "resolution")) {
            if (have_res || parse_resolution(val, &sp->width, &sp->height) != 0) {
                snprintf(err, errlen, "%s line %d: bad or duplicate resolution '%s'", path, lineno, val);
                fclose(f); free(text); return -1;
            }
            have_res = 1;
        } else if (!strcmp(key, "sampling")) {
            sp->sampling = parse_sampling(val);
            if (have_smp || sp->sampling < 0) {
                snprintf(err, errlen, "%s line %d: bad or duplicate sampling '%s' (valid: 420 422 444)", path, lineno, val);
                fclose(f); free(text); return -1;
            }
            have_smp = 1;
        } else if (!strcmp(key, "framerate")) {
            if (have_fps || parse_fps(val, &sp->fps_num, &sp->fps_den) != 0) {
                snprintf(err, errlen, "%s line %d: bad or duplicate framerate '%s' (fraction N:D)", path, lineno, val);
                fclose(f); free(text); return -1;
            }
            have_fps = 1;
        } else if (!strcmp(key, "bitdepth")) {
            if (have_bd || parse_bitdepth(val, &sp->bitdepth) != 0) {
                snprintf(err, errlen, "%s line %d: bad or duplicate bitdepth '%s' (valid: 8 10)", path, lineno, val);
                fclose(f); free(text); return -1;
            }
            have_bd = 1;
        } else if (!strcmp(key, "aspect")) {
            if (have_asp || parse_aspect(val, &sp->asp_num, &sp->asp_den) != 0) {
                snprintf(err, errlen, "%s line %d: bad or duplicate aspect '%s' (pixel aspect ratio N:M, e.g. 1:1 or 10:11)", path, lineno, val);
                fclose(f); free(text); return -1;
            }
            have_asp = 1;
        } else if (!strcmp(key, "quality")) {
            if (sp->have_quality || parse_quality(val, &sp->quality) != 0) {
                snprintf(err, errlen, "%s line %d: bad or duplicate quality '%s' (1..100)", path, lineno, val);
                fclose(f); free(text); return -1;
            }
            sp->have_quality = 1;
        } else if (!strcmp(key, "range")) {
            if (sp->have_range || parse_range(val, &sp->range_full) != 0) {
                snprintf(err, errlen, "%s line %d: bad or duplicate range '%s' (limited or full)", path, lineno, val);
                fclose(f); free(text); return -1;
            }
            sp->have_range = 1;
        } else if (!strcmp(key, "fieldorder") || !strcmp(key, "field_order") ||
                   !strcmp(key, "interlace") || !strcmp(key, "interlacing")) {
            int fo = have_fo ? -1 : parse_fieldorder(val);
            if (fo < 0) {
                snprintf(err, errlen, "%s line %d: bad or duplicate fieldorder '%s' (only progressive is supported)", path, lineno, val);
                fclose(f); free(text); return -1;
            }
            if (fo == 1) {
                snprintf(err, errlen, "%s line %d: fieldorder=%s means interlaced footage. " NAME " serves progressive sets only; "
                         "deinterlace the footage first and store progressive frames", path, lineno, val);
                fclose(f); free(text); return -1;
            }
            have_fo = 1;
        } else if (!strcmp(key, "chromasiting") || !strcmp(key, "chroma_siting") ||
                   !strcmp(key, "chromalocation") || !strcmp(key, "chroma_location")) {
            int cs = sp->have_chromasiting ? -1 : parse_siting(val);
            if (cs < 0) {
                snprintf(err, errlen, "%s line %d: bad or duplicate chromasiting '%s' (valid: jpeg, mpeg2, paldv)", path, lineno, val);
                fclose(f); free(text); return -1;
            }
            sp->chromasiting = cs;
            sp->have_chromasiting = 1;
        } else {
            fprintf(stderr, NAME ": %s: line %d: ignoring unknown key '%s'\n", path, lineno, key);
        }
    }
    fclose(f); free(text);
    if (!have_res || !have_smp || !have_fps || !have_bd) {
        snprintf(err, errlen, "%s: missing %s", path,
                 !have_res ? "resolution" : !have_smp ? "sampling" : !have_fps ? "framerate" : "bitdepth");
        return -1;
    }
    (void)have_fo;
    if (sp->bitdepth != 8) {
        snprintf(err, errlen, "%s: bitdepth=%d, but a JPEG set holds 8-bit pictures only", path, sp->bitdepth);
        return -1;
    }
    if (sp->have_chromasiting && !is_420(sp)) {
        snprintf(err, errlen, "%s: chromasiting applies only to sampling=420 (this specs.txt says %s): remove the line",
                 path, SAMPLINGS[sp->sampling].name);
        return -1;
    }
    sp->frame_bytes = specs_frame_bytes(sp);
    return 0;
}

/* ---------------------------------------------------------------- */
/* Filenames: natural order, and what makes a directory a clip       */
/* ---------------------------------------------------------------- */

/* Filename order where digit runs compare as numbers: f2 < f10. */
static int natural_cmp(const char *a, const char *b) {
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            const char *ra = a, *rb = b;
            while (isdigit((unsigned char)*ra)) ra++;
            while (isdigit((unsigned char)*rb)) rb++;
            const char *za = a, *zb = b;
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

static int name_cmp(const char *a, const char *b) {
    int c = natural_cmp(a, b);
    return c ? c : strcmp(a, b);
}

static int path_exists(const char *p) {
    int kind;
    uint64_t sz;
    int64_t mt;
    return g_src->stat(g_src->ctx, p, &kind, &sz, &mt) == 0;
}

/* A clip's pictures live in bucket folders b00000, b00001, ... (5000 files each). */
static int is_bucket_name(const char *name) {
    if (name[0] != 'b' || !name[1]) return 0;
    for (const char *p = name + 1; *p; p++) if (*p < '0' || *p > '9') return 0;
    return 1;
}

/* Any .jpg or .jpeg file directly in dir, or in one of its bucket folders? (Other loose files in a set
 * directory, such as audio or notes, do not make it a clip.) */
static int dir_has_frame_files(const char *dir);
typedef struct { const char *dir; int top, found; } hff_t;
static int dir_has_frame_files_in(const char *dir, int top);
static int hff_cb(void *arg, const char *name, int is_dir, uint64_t size) {
    hff_t *h = arg;
    (void)size;
    if (name[0] == '.') return 0;
    size_t nl = strlen(name);
    if (h->top && is_bucket_name(name)) {
        if (is_dir) {
            char sub[PATH_MAX + 300];
            snprintf(sub, sizeof(sub), "%.*s/%s", PATH_MAX - 1, h->dir, name);
            if (dir_has_frame_files_in(sub, 0)) h->found = 1;
        }
        return h->found;
    }
    if (is_dir) return 0;
    if ((nl > 4 && !strcasecmp(name + nl - 4, ".jpg")) || (nl > 5 && !strcasecmp(name + nl - 5, ".jpeg"))) h->found = 1;
    return h->found;
}
static int dir_has_frame_files_in(const char *dir, int top) {
    hff_t h = { dir, top, 0 };
    if (g_src->list(g_src->ctx, dir, hff_cb, &h) != 0) return 0;
    return h.found;
}

static int dir_has_frame_files(const char *dir) { return dir_has_frame_files_in(dir, 1); }

typedef struct {
    char *name;      /* file name */
    char *path;      /* dir/name */
    char *stem;      /* name without the plane extension (components only) */
    uint64_t size;
} fent_t;

static void fents_free(fent_t *v, size_t n) {
    for (size_t i = 0; i < n; i++) { free(v[i].name); free(v[i].path); free(v[i].stem); }
    free(v);
}

/* The regular files in dir: with ext, those whose names end in ext (their
 * stems recorded); with ext NULL, every file except dotfiles and specs.txt.
 * Sorted in natural order of name (frame files) or stem (planes). */
static int fent_cmp_name(const void *a, const void *b) {
    return name_cmp(((const fent_t *)a)->name, ((const fent_t *)b)->name);
}
static int fent_cmp_stem(const void *a, const void *b) {
    return name_cmp(((const fent_t *)a)->stem, ((const fent_t *)b)->stem);
}

typedef struct { const char *dir, *ext; int top; fent_t **vp; size_t *np, *capp; int err; } ldi_t;
static int list_dir_into(const char *dir, const char *ext, int top, fent_t **vp, size_t *np, size_t *capp);
static int ldi_cb(void *arg, const char *name, int is_dir, uint64_t size) {
    ldi_t *L = arg;
    size_t l = strlen(name), el = L->ext ? strlen(L->ext) : 0;
    if (name[0] == '.') return 0;
    if (L->top && is_bucket_name(name) && is_dir) {                /* a bucket folder: its pictures belong to the clip */
        char sub[PATH_MAX + 300];
        snprintf(sub, sizeof(sub), "%.*s/%s", PATH_MAX - 1, L->dir, name);
        int e = list_dir_into(sub, L->ext, 0, L->vp, L->np, L->capp);
        if (e) { L->err = e; return 1; }
        return 0;
    }
    if (is_dir) return 0;
    if (L->ext) {
        if (l <= el || strcasecmp(name + l - el, L->ext) != 0) return 0;
    } else if (!strcmp(name, SPECS_FILENAME)) {
        return 0;
    }
    char full[PATH_MAX + 300];
    snprintf(full, sizeof(full), "%.*s/%s", PATH_MAX - 1, L->dir, name);
    fent_t *v = *L->vp;
    size_t n = *L->np, cap = *L->capp;
    if (n == cap) {
        cap = cap ? cap * 2 : 1024;
        fent_t *nv = realloc(v, cap * sizeof(*v));
        if (!nv) { L->err = -ENOMEM; return 1; }
        v = nv;
        *L->vp = v;
        *L->capp = cap;
    }
    v[n].name = strdup(name);
    v[n].path = strdup(full);
    v[n].stem = NULL;
    v[n].size = size;
    if (L->ext) {
        v[n].stem = malloc(l - el + 1);
        if (v[n].stem) { memcpy(v[n].stem, name, l - el); v[n].stem[l - el] = 0; }
    }
    int bad = !v[n].name || !v[n].path || (L->ext && !v[n].stem);
    *L->np = n + 1;
    if (bad) { L->err = -ENOMEM; return 1; }
    return 0;
}

static int list_dir_into(const char *dir, const char *ext, int top, fent_t **vp, size_t *np, size_t *capp) {
    ldi_t L = { dir, ext, top, vp, np, capp, 0 };
    int e = g_src->list(g_src->ctx, dir, ldi_cb, &L);
    return L.err ? L.err : e;
}

static int list_dir(const char *dir, const char *ext, fent_t **out, size_t *count) {
    fent_t *v = NULL;
    size_t n = 0, cap = 0;
    *out = NULL;
    *count = 0;
    int e = list_dir_into(dir, ext, 1, &v, &n, &cap);
    if (e) { fents_free(v, n); return e; }
    if (n) qsort(v, n, sizeof(*v), ext ? fent_cmp_stem : fent_cmp_name);
    *out = v;
    *count = n;
    return 0;
}

/* ---------------------------------------------------------------- */
/* Matroska pieces (the same builders as mjpegstore -s)              */
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

/* ---------------------------------------------------------------- */
/* A clip: its pictures, the Matroska layout, and a small cache      */
/* ---------------------------------------------------------------- */

typedef struct {
    char *path;
    uint64_t size;
    uint64_t total;      /* bytes of picture data in the block (= size) */
} mj_frame_t;

typedef struct {
    uint64_t frame;      /* NO_FRAME (free), or the frame this slot holds or is loading */
    uint64_t stamp;      /* LRU tick; meaningful once loading == 0 */
    int loading;         /* 1 while a thread is fetching it: not readable yet, never evicted */
    unsigned char *buf;
    size_t cap;
} slot_t;
#define NO_FRAME UINT64_MAX
#define PREFETCH_MAX 2
#define CLUSTER_HDR 22   /* id 4 + size 8 + Timestamp element 10 */
#define BLOCK_HDR 9      /* SimpleBlock id 1 + size 4 + track 1 + time 2 + flags 1 */
#define MAX_PICTURE (200ull * 1000 * 1000)

typedef struct {
    char *name;                 /* virtual file name: "<clip>.mkv" */
    char *dir;                  /* absolute clip directory */
    time_t mtime;

    pthread_mutex_t mu;         /* guards indexing */
    int state;                  /* 0 = not indexed yet, 1 = ready, -1 = failed */

    unsigned char *head;        /* EBML header .. Tracks */
    size_t headn;
    unsigned char *cues;
    size_t cuesn;
    mj_frame_t *fr;
    uint64_t nframes;
    uint64_t *cum;              /* nframes + 1: offset of each frame's unit within the clusters */
    uint64_t *csz;              /* the size of each cluster, header included */
    uint64_t K;                 /* frames per cluster */
    uint64_t num, den;          /* frame rate */
    uint64_t clusters_total;
    uint64_t total;             /* size of the virtual file */

    pthread_mutex_t cache_mu;   /* guards the slots (not mu): reads of different frames overlap */
    pthread_cond_t cache_cv;
    slot_t *slots;
    unsigned nslots;
    uint64_t tick;
} clip_t;

static specs_t g_specs;
static clip_t *g_clips;
static size_t g_nclips;

#define TS_MS(c, i) (((uint64_t)(i) * 1000u * (c)->den + (c)->num / 2) / (c)->num)

/* ---------------------------------------------------------------- */
/* A small bounded pool, used only for prefetch jobs                 */
/* ---------------------------------------------------------------- */

typedef struct pool_job {
    void (*fn)(void *);
    void *arg;
    struct pool_job *next;
} pool_job_t;

static pthread_mutex_t g_pool_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pool_cv = PTHREAD_COND_INITIALIZER;
static pool_job_t *g_pool_head, *g_pool_tail;
static int g_pool_shutdown;

static void *pool_worker(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&g_pool_mu);
        while (!g_pool_head && !g_pool_shutdown) pthread_cond_wait(&g_pool_cv, &g_pool_mu);
        if (!g_pool_head) { pthread_mutex_unlock(&g_pool_mu); return NULL; }
        pool_job_t *j = g_pool_head;
        g_pool_head = j->next;
        if (!g_pool_head) g_pool_tail = NULL;
        pthread_mutex_unlock(&g_pool_mu);
        j->fn(j->arg);
        free(j);
    }
}

/* Threads spend their time blocked on pread(), not on CPU, so the pool is not sized
 * to the core count: a floor of 4 and a ceiling of 16 bound the background fetches. */
static void pool_init(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 4) n = 4;
    if (n > 16) n = 16;
    for (long i = 0; i < n; i++) {
        pthread_t th;
        if (pthread_create(&th, NULL, pool_worker, NULL) == 0) pthread_detach(th);
    }
}

static void pool_submit(void (*fn)(void *), void *arg) {
    pool_job_t *j = malloc(sizeof(*j));
    if (!j) return;
    j->fn = fn;
    j->arg = arg;
    j->next = NULL;
    pthread_mutex_lock(&g_pool_mu);
    if (g_pool_tail) g_pool_tail->next = j; else g_pool_head = j;
    g_pool_tail = j;
    pthread_cond_signal(&g_pool_cv);
    pthread_mutex_unlock(&g_pool_mu);
}

/* ---------------------------------------------------------------- */
/* Indexing                                                          */
/* ---------------------------------------------------------------- */

static int has_jpg_ext(const char *name) {
    size_t l = strlen(name);
    return (l > 4 && !strcasecmp(name + l - 4, ".jpg")) || (l > 5 && !strcasecmp(name + l - 5, ".jpeg"));
}

/* frame-0000007-1.jpg or -2.jpg: a field picture of an old interlaced set. */
static int is_field_name(const char *path) {
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    size_t l = strlen(b);
    if (l < 7 || strcasecmp(b + l - 4, ".jpg") != 0) return 0;
    char f = b[l - 5], dash = b[l - 6], dig = b[l - 7];
    return (f == '1' || f == '2') && dash == '-' && isdigit((unsigned char)dig);
}

/* The width and height in the first start-of-frame marker of a JPEG file. */
static int jpeg_file_dims(const char *path, int *w, int *h) {
    unsigned char b[1 << 16];
    int kind = 0;
    uint64_t sz = 0;
    int64_t mt = 0;
    if (g_src->stat(g_src->ctx, path, &kind, &sz, &mt) != 0 || kind != 1) return -1;
    ssize_t n = sz < sizeof(b) ? (ssize_t)sz : (ssize_t)sizeof(b);
    if (n > 0 && g_src->pread(g_src->ctx, path, b, (size_t)n, 0) != 0) return -1;
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

static void clip_free_index(clip_t *c) {
    if (c->fr) {
        for (uint64_t i = 0; i < c->nframes; i++) { free(c->fr[i].path); }
    }
    free(c->fr); free(c->cum); free(c->csz); free(c->head); free(c->cues);
    if (c->slots) { for (unsigned k = 0; k < c->nslots; k++) free(c->slots[k].buf); }
    free(c->slots);
    c->fr = NULL; c->cum = NULL; c->csz = NULL; c->head = NULL; c->cues = NULL; c->slots = NULL;
    c->nframes = 0; c->nslots = 0;
}

/* The Matroska header (EBML, Segment, SeekHead, Info, Tracks) and the Cues, from the
 * sizes in c->fr: byte for byte what mjpegstore -s writes. 0 or -1. */
static int build_layout(clip_t *c, const specs_t *sp) {
    const uint64_t num = c->num, den = c->den, nfr = c->nframes, K = c->K;
    const uint64_t ncl = (nfr + K - 1) / K;
    mb_t ebml = {0}, seg = {0}, seekh = {0}, info = {0}, tracks = {0}, cues = {0}, ch = {0};
    int rc = -1;

    c->csz = calloc((size_t)ncl, sizeof(*c->csz));
    c->cum = calloc((size_t)nfr + 1, sizeof(*c->cum));
    if (!c->csz || !c->cum) return -1;
    for (uint64_t k = 0; k < ncl; k++) {
        uint64_t s = CLUSTER_HDR;
        for (uint64_t i = k * K; i < nfr && i < (k + 1) * K; i++) {
            s += BLOCK_HDR + c->fr[i].total;
        }
        c->csz[k] = s;
    }
    /* the unit of a frame starts at its cluster's start (first frame) or at its block header */
    {
        uint64_t pos = 0;
        for (uint64_t k = 0; k < ncl; k++) {
            uint64_t p = pos;
            for (uint64_t i = k * K; i < nfr && i < (k + 1) * K; i++) {
                c->cum[i] = p;
                p += (i == k * K ? CLUSTER_HDR : 0) + BLOCK_HDR + c->fr[i].total;
            }
            pos += c->csz[k];
        }
        c->cum[nfr] = pos;
        c->clusters_total = pos;
    }

    el_uint(&ch, 0x2AD7B1, 1000000, 0);
    el_f64(&ch, 0x4489, (double)nfr * 1000.0 * (double)den / (double)num);
    el_str(&ch, 0x4D80, APPNAME);
    el_str(&ch, 0x5741, APPNAME);
    el_master(&info, 0x1549A966, &ch, 0);
    mb_free(&ch);

    {
        mb_t te = {0}, vid = {0}, col = {0}, tr = {0};
        el_uint(&te, 0xD7, 1, 0);
        el_uint(&te, 0x73C5, 1, 0);
        el_uint(&te, 0x83, 1, 0);
        el_uint(&te, 0x9C, 0, 0);
        el_str(&te, 0x86, "V_MJPEG");
        el_uint(&te, 0x23E383, (1000000000ull * den + num / 2) / num, 0);
        el_uint(&vid, 0x9A, 2, 0);          /* FlagInterlaced: progressive */
        el_uint(&vid, 0xB0, (uint64_t)sp->width, 0);
        el_uint(&vid, 0xBA, (uint64_t)sp->height, 0);
        el_uint(&vid, 0x54B0, ((uint64_t)sp->width * (uint64_t)sp->asp_num + (uint64_t)sp->asp_den / 2) / (uint64_t)sp->asp_den, 0);
        el_uint(&vid, 0x54BA, (uint64_t)sp->height, 0);
        el_uint(&col, 0x55B9, sp->range_full ? 2 : 1, 0);
        if (is_420(sp)) {
            el_uint(&col, 0x55B7, sp->chromasiting == CS_JPEG ? 2 : 1, 0);
            if (sp->chromasiting != CS_PALDV) el_uint(&col, 0x55B8, 2, 0);
        }
        el_master(&vid, 0x55B0, &col, 0);
        el_master(&te, 0xE0, &vid, 0);
        el_master(&tr, 0xAE, &te, 0);
        el_master(&tracks, 0x1654AE6B, &tr, 0);
        mb_free(&te); mb_free(&vid); mb_free(&col); mb_free(&tr);
    }

    seek_entry(&seekh, 0x1549A966, 0);
    seek_entry(&seekh, 0x1654AE6B, 0);
    seek_entry(&seekh, 0x1C53BB6B, 0);
    mb_t sh = {0};
    el_master(&sh, 0x114D9B74, &seekh, 0);
    const uint64_t S = sh.n, I = info.n, T = tracks.n;
    mb_free(&sh);
    mb_free(&seekh);

    {
        uint64_t pos = S + I + T;
        for (uint64_t k = 0; k < ncl; k++) {
            mb_t pt = {0}, ctp = {0}, cp = {0};
            el_uint(&pt, 0xB3, TS_MS(c, k * K), 8);
            el_uint(&ctp, 0xF7, 1, 1);
            el_uint(&ctp, 0xF1, pos, 8);
            el_master(&pt, 0xB7, &ctp, 0);
            el_master(&cp, 0xBB, &pt, 0);
            mb_put(&ch, cp.p, cp.n);
            mb_free(&pt); mb_free(&ctp); mb_free(&cp);
            pos += c->csz[k];
        }
        el_master(&cues, 0x1C53BB6B, &ch, 0);
        mb_free(&ch);
    }
    seek_entry(&ch, 0x1549A966, S);
    seek_entry(&ch, 0x1654AE6B, S + I);
    seek_entry(&ch, 0x1C53BB6B, S + I + T + c->clusters_total);
    el_master(&seekh, 0x114D9B74, &ch, 0);
    mb_free(&ch);
    if (seekh.n != S || info.bad || tracks.bad || cues.bad || seekh.bad) goto out;
    {
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
        mb_size(&seg, S + I + T + c->clusters_total + cues.n, 8);
    }
    {
        size_t hn = ebml.n + seg.n + seekh.n + info.n + tracks.n;
        c->head = malloc(hn);
        c->cues = malloc(cues.n);
        if (!c->head || !c->cues || ebml.bad || seg.bad) goto out;
        size_t o = 0;
        memcpy(c->head + o, ebml.p, ebml.n); o += ebml.n;
        memcpy(c->head + o, seg.p, seg.n); o += seg.n;
        memcpy(c->head + o, seekh.p, seekh.n); o += seekh.n;
        memcpy(c->head + o, info.p, info.n); o += info.n;
        memcpy(c->head + o, tracks.p, tracks.n);
        c->headn = hn;
        memcpy(c->cues, cues.p, cues.n);
        c->cuesn = cues.n;
    }
    rc = 0;
out:
    mb_free(&ebml); mb_free(&seg); mb_free(&seekh); mb_free(&info); mb_free(&tracks); mb_free(&cues); mb_free(&ch);
    return rc;
}

static int build_frames(clip_t *c) {
    fent_t *v = NULL;
    size_t n = 0;
    int e = list_dir(c->dir, NULL, &v, &n);
    if (e) { fprintf(stderr, NAME ": %s: cannot read %s: %s\n", c->name, c->dir, strerror(-e)); return -1; }
    size_t nj = 0, skipped = 0;
    for (size_t i = 0; i < n; i++) {
        if (has_jpg_ext(v[i].name)) v[nj++] = v[i];
        else { skipped++; free(v[i].name); free(v[i].path); free(v[i].stem); }
    }
    n = nj;
    if (skipped) fprintf(stderr, NAME ": %s: warning: %zu file%s without a .jpg extension %s ignored\n", c->name,
                         skipped, skipped == 1 ? "" : "s", skipped == 1 ? "is" : "are");
    if (n == 0) { fprintf(stderr, NAME ": %s: no .jpg files in %s\n", c->name, c->dir); fents_free(v, n); return -1; }

    c->fr = calloc(n, sizeof(*c->fr));
    if (!c->fr) { fents_free(v, n); return -1; }
    uint64_t nfr = 0;
    int rc = -1;
    for (size_t i = 0; i < n; i++) {
        mj_frame_t *f = &c->fr[nfr];
        if (is_field_name(v[i].path)) {
            fprintf(stderr, NAME ": %s: %s looks like one field of an interlaced frame. " NAME " serves progressive sets only: "
                            "deinterlace the footage and store progressive frames\n", c->name, v[i].path);
            goto done;
        }
        f->path = v[i].path; v[i].path = NULL;
        f->size = v[i].size;
        if (f->size < 4 || f->size > MAX_PICTURE) {
            fprintf(stderr, NAME ": %s: %s is %llu bytes; that is not a usable JPEG picture\n", c->name, f->path,
                    (unsigned long long)f->size);
            nfr++;
            goto done;
        }
        f->total = f->size;
        nfr++;
    }
    c->nframes = nfr;
    {   /* the pictures must be the size specs.txt says */
        int w = 0, h = 0;
        if (jpeg_file_dims(c->fr[0].path, &w, &h) != 0) {
            fprintf(stderr, NAME ": %s: %s is not a readable JPEG\n", c->name, c->fr[0].path);
            goto done;
        }
        if (w != g_specs.width || h != g_specs.height) {
            fprintf(stderr, NAME ": %s: %s is %dx%d, but specs.txt (%dx%d) needs it\n", c->name, c->fr[0].path, w, h,
                    g_specs.width, g_specs.height);
            goto done;
        }
    }
    rc = 0;
done:
    if (rc != 0) c->nframes = nfr;     /* so clip_free_index frees every path taken */
    fents_free(v, n);
    return rc;
}

/* Builds the index on first use. Caller holds c->mu. */
static int clip_index_locked(clip_t *c) {
    if (c->state) return c->state > 0 ? 0 : -1;
    c->num = (uint64_t)g_specs.fps_num;
    c->den = (uint64_t)g_specs.fps_den;
    c->K = (c->num + c->den / 2) / c->den;       /* frames per cluster: about a second */
    if (c->K < 1) c->K = 1;
    int rc = build_frames(c);
    if (rc == 0) rc = build_layout(c, &g_specs);
    if (rc == 0) {
        uint64_t sum = 0, big = 0;
        for (uint64_t i = 0; i < c->nframes; i++) { sum += c->fr[i].total; if (c->fr[i].total > big) big = c->fr[i].total; }
        uint64_t avg = sum / c->nframes + 1;
        uint64_t n = (48ull << 20) / avg;               /* a few pictures of cache, about 48 MiB at most */
        c->nslots = n < 4 ? 4 : n > 16 ? 16 : (unsigned)n;
        c->slots = calloc(c->nslots, sizeof(slot_t));
        if (!c->slots) rc = -1;
        else for (unsigned k = 0; k < c->nslots; k++) c->slots[k].frame = NO_FRAME;
    }
    if (rc != 0) {
        clip_free_index(c);
        c->state = -1;
        return -1;
    }
    c->total = c->headn + c->clusters_total + c->cuesn;
    c->state = 1;
    return 0;
}

static int clip_ensure(clip_t *c) {
    pthread_mutex_lock(&c->mu);
    int r = clip_index_locked(c);
    pthread_mutex_unlock(&c->mu);
    return r;
}

/* ---------------------------------------------------------------- */
/* Reading                                                           */
/* ---------------------------------------------------------------- */

/* Reads exactly n bytes at off from path. Returns 0 or -errno. */
static int pread_full(const char *path, void *buf, size_t n, uint64_t off) {
    return g_src->pread(g_src->ctx, path, buf, n, off);
}

/* Fills dst with the picture bytes of frame f . */
static int fill_frame(const clip_t *c, uint64_t f, unsigned char *dst) {
    const mj_frame_t *fr = &c->fr[f];
    int e = pread_full(fr->path, dst, (size_t)fr->size, 0);
    if (e) return e;
    if (fr->size < 2 || dst[0] != 0xFF || dst[1] != 0xD8) return -EIO;   /* not a JPEG */
    return 0;
}

/* Picks a slot to hold frame f: one already loading or holding it (*is_new = 0), the
 * least-recently-used slot not being loaded (*is_new = 1: the caller must fill it), or
 * none if every one is mid-load (returns 0). Caller holds cache_mu. */
static int pick_slot(clip_t *c, uint64_t f, unsigned *idx, int *is_new) {
    for (unsigned k = 0; k < c->nslots; k++) {
        if (c->slots[k].frame == f) { *idx = k; *is_new = 0; return 1; }
    }
    int have = 0;
    unsigned victim = 0;
    for (unsigned k = 0; k < c->nslots; k++) {
        if (c->slots[k].loading) continue;
        if (c->slots[k].frame == NO_FRAME) { victim = k; have = 1; }
        else if (!have || (c->slots[victim].frame != NO_FRAME && c->slots[k].stamp < c->slots[victim].stamp)) {
            victim = k; have = 1;
        }
    }
    if (!have) return 0;
    *idx = victim; *is_new = 1;
    return 1;
}

/* Makes sure slot idx can hold frame f's bytes. The slot is claimed (loading), so
 * nobody else touches its buffer. */
static int slot_reserve(clip_t *c, unsigned idx, uint64_t f) {
    slot_t *s = &c->slots[idx];
    size_t need = (size_t)c->fr[f].total;
    if (s->cap < need) {
        unsigned char *nb = realloc(s->buf, need);
        if (!nb) return -ENOMEM;
        s->buf = nb;
        s->cap = need;
    }
    return 0;
}

/* Copies n bytes of frame f's picture data, starting at offset `within`, into dst:
 * from the cache, waiting if another thread is already fetching it, or fetching it
 * itself. Copies only what was asked for. Returns 0, or a negative errno. */
static int read_frame_slice(clip_t *c, uint64_t f, uint64_t within, unsigned char *dst, size_t n) {
    pthread_mutex_lock(&c->cache_mu);
    for (;;) {
        unsigned idx; int is_new;
        if (!pick_slot(c, f, &idx, &is_new)) { pthread_cond_wait(&c->cache_cv, &c->cache_mu); continue; }
        if (!is_new && c->slots[idx].loading) { pthread_cond_wait(&c->cache_cv, &c->cache_mu); continue; }
        if (!is_new) {
            memcpy(dst, c->slots[idx].buf + within, n);
            c->slots[idx].stamp = ++c->tick;
            pthread_mutex_unlock(&c->cache_mu);
            return 0;
        }
        c->slots[idx].frame = f;
        c->slots[idx].loading = 1;
        pthread_mutex_unlock(&c->cache_mu);
        int e = slot_reserve(c, idx, f);
        if (!e) e = fill_frame(c, f, c->slots[idx].buf);
        pthread_mutex_lock(&c->cache_mu);
        if (e) {
            c->slots[idx].frame = NO_FRAME;
            c->slots[idx].loading = 0;
            pthread_cond_broadcast(&c->cache_cv);
            pthread_mutex_unlock(&c->cache_mu);
            return e;
        }
        memcpy(dst, c->slots[idx].buf + within, n);
        c->slots[idx].loading = 0;
        c->slots[idx].stamp = ++c->tick;
        pthread_cond_broadcast(&c->cache_cv);
        pthread_mutex_unlock(&c->cache_mu);
        return 0;
    }
}

typedef struct { clip_t *c; uint64_t frame; } prefetch_job_t;

/* Fetches one picture into the cache and discards the result. Gives up at once if
 * every slot is busy or it is already cached: a missed prefetch costs nothing. */
static void prefetch_thread_fn(void *arg) {
    prefetch_job_t *j = arg;
    clip_t *c = j->c;
    uint64_t f = j->frame;
    free(j);
    pthread_mutex_lock(&c->cache_mu);
    unsigned idx; int is_new;
    if (!pick_slot(c, f, &idx, &is_new) || !is_new) { pthread_mutex_unlock(&c->cache_mu); return; }
    c->slots[idx].frame = f;
    c->slots[idx].loading = 1;
    pthread_mutex_unlock(&c->cache_mu);
    int e = slot_reserve(c, idx, f);
    if (!e) e = fill_frame(c, f, c->slots[idx].buf);
    pthread_mutex_lock(&c->cache_mu);
    if (e) c->slots[idx].frame = NO_FRAME;
    else c->slots[idx].stamp = ++c->tick;
    c->slots[idx].loading = 0;
    pthread_cond_broadcast(&c->cache_cv);
    pthread_mutex_unlock(&c->cache_mu);
}

static void kick_prefetch(clip_t *c, uint64_t f) {
    if (f >= c->nframes) return;
    prefetch_job_t *j = malloc(sizeof(*j));
    if (!j) return;
    j->c = c;
    j->frame = f;
    pool_submit(prefetch_thread_fn, j);
}

/* The bytes in front of frame i's picture data: the Cluster header if it opens a
 * cluster, then the SimpleBlock header. Returns their count. */
static size_t frame_prefix(const clip_t *c, uint64_t i, unsigned char *out) {
    size_t o = 0;
    const uint64_t first = i - i % c->K;
    if (i == first) {
        mb_t hd = {0};
        mb_id(&hd, 0x1F43B675);
        mb_size(&hd, c->csz[i / c->K] - 12, 8);
        el_uint(&hd, 0xE7, TS_MS(c, first), 8);
        if (!hd.bad && hd.n == CLUSTER_HDR) { memcpy(out, hd.p, hd.n); o = hd.n; }
        mb_free(&hd);
        if (o != CLUSTER_HDR) return 0;
    }
    uint32_t bs = (uint32_t)(4 + c->fr[i].total) | 0x10000000u;
    out[o + 0] = 0xA3;
    out[o + 1] = (unsigned char)(bs >> 24); out[o + 2] = (unsigned char)(bs >> 16);
    out[o + 3] = (unsigned char)(bs >> 8);  out[o + 4] = (unsigned char)bs;
    out[o + 5] = 0x81;
    int16_t rel = (int16_t)(TS_MS(c, i) - TS_MS(c, first));
    out[o + 6] = (unsigned char)((uint16_t)rel >> 8); out[o + 7] = (unsigned char)rel;
    out[o + 8] = 0x80;                                   /* every JPEG is a key frame */
    return o + BLOCK_HDR;
}

/* read(2) semantics on the virtual file: bytes copied, 0 at end of file, or -errno. An
 * error partway through fails the whole read: FUSE would otherwise treat a short
 * read as end of file. */
static ssize_t clip_read(clip_t *c, char *out, size_t size, uint64_t off) {
    if (clip_ensure(c) != 0) return -EIO;
    if (off >= c->total) return 0;
    if (size > c->total - off) size = (size_t)(c->total - off);

    unsigned depth = c->nslots > PREFETCH_MAX + 1 ? PREFETCH_MAX : c->nslots > 2 ? 1 : 0;
    const uint64_t cl_end = c->headn + c->clusters_total;

    size_t done = 0;
    while (done < size) {
        uint64_t pos = off + done;
        if (pos < c->headn) {
            size_t n = (size_t)(c->headn - pos);
            if (n > size - done) n = size - done;
            memcpy(out + done, c->head + pos, n);
            done += n;
            continue;
        }
        if (pos >= cl_end) {
            size_t n = (size_t)(c->total - pos);
            if (n > size - done) n = size - done;
            memcpy(out + done, c->cues + (pos - cl_end), n);
            done += n;
            continue;
        }
        const uint64_t rel = pos - c->headn;
        /* the frame whose unit holds rel: largest i with cum[i] <= rel */
        uint64_t lo = 0, hi = c->nframes;
        while (hi - lo > 1) {
            uint64_t mid = lo + (hi - lo) / 2;
            if (c->cum[mid] <= rel) lo = mid; else hi = mid;
        }
        const uint64_t f = lo, within = rel - c->cum[f];
        unsigned char pre[CLUSTER_HDR + BLOCK_HDR];
        const size_t P = frame_prefix(c, f, pre);
        if (P == 0) return -EIO;
        if (within < P) {
            size_t n = (size_t)(P - within);
            if (n > size - done) n = size - done;
            memcpy(out + done, pre + within, n);
            done += n;
            continue;
        }
        const uint64_t d = within - P;
        size_t n = (size_t)(c->fr[f].total - d);
        if (n > size - done) n = size - done;
        int e = read_frame_slice(c, f, d, (unsigned char *)(out + done), n);
        if (e) {
            fprintf(stderr, NAME ": %s: reading frame %llu failed: %s\n", c->name, (unsigned long long)f, strerror(-e));
            return -EIO;
        }
        for (unsigned k = 1; k <= depth; k++) kick_prefetch(c, f + k);
        done += n;
    }
    return (ssize_t)done;
}

/* ---------------------------------------------------------------- */
/* Finding the clips                                                 */
/* ---------------------------------------------------------------- */

static int clip_cmp(const void *a, const void *b) {
    return name_cmp(((const clip_t *)a)->name, ((const clip_t *)b)->name);
}

static int add_clip(const char *dir, const char *base) {
    clip_t *nc = realloc(g_clips, (g_nclips + 1) * sizeof(clip_t));
    if (!nc) return -1;
    g_clips = nc;
    clip_t *c = &g_clips[g_nclips];
    memset(c, 0, sizeof(*c));
    size_t bl = strlen(base);
    c->name = malloc(bl + 5);
    c->dir = strdup(dir);
    if (!c->name || !c->dir) return -1;
    memcpy(c->name, base, bl);
    memcpy(c->name + bl, ".mkv", 5);
    {
        int kind = 0;
        uint64_t sz = 0;
        int64_t mt = 0;
        c->mtime = g_src->stat(g_src->ctx, dir, &kind, &sz, &mt) == 0 ? (time_t)mt : 0;
    }
    g_nclips++;
    return 0;
}

/* Called once the clip list is final: a mutex must not be moved (as qsort would) after
 * it has been initialized. */
static void init_clip_locks(void) {
    for (size_t i = 0; i < g_nclips; i++) {
        pthread_mutex_init(&g_clips[i].mu, NULL);
        pthread_mutex_init(&g_clips[i].cache_mu, NULL);
        pthread_cond_init(&g_clips[i].cache_cv, NULL);
    }
}

#define NO_SPECS_HINT \
    "  A read-only mount can't have a specs.txt generated, so it is required up front.\n" \
    "  Make one with `mjpegstore -c SETDIR` on a writable copy, or build the archive from a\n" \
    "  mjpegstore list: those begin with the set's specs.txt.\n"

/* specs.txt belongs in the set directory, never inside a clip. */
static void warn_clip_copy(const char *clipdir) {
    char p[PATH_MAX + 32];
    snprintf(p, sizeof(p), "%s/%s", clipdir, SPECS_FILENAME);
    if (path_exists(p))
        fprintf(stderr, NAME ": warning: ignoring %s -- specs.txt belongs in the set directory, "
                        "never inside a clip; this copy can be deleted\n", p);
}

typedef struct { const char *dir; int bad; } disc_t;
static int disc_cb(void *arg, const char *name, int is_dir, uint64_t size) {
    disc_t *D = arg;
    char sub[PATH_MAX + 300];
    (void)size;
    if (name[0] == '.' || !is_dir) return 0;
    snprintf(sub, sizeof(sub), "%.*s/%s", PATH_MAX - 1, D->dir, name);
    if (dir_has_frame_files(sub)) {
        warn_clip_copy(sub);
        if (add_clip(sub, name) != 0) { D->bad = 1; return 1; }
    } else {
        fprintf(stderr, NAME ": skipping %s: no picture files\n", sub);
    }
    return 0;
}

/* dir is a set (its own specs.txt, clips in subdirectories) or a single clip (specs.txt
 * in its parent). Refuses (returns -1, after saying why) unless a valid specs.txt is
 * found: without one the frame rate and size are unknown. */
static int discover_in(const char *dir) {
    char spath[PATH_MAX + 32], err[PATH_MAX + 600];
    /* A set or a clip is decided by what the directory contains, never by a specs.txt
     * sitting inside it. */
    int is_set = !dir_has_frame_files(dir);

    if (is_set) {
        snprintf(spath, sizeof(spath), "%s/%s", dir, SPECS_FILENAME);
        if (!path_exists(spath)) {
            fprintf(stderr, NAME ": refusing to mount: %s has no %s.\n"
                            "  It holds no picture files of its own, so it is not a clip; a set directory\n"
                            "  (specs.txt plus one subdirectory per clip) needs specs.txt.\n" NO_SPECS_HINT,
                    dir, SPECS_FILENAME);
            return -1;
        }
    } else {
        warn_clip_copy(dir);
        char parent[PATH_MAX];
        snprintf(parent, sizeof(parent), "%s", dir);
        char *slash = strrchr(parent, '/');
        if (slash == parent) parent[1] = 0; else if (slash) *slash = 0;
        snprintf(spath, sizeof(spath), "%s/%s", parent, SPECS_FILENAME);
        if (!path_exists(spath)) {
            fprintf(stderr, NAME ": refusing to mount: no %s for the clip %s (its parent %s has none).\n" NO_SPECS_HINT,
                    SPECS_FILENAME, dir, parent);
            return -1;
        }
    }
    if (specs_load(spath, &g_specs, err, sizeof(err)) != 0) {
        fprintf(stderr, NAME ": refusing to mount: %s\n", err);
        return -1;
    }
    {
        char cs[64] = "";
        if (is_420(&g_specs)) snprintf(cs, sizeof(cs), ", chroma siting %s", SITINGS[g_specs.chromasiting].name);
        fprintf(stderr, NAME ": %s: pixel aspect %ld:%ld%s, %s range, JPEG quality %d\n", spath,
                g_specs.asp_num, g_specs.asp_den,
                cs,
                g_specs.range_full ? "full" : "limited", g_specs.quality);
    }

    if (!is_set) {
        const char *base = strrchr(dir, '/');
        base = base && base[1] ? base + 1 : dir;
        if (add_clip(dir, base) != 0) return -1;
        init_clip_locks();
        return 0;
    }

    {
        disc_t D = { dir, 0 };
        int e = g_src->list(g_src->ctx, dir, disc_cb, &D);
        if (e) { fprintf(stderr, NAME ": cannot open %s: %s\n", dir, strerror(-e)); return -1; }
        if (D.bad) return -1;
    }
    if (!g_nclips) { fprintf(stderr, NAME ": no clips found in %s\n", dir); return -1; }
    qsort(g_clips, g_nclips, sizeof(clip_t), clip_cmp);
    init_clip_locks();
    return 0;
}

#ifndef MJPEGMOUNT_AS_LIBRARY
static int discover(const char *arg) {
    char dir[PATH_MAX];
    if (!realpath(arg, dir)) {     /* absolute: the daemon chdir()s to / */
        fprintf(stderr, NAME ": %s: %s\n", arg, strerror(errno));
        return -1;
    }
    return discover_in(dir);
}
#endif

static clip_t *find_clip(const char *path) {
    if (path[0] != '/' || !path[1]) return NULL;
    for (size_t i = 0; i < g_nclips; i++)
        if (!strcmp(path + 1, g_clips[i].name)) return &g_clips[i];
    return NULL;
}

#ifdef MJPEGMOUNT_AS_LIBRARY
/* ---------------------------------------------------------------- */
/* Library use (see mjpegmount.h): a set in any source, no FUSE       */
/* ---------------------------------------------------------------- */

void mjl_close(void);

typedef struct { const char *dir; int found; } probe_t;
static int probe_cb(void *arg, const char *name, int is_dir, uint64_t size) {
    probe_t *q = arg;
    char sub[PATH_MAX + 300];
    (void)size;
    if (name[0] == '.' || !is_dir) return 0;
    snprintf(sub, sizeof(sub), "%.*s/%s", PATH_MAX - 1, q->dir, name);
    if (dir_has_frame_files(sub)) { q->found = 1; return 1; }
    return 0;
}

int mjl_probe(const mj_source *src, const char *setdir) {
    char spath[PATH_MAX + 32];
    const mj_source *old = g_src;
    int ok = 0;
    g_src = src;
    snprintf(spath, sizeof(spath), "%s/%s", setdir, SPECS_FILENAME);
    if (path_exists(spath) && !dir_has_frame_files(setdir)) {
        probe_t q = { setdir, 0 };
        if (src->list(src->ctx, setdir, probe_cb, &q) == 0) ok = q.found;
    }
    g_src = old;
    return ok;
}

int mjl_open(const mj_source *src, const char *setdir, char *describe, size_t dn) {
    static int pool_started;
    if (!pool_started) { pool_init(); pool_started = 1; }          /* the prefetch threads, once per process */
    g_src = src;
    if (discover_in(setdir) != 0) { mjl_close(); return -1; }
    snprintf(describe, dn, "%dx%d %s, %ld:%ld fps", g_specs.width, g_specs.height,
             SAMPLINGS[g_specs.sampling].name, g_specs.fps_num, g_specs.fps_den);
    return 0;
}

void mjl_close(void) {
    for (size_t i = 0; i < g_nclips; i++) {
        clip_free_index(&g_clips[i]);
        free(g_clips[i].name);
        free(g_clips[i].dir);
    }
    free(g_clips);
    g_clips = NULL;
    g_nclips = 0;
    g_src = &g_posix;
}

size_t mjl_nclips(void) { return g_nclips; }
const char *mjl_clip_name(size_t i) { return i < g_nclips ? g_clips[i].name : NULL; }
void *mjl_find(const char *path) { return find_clip(path); }
int mjl_ready(void *c) { return clip_ensure(c) == 0 ? 0 : -EIO; }
uint64_t mjl_size(void *c) { return ((clip_t *)c)->total; }
int64_t mjl_mtime(void *c) { return (int64_t)((clip_t *)c)->mtime; }
ssize_t mjl_read(void *c, void *buf, size_t size, uint64_t off) { return clip_read(c, buf, size, off); }
#endif /* MJPEGMOUNT_AS_LIBRARY */

#ifndef MJPEGMOUNT_AS_LIBRARY
/* ---------------------------------------------------------------- */
/* FUSE (libfuse 3, high-level API)                                  */
/* ---------------------------------------------------------------- */

static int mj_getattr(const char *path, struct stat *st, struct fuse_file_info *fi) {
    (void)fi;
    memset(st, 0, sizeof(*st));
    st->st_uid = getuid();
    st->st_gid = getgid();
    if (!strcmp(path, "/")) {
        st->st_mode = S_IFDIR | 0555;
        st->st_nlink = 2;
        return 0;
    }
    clip_t *c = find_clip(path);
    if (!c) return -ENOENT;
    if (clip_ensure(c) != 0) return -EIO;
    st->st_mode = S_IFREG | 0444;
    st->st_nlink = 1;
    st->st_size = (off_t)c->total;
    st->st_mtime = st->st_atime = st->st_ctime = c->mtime;
    return 0;
}

static int mj_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t off,
                      struct fuse_file_info *fi, enum fuse_readdir_flags flags) {
    (void)off; (void)fi; (void)flags;
    if (strcmp(path, "/") != 0) return -ENOENT;
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    for (size_t i = 0; i < g_nclips; i++) filler(buf, g_clips[i].name, NULL, 0, 0);
    return 0;
}

static int mj_open(const char *path, struct fuse_file_info *fi) {
    clip_t *c = find_clip(path);
    if (!c) return -ENOENT;
    if ((fi->flags & O_ACCMODE) != O_RDONLY) return -EACCES;   /* read-only */
    return clip_ensure(c) == 0 ? 0 : -EIO;
}

static int mj_read(const char *path, char *buf, size_t size, off_t off, struct fuse_file_info *fi) {
    (void)fi;
    clip_t *c = find_clip(path);
    if (!c) return -ENOENT;
    if (off < 0) return -EINVAL;
    ssize_t r = clip_read(c, buf, size, (uint64_t)off);
    return (int)r;
}

static const struct fuse_operations mj_ops = {
    .getattr = mj_getattr,
    .readdir = mj_readdir,
    .open    = mj_open,
    .read    = mj_read,
};

static void usage(FILE *fp) {
    fprintf(fp,
        "usage: " NAME " [options] SETDIR MOUNTPOINT\n\n"
        "Mounts an mjpegstore set read-only. Every clip in SETDIR (a subdirectory of JPEG\n"
        "pictures from mjpegstore -i) appears as <clip>.mkv: the same bytes mjpegstore -s\n"
        "writes (one V_MJPEG track in Matroska), but as a seekable file.\n"
        "SETDIR may also be a single clip directory (specs.txt in its parent).\n"
        "It refuses to mount unless a valid specs.txt is found (it supplies the frame rate\n"
        "and size, and can't be generated on a read-only mount).\n"
        "Progressive sets only: interlaced footage must be deinterlaced before it is stored.\n\n"
        "Options are passed to libfuse:\n"
        "  -f          stay in the foreground (errors are printed to the terminal)\n"
        "  -d          foreground plus libfuse debug output\n"
        "  -o OPT,...  mount options, e.g. -o allow_other\n"
        "Unmount with: fusermount3 -u MOUNTPOINT\n");
}

int main(int argc, char *argv[]) {
    const char *setdir = NULL, *mnt = NULL;
    char **fargv = calloc((size_t)argc + 4, sizeof(char *));
    int fargc = 0;
    if (!fargv) return 1;
    fargv[fargc++] = argv[0];
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(stdout); return 0; }
        if (!strcmp(a, "-o")) {
            if (i + 1 >= argc) { usage(stderr); return 1; }
            fargv[fargc++] = argv[i];
            fargv[fargc++] = argv[++i];
        } else if (a[0] == '-' && a[1]) {
            fargv[fargc++] = argv[i];
        } else if (!setdir) {
            setdir = a;
        } else if (!mnt) {
            mnt = a;
        } else {
            usage(stderr);
            return 1;
        }
    }
    if (!setdir || !mnt) { usage(stderr); return 1; }

    pool_init();
    if (discover(setdir) != 0) return 1;
    fprintf(stderr, NAME ": %zu clip%s, %dx%d %s, %ld:%ld fps\n",
            g_nclips, g_nclips == 1 ? "" : "s", g_specs.width, g_specs.height,
            SAMPLINGS[g_specs.sampling].name, g_specs.fps_num, g_specs.fps_den);

    fargv[fargc++] = (char *)mnt;
    fargv[fargc++] = (char *)"-o";
    fargv[fargc++] = (char *)"ro";
    return fuse_main(fargc, fargv, &mj_ops, NULL);
}
#endif /* !MJPEGMOUNT_AS_LIBRARY */
