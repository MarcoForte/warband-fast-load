// takeover: let single-player scripts move the player into another agent.
//
// The script operation (player_control_agent, <player>, <agent>) only works
// in multiplayer: its argument check (FUN_100ae454c, "Invalid Player ID")
// fails unless a multiplayer game is running, and the engine function behind
// it indexes the multiplayer player table, which has no entry for the
// single-player hero (my player id is -1 there).
//
// We hook that argument check.  In single player, when the running opcode is
// player_control_agent, the first argument is taken as the agent to control
// and we do what the engine's "this is my player" branch does (FUN_100a44df0):
// make it the mission's player agent, mark it player-controlled, sync the
// camera and refresh first-person mesh visibility.  The check then reports
// failure so the multiplayer code path is skipped.  Used by the
// companion_takeover mod (mods/companion_takeover).
//
// Env: FASTSWAP_NOTAKEOVER disables this.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "patch.h"

void trace_mark(const char *fmt, ...);

#define OP_PLAYER_CONTROL_AGENT 421
#define AGENT_SIZE 0x94a0

static uint64_t (*orig_check_player)(uint32_t);

static char *agent_ptr(char *mission, int a) {
    if (a < 0 || a >= *(int *)(mission + 0xc)) return NULL;
    char *ag = *(char **)(*(char **)(mission + 0x28) + (uint32_t)(a >> 4) * 8) + (a & 0xf) * AGENT_SIZE;
    return *(int *)(ag + 8) ? ag : NULL;  // slot in use
}

static void take_control(int a) {
    char *mission = *(char **)game_addr(0x1018450c8);
    if (!mission) return;
    char *ag = agent_ptr(mission, a);
    int old = *(int *)(mission + 0xd0);
    if (!ag || a == old) return;
    char *old_ag = agent_ptr(mission, old);
    trace_mark("takeover: agent %d -> %d (old ctl %d pid %d, new ctl %d pid %d)", old, a,
               old_ag ? *(int *)(old_ag + 0x2a8) : -9, old_ag ? *(int *)(old_ag + 0x3c) : -9,
               *(int *)(ag + 0x2a8), *(int *)(ag + 0x3c));
    if (old_ag) {
        *(int *)(ag + 0x3c) = *(int *)(old_ag + 0x3c);
        *(int *)(old_ag + 0x2a8) = 0;  // no longer player-controlled
    }
    *(int *)(ag + 0x2a8) = 2;  // player-controlled
    *(int *)(mission + 0xd0) = a;
    memcpy(mission + 0xd7ec, ag + 0x44, 16);
    *(int *)(*(char **)game_addr(0x1018450c0) + 0x74c) = *(int *)(ag + 0x2d0);
    *(uint64_t *)(mission + 0x53bd0) = 0;
    *(uint64_t *)(mission + 0x59bd8) = 0;
    mission[0x4c0] = 1;
    ((void (*)(char *))game_addr(0x100946b5c))(mission);
}

static uint64_t check_player(uint32_t id) {
    int multiplayer = *(int *)game_addr(0x101464ca8);
    int opcode = *(int *)game_addr(0x103886d9c) & 0x0fffffff;
    if (!multiplayer && opcode == OP_PLAYER_CONTROL_AGENT) {
        take_control((int)id);
        return 0;
    }
    return orig_check_player(id);
}

__attribute__((constructor)) static void takeover_init(void) {
    if (getenv("FASTSWAP_NOTAKEOVER")) return;
    orig_check_player = patch_function(0x100AE454C, check_player);
}
