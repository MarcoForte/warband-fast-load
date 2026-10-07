// fopenhook: faster case-insensitive file opening.
//
// The macOS port opens files through a helper (FUN_100923954 in the 1.174
// binary) that first tries fopen() and, if that fails, rebuilds the path one
// component at a time, scanning each directory for a case-insensitive match.
// The engine probes several candidate paths per texture and sound, so this
// scan runs thousands of times over directories with 1000+ entries.  While
// loading we do the same resolution with hashed lookups in the cached
// directory listings; afterwards the original helper is used.
//
// Env: FASTSWAP_NOFOPEN disables this hook.

#include "patch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAME_FOPEN_ADDR 0x100923954ULL

int is_loading(void);
int dircache_find_ci(const char *dir, const char *name, const char **match);

typedef FILE *(*fopen_fn)(const char *path, const char *mode);
static fopen_fn original_fopen;
long fopenhook_resolved;

static FILE *fast_game_fopen(const char *path, const char *mode) {
    if (!is_loading()) return original_fopen(path, mode);
    FILE *f = fopen(path, mode);
    if (f || !path) return f;

    // Same walk as the original: "./a/b" for relative paths, "/a/b" for
    // absolute ones; an unmatched component is kept as-is, but only the last
    // component may be unmatched (so files can still be created).
    char rest[2048], built[2048];
    if (strlcpy(rest, path, sizeof rest) >= sizeof rest) return original_fopen(path, mode);
    char *cursor = rest;
    size_t len = 0;
    if (rest[0] == '/') {
        cursor++;
        built[0] = 0;
    } else {
        strcpy(built, ".");
        len = 1;
    }
    int missing = 0;
    for (char *comp; (comp = strsep(&cursor, "/"));) {
        if (missing) return NULL;
        const char *match = NULL;
        int r = dircache_find_ci(len ? built : "/", comp, &match);
        if (r < 0) return original_fopen(path, mode);  // unlistable dir or cache off: defer to original
        if (!r) {
            match = comp;
            missing = 1;
        }
        int n = snprintf(built + len, sizeof built - len, "/%s", match);
        if (n < 0 || (size_t)n >= sizeof built - len) return original_fopen(path, mode);
        len += n;
    }
    fopenhook_resolved++;
    return fopen(built, mode);
}

__attribute__((constructor)) static void fopenhook_install(void) {
    if (getenv("FASTSWAP_NOFOPEN")) return;
    original_fopen = (fopen_fn)patch_function(GAME_FOPEN_ADDR, (void *)fast_game_fopen);
}
