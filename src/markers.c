// markers: timestamp user-visible events (music/sounds starting) in the trace,
// to measure when the menu is actually ready rather than when the engine logs
// "loading time".  Only records anything when FASTSWAP_TRACE is set.

typedef struct FMODSystem FMODSystem;
typedef struct FMODSound FMODSound;
typedef struct FMODChannel FMODChannel;

void trace_mark(const char *fmt, ...);

int fmod_playSound(FMODSystem *, int index, FMODSound *, _Bool paused, FMODChannel **)
    __asm("__ZN4FMOD6System9playSoundE17FMOD_CHANNELINDEXPNS_5SoundEbPPNS_7ChannelE");
int fmod_createStream(FMODSystem *, const char *name, unsigned mode, void *exinfo, FMODSound **)
    __asm("__ZN4FMOD6System12createStreamEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE");

static int mk_playSound(FMODSystem *s, int index, FMODSound *snd, _Bool paused, FMODChannel **ch) {
    int r = fmod_playSound(s, index, snd, paused, ch);
    static int n;
    if (++n <= 5) trace_mark("FMOD playSound #%d paused=%d", n, paused);
    return r;
}

static int mk_createStream(FMODSystem *s, const char *name, unsigned mode, void *exinfo, FMODSound **out) {
    trace_mark("FMOD createStream begin %s", name ? name : "?");
    int r = fmod_createStream(s, name, mode, exinfo, out);
    trace_mark("FMOD createStream end");
    return r;
}

__attribute__((used)) static struct { const void *repl, *orig; } interposers[]
    __attribute__((section("__DATA,__interpose"))) = {
    {(const void *)mk_playSound, (const void *)fmod_playSound},
    {(const void *)mk_createStream, (const void *)fmod_createStream},
};
