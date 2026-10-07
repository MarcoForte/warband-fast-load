// loadstep: run loading steps in batches, and while waiting on the renderer.
//
// Startup loading is a state machine (FUN_1009e1750 in the 1.174 binary) that
// the engine calls once per frame and that does one unit of work per call:
// in state 2 one module resource (BRF), in state 4 one texture.  The frame
// loop is strictly alternating: at the start of each frame the main thread
// waits (WaitForEvent, FUN_100a454b4) until the render thread has executed
// the previous frame's commands -- GPU uploads, texture loads, and at startup
// compiling all shaders -- and only then runs the next loading step.
//
// 1. Batching: in states 2 and 4 each frame runs steps for up to
//    FASTSWAP_STEP_MS (default 30) ms, so the loading screen still redraws.
// 2. Overlap: when the main thread is about to wait for the render thread in
//    those states, it runs further loading steps until the render thread
//    signals, so CPU-side loading overlaps the render thread's work.  The
//    command queue the steps feed is locked, and the render thread drains it
//    in order, so commands are still executed in the original order.
//
// Env: FASTSWAP_NOBATCH disables both, FASTSWAP_NOOVERLAP only the overlap.

#include "patch.h"

#include <pthread.h>
#include <stdlib.h>
#include <mach/mach_time.h>

#define STEP_ADDR 0x1009e1750ULL
#define WAIT_ADDR 0x100a454b4ULL
#define FRAME_DONE_EVENT 0x103886dd8ULL  // global holding the render thread's "frame done" event
#define STATE_OFFSET 0x208               // int: current loading state

typedef void (*step_fn)(char *loader);
typedef int (*wait_fn)(void *event, long timeout_ms);
static step_fn original_step;
static wait_fn original_wait;
static char *loader;  // the loading state machine, captured on its first step
static double budget_ms = 30;
static int overlap = 1;
long loadstep_calls, loadstep_frames, loadstep_overlapped;

static double now_ms(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}

static int batchable(void) {
    int state = *(int *)(loader + STATE_OFFSET);
    return state == 2 || state == 4;
}

static void batched_step(char *l) {
    double start = now_ms();
    loader = l;
    loadstep_frames++;
    do {
        original_step(l);
        loadstep_calls++;
    } while (batchable() && now_ms() - start < budget_ms);
}

static int overlapped_wait(void *event, long timeout_ms) {
    if (overlap && loader && pthread_main_np() && event == *(void **)game_addr(FRAME_DONE_EVENT)) {
        // Poll the event (timeout 0 = try, returns 0 once signaled) and do a
        // loading step each time it is not signaled yet.
        while (batchable()) {
            if (original_wait(event, 0) == 0) return 0;
            original_step(loader);
            loadstep_calls++;
            loadstep_overlapped++;
        }
    }
    return original_wait(event, timeout_ms);
}

__attribute__((constructor)) static void loadstep_install(void) {
    if (getenv("FASTSWAP_NOBATCH")) return;
    const char *e = getenv("FASTSWAP_STEP_MS");
    if (e) budget_ms = atof(e);
    if (getenv("FASTSWAP_NOOVERLAP")) overlap = 0;
    original_step = (step_fn)patch_function(STEP_ADDR, (void *)batched_step);
    if (original_step) original_wait = (wait_fn)patch_function(WAIT_ADDR, (void *)overlapped_wait);
}
