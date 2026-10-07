#pragma once
#include <stdint.h>

// Address in the running game of an (unslid) address from the 1.174 binary.
void *game_addr(uint64_t unslid);

// Redirect the game function at `unslid` to `replacement`.  Returns a pointer
// through which the original can still be called, or NULL if the function
// does not look as expected (e.g. a different game build) and was left alone.
void *patch_function(uint64_t unslid, void *replacement);
