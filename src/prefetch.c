// prefetch: warm the file cache for what startup loading will read.
//
// The engine reads its files one at a time on the critical path: ~450 MB of
// resource (BRF) and sound files, and the header and small end-of-file mip
// levels of ~3000 DDS textures (see texhook.c).  With a cold file cache
// (after a reboot, or once other data has pushed these files out) that is
// mostly waiting on the SSD, one request at a time.  At launch we instead
// ask the kernel to read ahead the BRF and sound files (F_RDADVISE, async)
// and read the texture headers/tails from several threads, at utility QoS,
// so by the time the engine needs a file it is usually in memory.  This only
// reads files; it changes nothing the game sees.
//
// Env: FASTSWAP_NOPREFETCH disables this.

#include <dirent.h>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

void trace_mark(const char *fmt, ...);

#define TAIL_BYTES 16384

typedef struct {
    char **paths;
    int n, cap;
} list_t;

static void add_dir(list_t *l, const char *dir, const char *const *exts) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot) continue;
        for (const char *const *x = exts; *x; x++) {
            if (strcasecmp(dot + 1, *x) != 0) continue;
            if (l->n == l->cap) l->paths = realloc(l->paths, (l->cap = l->cap ? l->cap * 2 : 1024) * sizeof *l->paths);
            asprintf(&l->paths[l->n++], "%s/%s", dir, e->d_name);
            break;
        }
    }
    closedir(d);
}

static void read_ahead(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) == 0) {
        struct radvisory ra = {.ra_offset = 0, .ra_count = (int)(st.st_size > 0x7fffffff ? 0x7fffffff : st.st_size)};
        fcntl(fd, F_RDADVISE, &ra);
    }
    close(fd);
}

static void read_dds_tail(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    char buf[TAIL_BYTES];
    struct stat st;
    pread(fd, buf, 128, 0);
    if (fstat(fd, &st) == 0 && st.st_size > 128)
        pread(fd, buf, TAIL_BYTES, st.st_size > TAIL_BYTES ? st.st_size - TAIL_BYTES : 0);
    close(fd);
}

static void *prefetch_main(void *arg) {
    (void)arg;
    // The executable is <install>/Mount and Blade.app/Contents/MacOS/<binary>.
    char exe[2048], base[2048], module[512] = "", path[4096];
    uint32_t size = sizeof exe;
    if (_NSGetExecutablePath(exe, &size) != 0 || !realpath(exe, base)) return NULL;
    for (int up = 0; up < 4; up++) {
        char *slash = strrchr(base, '/');
        if (!slash) return NULL;
        *slash = 0;
    }
    snprintf(path, sizeof path, "%s/Library/Application Support/MBWarband/last_module_warband", getenv("HOME"));
    FILE *f = fopen(path, "r");
    if (f) {
        if (fgets(module, sizeof module, f)) module[strcspn(module, "\r\n")] = 0;
        fclose(f);
    }
    if (!module[0]) return NULL;

    static const char *const brf_snd[] = {"brf", "ogg", "wav", "mp3", NULL}, *const dds[] = {"dds", NULL};
    list_t big = {0}, tex = {0};
    snprintf(path, sizeof path, "%s/Modules/%s/Resource", base, module);
    add_dir(&big, path, brf_snd);
    snprintf(path, sizeof path, "%s/CommonRes", base);
    add_dir(&big, path, brf_snd);
    snprintf(path, sizeof path, "%s/Modules/%s/Sounds", base, module);
    add_dir(&big, path, brf_snd);
    snprintf(path, sizeof path, "%s/Sounds", base);
    add_dir(&big, path, brf_snd);
    snprintf(path, sizeof path, "%s/Modules/%s/Textures", base, module);
    add_dir(&tex, path, dds);
    snprintf(path, sizeof path, "%s/Textures", base);
    add_dir(&tex, path, dds);

    for (int i = 0; i < big.n; i++) read_ahead(big.paths[i]);
    dispatch_queue_t q = dispatch_get_global_queue(QOS_CLASS_UTILITY, 0);
    dispatch_apply((size_t)tex.n, q, ^(size_t i) { read_dds_tail(tex.paths[i]); });
    trace_mark("prefetch done: %d resource/sound files advised, %d texture tails read", big.n, tex.n);
    return NULL;
}

__attribute__((constructor)) static void prefetch_start(void) {
    if (getenv("FASTSWAP_NOPREFETCH")) return;
    pthread_t t;
    if (pthread_create(&t, NULL, prefetch_main, NULL) == 0) pthread_detach(t);
}
