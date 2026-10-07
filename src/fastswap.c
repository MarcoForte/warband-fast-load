// fastswap: speed up Mount & Blade Warband (macOS) startup loading.
//
// Everything below is only active while the game is loading; as soon as the
// engine logs "loading time" (end of the startup load) every hook becomes a
// pass-through, so gameplay is unaffected.  A failsafe also ends loading mode
// FASTSWAP_MAX_S (default 180) seconds after launch.
//
// 1. Presents.  The engine renders one loading-screen frame per loading step
//    and every SDL_GL_SwapWindow blocks until the display's next refresh
//    (macOS enforces this for OpenGL surfaces regardless of the game's vsync
//    setting).  We drop presents arriving less than FASTSWAP_SKIP_MS (default
//    50) after the previous real one, so the loading screen still updates.
// 2. Directory listings.  The port finds files case-insensitively by listing
//    whole directories, ~40k times over the same few directories.  We cache
//    each listing on first use and serve later opendir/readdir from memory.
//
// Env:
//   FASTSWAP_SKIP_MS  min ms between real presents while loading (0 = off)
//   FASTSWAP_MAX_S    failsafe: stop loading mode after this many seconds
//   FASTSWAP_NODIR    disable the directory cache
//   FASTSWAP_TRACE    path of a file to log to (debugging)

#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <mach/mach_time.h>

typedef struct SDL_Window SDL_Window;
extern void SDL_GL_SwapWindow(SDL_Window *w);

// The game imports both the 64-bit-inode and the legacy directory functions.
DIR *opendir_legacy(const char *) __asm("_opendir");
struct dirent *readdir_legacy(DIR *) __asm("_readdir");

struct dirent_legacy {  // struct dirent without _DARWIN_FEATURE_64_BIT_INODE
    uint32_t d_ino;
    uint16_t d_reclen;
    uint8_t d_type;
    uint8_t d_namlen;
    char d_name[256];
};

static double now_ms(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}

static volatile int loading = 1;
static int dir_cache_on = 1;
static double start_ms, last_real_ms, load_end_ms, skip_ms = 50, max_ms = 180e3;
static FILE *trace;
static long n_skipped, n_dir_hits;

__attribute__((constructor)) static void fs_init(void) {
    const char *e;
    if (!start_ms) start_ms = now_ms();
    if ((e = getenv("FASTSWAP_SKIP_MS"))) skip_ms = atof(e);
    if ((e = getenv("FASTSWAP_MAX_S"))) max_ms = atof(e) * 1000;
    if (getenv("FASTSWAP_NODIR")) dir_cache_on = 0;
    if ((e = getenv("FASTSWAP_TRACE"))) trace = fopen(e, "w");
    if (trace) {  // wall-clock anchor, to line the trace up with the launcher's clock
        struct timeval tv;
        gettimeofday(&tv, NULL);
        fprintf(trace, "epoch %.3f at %.1f ms\n", tv.tv_sec + tv.tv_usec / 1e6, now_ms() - start_ms);
    }
}

int is_loading(void) {
    // Hooks can run (from other libraries' initializers) before fs_init.
    if (!start_ms) start_ms = now_ms();
    if (loading && now_ms() - start_ms > max_ms) loading = 0;
    return loading;
}

static void end_loading(void) {
    if (!loading) return;
    loading = 0;
    load_end_ms = now_ms();
    if (trace) {
        extern long texhook_fast, texhook_passed, loadstep_calls, loadstep_frames, loadstep_overlapped, fopenhook_resolved;
        extern long long texhook_bytes_read;
        fprintf(trace, "%.0f ms after launch: loading done, skipped %ld presents, %ld cached opendirs, "
                       "textures: %ld fast (%.1f MB read), %ld passed through, "
                       "loading steps: %ld in %ld frames (%ld overlapped), %ld case-insensitive opens\n",
                now_ms() - start_ms, n_skipped, n_dir_hits, texhook_fast, texhook_bytes_read / 1e6,
                texhook_passed, loadstep_calls, loadstep_frames, loadstep_overlapped, fopenhook_resolved);
        fflush(trace);
    }
}

// Timestamped event in the trace (only when FASTSWAP_TRACE is set).
void trace_mark(const char *fmt, ...) {
    if (!trace) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(trace, "%8.1f mark: ", now_ms() - start_ms);
    vfprintf(trace, fmt, ap);
    fputc('\n', trace);
    fflush(trace);
    va_end(ap);
}

// ---- 1. presents ----------------------------------------------------------

static void fs_SwapWindow(SDL_Window *w) {
    if (is_loading() && skip_ms > 0 && now_ms() - last_real_ms < skip_ms) {
        n_skipped++;
        return;
    }
    double t = now_ms();
    if (!loading) {
        extern void shaderdefer_drain(double budget_ms);
        shaderdefer_drain(4);  // compile shaders deferred during loading, a little per frame
    }
    SDL_GL_SwapWindow(w);
    if (trace && !loading) {  // frames after loading: the first few, then any stall
        static int n;
        if (++n <= 5 || t - last_real_ms > 50 || (getenv("FASTSWAP_TRACE_FRAMES") && t - load_end_ms < 1500))
            trace_mark("present #%d (%.0f ms since previous)", n, t - last_real_ms);
    }
    last_real_ms = now_ms();
}

// ---- 2. directory listing cache -------------------------------------------

typedef struct {
    uint64_t ino;
    uint8_t type;
    uint16_t namlen;
    char *name;
} entry_t;

typedef struct listing {
    struct listing *next;
    char *path;
    entry_t *ents;
    int n;
    int *ci_hash, ci_cap;  // case-insensitive name index, built on first lookup
} listing_t;

#define FAKE_MAGIC 0x46415354u  // "FAST"
typedef struct {
    uint32_t magic;
    const listing_t *l;
    int pos;
    struct dirent de;
    struct dirent_legacy del;
} fakedir_t;

static pthread_mutex_t dir_lock = PTHREAD_MUTEX_INITIALIZER;
static listing_t *listings;

// Fake handles are tracked so readdir/closedir can tell them from real DIR*s.
static fakedir_t **fakes;
static int n_fakes, cap_fakes;

static int is_fake(DIR *d) {
    int r = 0;
    pthread_mutex_lock(&dir_lock);
    for (int i = n_fakes - 1; i >= 0; i--)
        if ((DIR *)fakes[i] == d) { r = 1; break; }
    pthread_mutex_unlock(&dir_lock);
    return r;
}

static listing_t *find_listing(const char *path) {
    for (listing_t *l = listings; l; l = l->next)
        if (!strcmp(l->path, path)) return l;
    return NULL;
}

static listing_t *build_listing(const char *path) {
    DIR *d = opendir(path);
    if (!d) return NULL;
    listing_t *l = calloc(1, sizeof *l);
    int cap = 64;
    l->ents = malloc(cap * sizeof *l->ents);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (l->n == cap) l->ents = realloc(l->ents, (cap *= 2) * sizeof *l->ents);
        l->ents[l->n++] = (entry_t){e->d_ino, e->d_type, e->d_namlen, strndup(e->d_name, e->d_namlen)};
    }
    closedir(d);
    l->path = strdup(path);
    return l;
}

static DIR *cached_opendir(const char *path) {
    if (!dir_cache_on || !path || !is_loading()) return NULL;
    pthread_mutex_lock(&dir_lock);
    listing_t *l = find_listing(path);
    if (!l && (l = build_listing(path))) {
        l->next = listings;
        listings = l;
    }
    fakedir_t *f = NULL;
    if (l) {
        f = calloc(1, sizeof *f);
        f->magic = FAKE_MAGIC;
        f->l = l;
        if (n_fakes == cap_fakes) fakes = realloc(fakes, (cap_fakes = cap_fakes ? cap_fakes * 2 : 16) * sizeof *fakes);
        fakes[n_fakes++] = f;
        n_dir_hits++;
    }
    pthread_mutex_unlock(&dir_lock);
    return (DIR *)f;
}

static uint32_t ci_hash(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)tolower((unsigned char)*s)) * 16777619u;
    return h;
}

static void build_ci_index(listing_t *l) {
    l->ci_cap = 16;
    while (l->ci_cap < l->n * 2) l->ci_cap *= 2;
    l->ci_hash = calloc(l->ci_cap, sizeof *l->ci_hash);
    for (int i = 0; i < l->n; i++) {
        uint32_t h = ci_hash(l->ents[i].name) & (l->ci_cap - 1);
        int dup = 0;
        for (; l->ci_hash[h]; h = (h + 1) & (l->ci_cap - 1))
            if (!strcasecmp(l->ents[l->ci_hash[h] - 1].name, l->ents[i].name)) { dup = 1; break; }
        if (!dup) l->ci_hash[h] = i + 1;  // keep the first match, like a readdir scan would
    }
}

// Case-insensitive lookup of `name` in the (cached) listing of `dir`.
// Returns 1 and sets *match to the real name, 0 if absent, -1 if `dir`
// cannot be listed.  Only valid while loading; returns -1 otherwise.
int dircache_find_ci(const char *dir, const char *name, const char **match) {
    if (!dir_cache_on || !is_loading()) return -1;
    int r = 0;
    pthread_mutex_lock(&dir_lock);
    listing_t *l = find_listing(dir);
    if (!l && (l = build_listing(dir))) {
        l->next = listings;
        listings = l;
    }
    if (!l) {
        r = -1;
    } else {
        if (!l->ci_hash) build_ci_index(l);
        for (uint32_t h = ci_hash(name) & (l->ci_cap - 1); l->ci_hash[h]; h = (h + 1) & (l->ci_cap - 1)) {
            const entry_t *e = &l->ents[l->ci_hash[h] - 1];
            if (!strcasecmp(e->name, name)) { *match = e->name; r = 1; break; }
        }
    }
    pthread_mutex_unlock(&dir_lock);
    return r;
}

static const entry_t *fake_next(fakedir_t *f) {
    return f->pos < f->l->n ? &f->l->ents[f->pos++] : NULL;
}

static void fake_close(DIR *d) {
    pthread_mutex_lock(&dir_lock);
    for (int i = 0; i < n_fakes; i++)
        if ((DIR *)fakes[i] == d) { fakes[i] = fakes[--n_fakes]; break; }
    pthread_mutex_unlock(&dir_lock);
    free(d);
}

static DIR *fs_opendir(const char *p) {
    DIR *d = cached_opendir(p);
    return d ? d : opendir(p);
}

static DIR *fs_opendir_legacy(const char *p) {
    DIR *d = cached_opendir(p);
    return d ? d : opendir_legacy(p);
}

static struct dirent *fs_readdir(DIR *d) {
    if (!is_fake(d)) return readdir(d);
    fakedir_t *f = (fakedir_t *)d;
    const entry_t *e = fake_next(f);
    if (!e) return NULL;
    f->de.d_ino = e->ino;
    f->de.d_seekoff = f->pos;
    f->de.d_type = e->type;
    f->de.d_namlen = e->namlen;
    f->de.d_reclen = sizeof f->de;
    memcpy(f->de.d_name, e->name, e->namlen + 1);
    return &f->de;
}

static struct dirent *fs_readdir_legacy(DIR *d) {
    if (!is_fake(d)) return readdir_legacy(d);
    fakedir_t *f = (fakedir_t *)d;
    const entry_t *e;
    while ((e = fake_next(f)) && e->namlen > 255) {}  // can't be represented
    if (!e) return (struct dirent *)NULL;
    f->del.d_ino = (uint32_t)e->ino;
    f->del.d_type = e->type;
    f->del.d_namlen = (uint8_t)e->namlen;
    f->del.d_reclen = sizeof f->del;
    memcpy(f->del.d_name, e->name, e->namlen + 1);
    return (struct dirent *)&f->del;
}

static int fs_closedir(DIR *d) {
    if (!is_fake(d)) return closedir(d);
    fake_close(d);
    return 0;
}

// Relative paths are cached by string, so drop everything if cwd changes.
static int fs_chdir(const char *p) {
    int r = chdir(p);
    pthread_mutex_lock(&dir_lock);
    // Old listings stay allocated: open fake handles may still point at them.
    listings = NULL;
    pthread_mutex_unlock(&dir_lock);
    return r;
}

// ---- end-of-loading detection: watch the engine's log output ---------------

static void scan(const void *buf, size_t len) {
    if (!buf) return;
    if (!loading && (!trace || now_ms() - load_end_ms > 3000)) return;
    if (trace && len > 1) {  // timeline of the engine's log output while loading (and just after)
        int n = len < 70 ? (int)len : 70;
        fprintf(trace, "%8.1f log: %.*s\n", now_ms() - start_ms, n, (const char *)buf);
    }
    if (loading && memmem(buf, len, "loading time", 12)) end_loading();
}

static int fs_fputs(const char *s, FILE *f) {
    if (f != trace) scan(s, strlen(s));
    return fputs(s, f);
}

static size_t fs_fwrite(const void *p, size_t sz, size_t n, FILE *f) {
    if (f != trace) scan(p, sz * n);
    return fwrite(p, sz, n, f);
}

static int fs_fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    if (f != trace) {
        scan(fmt, strlen(fmt));
        if (loading && strchr(fmt, '%')) {
            char tmp[512];
            va_start(ap, fmt);
            vsnprintf(tmp, sizeof tmp, fmt, ap);
            va_end(ap);
            scan(tmp, strlen(tmp));
        }
    }
    va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

#define INTERPOSE(repl, orig) { (const void *)repl, (const void *)orig }
__attribute__((used)) static struct { const void *repl, *orig; } interposers[]
    __attribute__((section("__DATA,__interpose"))) = {
    INTERPOSE(fs_SwapWindow, SDL_GL_SwapWindow),
    INTERPOSE(fs_opendir, opendir),
    INTERPOSE(fs_opendir_legacy, opendir_legacy),
    INTERPOSE(fs_readdir, readdir),
    INTERPOSE(fs_readdir_legacy, readdir_legacy),
    INTERPOSE(fs_closedir, closedir),
    INTERPOSE(fs_chdir, chdir),
    INTERPOSE(fs_fputs, fputs),
    INTERPOSE(fs_fwrite, fwrite),
    INTERPOSE(fs_fprintf, fprintf),
};
