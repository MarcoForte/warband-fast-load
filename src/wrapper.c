// Stand-in for "Mount and Blade": runs the game with fastswap.dylib injected.
//
// Runs "Mount and Blade.patched" (hook entry points, see make_patched.py) when
// both it and the dylib are present -- the patched binary must not run
// without the dylib -- and otherwise the untouched "Mount and Blade.real".
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    char self[PATH_MAX], dir[PATH_MAX], real[PATH_MAX], patched[PATH_MAX], lib[PATH_MAX];
    uint32_t n = sizeof self;
    if (_NSGetExecutablePath(self, &n) != 0 || !realpath(self, dir)) return 1;
    *strrchr(dir, '/') = 0;
    snprintf(real, sizeof real, "%s/Mount and Blade.real", dir);
    snprintf(patched, sizeof patched, "%s/Mount and Blade.patched", dir);
    snprintf(lib, sizeof lib, "%s/fastswap.dylib", dir);

    char *target = real;
    if (access(lib, R_OK) == 0) {
        setenv("DYLD_INSERT_LIBRARIES", lib, 1);
        if (access(patched, X_OK) == 0) target = patched;
    }
    // Borderless fullscreen instead of a new Space: skips the ~0.4 s Space animation.
    setenv("SDL_VIDEO_MAC_FULLSCREEN_SPACES", "0", 0);
    argv[0] = target;
    execv(target, argv);
    perror("fastswap wrapper: execv");
    return 1;
}
