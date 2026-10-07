// texhook: fast low-resolution texture loads for Warband's on-demand textures.
//
// With use_ondemand_textures enabled the engine loads every texture at
// startup as a low-resolution placeholder: the texture loader
// (FUN_100904054(path, skip_mips, &gl_texture) in the 1.174 macOS binary)
// reads and parses the *whole* DDS file, then uploads only mip levels
// >= skip_mips (skip_mips = 7, so a 2048px texture becomes a 16px one).  The
// full-resolution version is loaded later by a background thread.  That is
// ~2.3 GB read and parsed at startup to upload a few KB per texture.
//
// We patch the loader's entry to jump here.  For DXT1/3/5 2D textures with
// skip_mips > 0 we read just the DDS header and the small mip levels at the
// end of the file and upload them exactly as the original would.  Anything
// else (full loads, TGA, uncompressed or cube/volume textures, errors) goes
// to the original function through a trampoline.
//
// Env: FASTSWAP_NOTEX disables this hook.

#include "patch.h"

#include <OpenGL/gl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach_time.h>

#define LOADER_ADDR 0x100904054ULL      // FUN_100904054 (unslid)
#define GAME_FOPEN_ADDR 0x100923954ULL  // fopen with case-insensitive fallback

typedef uint32_t (*loader_fn)(const char *path, uint32_t skip, uint32_t *tex);
typedef FILE *(*game_fopen_fn)(const char *path, const char *mode);

static loader_fn original_loader;
static FILE *passlog;  // FASTSWAP_TEXLOG: log textures handled by the original loader

static double now_ms(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}  // trampoline into the original function
static game_fopen_fn game_fopen;
long texhook_fast, texhook_passed;
long long texhook_bytes_read;

#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

static uint32_t level_size(uint32_t w, uint32_t h, uint32_t block) {
    return ((w + 3) / 4) * ((h + 3) / 4) * block;
}

static uint32_t fast_loader(const char *path, uint32_t skip, uint32_t *tex) {
    const char *dot = strrchr(path, '.');
    FILE *f = NULL;
    uint8_t *data = NULL;
    if (skip == 0 || !dot || strcmp(dot + 1, "dds") != 0) goto passthrough;
    if (!(f = game_fopen(path, "r"))) goto passthrough;

    // pread on the descriptor: stdio would refill a whole buffer per call.
    int fd = fileno(f);
    uint32_t h[32];
    if (pread(fd, h, sizeof h, 0) != sizeof h || h[0] != FOURCC('D', 'D', 'S', ' ')) goto passthrough;
    uint32_t height = h[3], width = h[4], depth = h[6], mipcount = h[7];
    uint32_t pf_flags = h[20], fourcc = h[21], caps2 = h[28];
    if (!(pf_flags & 4) || (caps2 & 0x200) || ((caps2 & 0x200000) && depth)) goto passthrough;
    GLenum format;
    uint32_t block;
    if (fourcc == FOURCC('D', 'X', 'T', '1')) format = 0x83f1, block = 8;
    else if (fourcc == FOURCC('D', 'X', 'T', '3')) format = 0x83f2, block = 16;
    else if (fourcc == FOURCC('D', 'X', 'T', '5')) format = 0x83f3, block = 16;
    else goto passthrough;

    // Same clamping as the original: nv_dds counts mipmaps excluding the base.
    uint32_t num_mips = mipcount ? mipcount - 1 : 0;
    uint32_t first = skip < num_mips ? skip : num_mips;
    if (first == 0) goto passthrough;

    uint32_t w = width, ht = height;
    long offset = 128;
    for (uint32_t i = 0; i < first; i++) {
        offset += level_size(w, ht, block);
        w = w > 1 ? w >> 1 : 1;
        ht = ht > 1 ? ht >> 1 : 1;
    }
    uint32_t lw[32], lh[32], lsize[32], n = 0;
    size_t total = 0;
    for (uint32_t lvl = first; lvl <= num_mips && n < 32; lvl++, n++) {
        lw[n] = w, lh[n] = ht, lsize[n] = level_size(w, ht, block);
        total += lsize[n];
        w = w > 1 ? w >> 1 : 1;
        ht = ht > 1 ? ht >> 1 : 1;
    }
    if (!(data = malloc(total)) || pread(fd, data, total, offset) != (ssize_t)total) goto passthrough;
    fclose(f);

    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, num_mips - first);
    const uint8_t *p = data;
    for (uint32_t i = 0; i < n; i++) {
        glCompressedTexImage2D(GL_TEXTURE_2D, i, format, lw[i], lh[i], 0, lsize[i], p);
        p += lsize[i];
    }
    free(data);
    *tex = t;
    texhook_fast++;
    texhook_bytes_read += 128 + total;
    return 0;

passthrough:
    if (f) fclose(f);
    free(data);
    texhook_passed++;
    extern int is_loading(void);
    extern void trace_mark(const char *fmt, ...);
    if (!is_loading()) trace_mark("full texture load: %s", path);
    if (!passlog) return original_loader(path, skip, tex);
    double t0 = now_ms();
    uint32_t r = original_loader(path, skip, tex);
    fprintf(passlog, "%.2f ms skip=%u %s\n", now_ms() - t0, skip, path);
    return r;
}

__attribute__((constructor)) static void texhook_install(void) {
    if (getenv("FASTSWAP_NOTEX")) return;
    const char *log = getenv("FASTSWAP_TEXLOG");
    if (log) passlog = fopen(log, "w");
    game_fopen = (game_fopen_fn)game_addr(GAME_FOPEN_ADDR);
    original_loader = (loader_fn)patch_function(LOADER_ADDR, (void *)fast_loader);
}
